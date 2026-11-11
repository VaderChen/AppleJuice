// Windows Imaging Component（windowscodecs.dll）：IWICImagingFactory 與解碼、串流、格式轉換、縮放、點陣圖。
// 影像解碼沿用 D3DX／GDI+ 共用的 texture_image::decode（DDS 原生；PNG、JPEG、TIFF、GIF、BMP 經 macOS ImageIO），
// 有 DXGI 對應的像素格式轉換與縮放沿用 texture_image::transform；24bpp、預乘 alpha、灰階另行處理。
#include "service_callback_task.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/unicode.hpp"
#include "texture_load.hpp"
#include "wic_interfaces.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <map>
#include <memory>

namespace x64arm {
namespace {
using wic::Guid;using wic::guid;
constexpr std::uint32_t ok=0,s_false=1,pointer_error=0x80004003U,no_interface=0x80004002U,invalid=0x80070057U,out_of_memory=0x8007000eU,
    not_implemented=0x80004001U,fail=0x80004005U,no_aggregation=0x80040110U,class_not_registered=0x80040154U,
    wrong_state=0x88982f04U,unknown_format=0x88982f07U,not_initialized=0x88982f0cU,component_not_found=0x88982f50U,
    no_thumbnail=0x88982f44U,palette_unavailable=0x88982f45U,frame_missing=0x88982f62U,unsupported_pixel_format=0x88982f80U,
    unsupported_operation=0x88982f81U,insufficient_buffer=0x88982f8cU,already_locked=0x88982f0dU,access_denied=0x80030005U;
constexpr Guid iid_unknown=guid("00000000-0000-0000-c000-000000000046"),iid_class_factory=guid("00000001-0000-0000-c000-000000000046"),
    iid_factory=guid("ec5ec8a9-c395-4314-9c77-54d7a935ff70"),iid_factory2=guid("7b816b45-1996-4476-b132-de9e247c8af0"),
    iid_decoder=guid("9edde9e7-8dee-47ea-99df-e6faf2ed44bf"),iid_source=guid("00000120-a8f2-4877-ba0a-fd2b6645fb94"),
    iid_frame=guid("3b16811b-6a43-4ec9-a813-3d930c13b940"),iid_converter=guid("00000301-a8f2-4877-ba0a-fd2b6645fb94"),
    iid_scaler=guid("00000302-a8f2-4877-ba0a-fd2b6645fb94"),iid_bitmap=guid("00000121-a8f2-4877-ba0a-fd2b6645fb94"),
    iid_lock=guid("00000123-a8f2-4877-ba0a-fd2b6645fb94"),iid_stream=guid("135ff860-22b7-4ddf-b0f6-218f4f299a43"),
    iid_istream=guid("0000000c-0000-0000-c000-000000000046"),iid_sequential=guid("0c733a30-2a1c-11ce-ade5-00aa0044773d"),
    iid_component=guid("23bc3f0a-698b-4357-886b-f24d50671334"),iid_pixel_info=guid("e8eda601-3d48-431a-ab44-69059be88bbe"),
    iid_pixel_info2=guid("a9db33a2-af5f-43c7-b679-74f5984b5aa4");
// 容器格式（GetContainerFormat）。
constexpr Guid container_png=guid("1b7cfaf4-713f-473c-bbcd-6137425faeaf"),container_jpeg=guid("19e4a5aa-5662-4fc5-a0c0-1758028e1057"),
    container_bmp=guid("0af1d87e-fcfe-4188-bdeb-a7906471cbe3"),container_tiff=guid("163bcc30-e2e9-4f0b-961d-a3e9fdb788a3"),
    container_gif=guid("1f8a5601-7d4d-4cbd-9c82-1bc8d4eeb9a5"),container_dds=guid("9967cb95-2e85-4ac8-8ca2-83d7ccd425c9");
// 像素格式：GUID、每像素位元、通道數、對應 DXGI 格式（0＝無）、是否帶 alpha、是否預乘、數值表示（WICPixelFormatNumericRepresentation）。
struct PixelFormat {Guid id;std::uint32_t bits,channels,dxgi;bool alpha,premultiplied;std::uint32_t numeric;std::u16string_view name;};
constexpr PixelFormat formats[]{
    {guid("6fddc324-4e03-4bfe-b185-3d77768dc90f"),32,4,87,true,false,3,u"32bpp BGRA"},
    {guid("6fddc324-4e03-4bfe-b185-3d77768dc910"),32,4,0,true,true,3,u"32bpp PBGRA"},
    {guid("6fddc324-4e03-4bfe-b185-3d77768dc90e"),32,3,88,false,false,3,u"32bpp BGR"},
    {guid("f5c7ad2d-6a8d-43dd-a7a8-a29935261ae9"),32,4,28,true,false,3,u"32bpp RGBA"},
    {guid("3cc4a650-a527-4d37-a916-3142c7ebedba"),32,4,0,true,true,3,u"32bpp PRGBA"},
    {guid("d98c6b95-3efe-47d6-bb25-eb1748ab0cf1"),32,3,0,false,false,3,u"32bpp RGB"},
    {guid("6fddc324-4e03-4bfe-b185-3d77768dc90c"),24,3,0,false,false,3,u"24bpp BGR"},
    {guid("6fddc324-4e03-4bfe-b185-3d77768dc90d"),24,3,0,false,false,3,u"24bpp RGB"},
    {guid("6fddc324-4e03-4bfe-b185-3d77768dc908"),8,1,0,false,false,3,u"8bpp Gray"},
    {guid("e6cd0116-eeba-4161-aa85-27dd9fb3a895"),8,1,65,true,false,3,u"8bpp Alpha"},
    {guid("6fddc324-4e03-4bfe-b185-3d77768dc90b"),16,1,56,false,false,3,u"16bpp Gray"},
    {guid("6fddc324-4e03-4bfe-b185-3d77768dc90a"),16,3,85,false,false,3,u"16bpp BGR565"},
    {guid("05ec7c2b-f1e6-4961-ad46-e1cc810a87d2"),16,4,86,true,false,3,u"16bpp BGRA5551"},
    {guid("6fddc324-4e03-4bfe-b185-3d77768dc916"),64,4,11,true,false,3,u"64bpp RGBA"},
    {guid("6fddc324-4e03-4bfe-b185-3d77768dc93a"),64,4,10,true,false,4,u"64bpp RGBA Half"},
    {guid("6fddc324-4e03-4bfe-b185-3d77768dc919"),128,4,2,true,false,4,u"128bpp RGBA Float"},
    {guid("6fddc324-4e03-4bfe-b185-3d77768dc911"),32,1,41,false,false,4,u"32bpp Gray Float"},
    {guid("25238d72-fcf9-4522-b514-5578e5ad55e0"),32,4,24,true,false,3,u"32bpp RGBA1010102"},
};
const PixelFormat* find_format(const Guid& id){for(const auto& f:formats)if(f.id==id)return &f;return nullptr;}
constexpr Guid fmt_bgra=formats[0].id,fmt_bgr24=formats[6].id;

// 共用影像：緊密排列（stride＝ceil(寬×位元/8)），所有 BitmapSource 物件最終都化為此形式。
struct Image {std::uint32_t width{},height{};Guid format{};std::vector<std::uint8_t> bytes;double dpi_x{96},dpi_y{96};
    std::uint32_t stride() const {return (width*find_format(format)->bits+7)/8;}};

// 8 位元通道格式彼此轉換經 RGBA8（直通 alpha）中介；有 DXGI 對應的格式另走共用 transform。
std::vector<std::uint8_t> to_rgba8(const Image& image){
    const auto& f=*find_format(image.format);const std::size_t count=std::size_t(image.width)*image.height;std::vector<std::uint8_t> out(count*4,255);
    const auto* s=image.bytes.data();
    const auto unpremultiply=[](std::uint8_t c,std::uint8_t a){return a?static_cast<std::uint8_t>(std::min(255U,(unsigned(c)*255U+a/2U)/a)):std::uint8_t{0};};
    for(std::size_t i=0;i<count;++i){
        auto* d=out.data()+i*4;
        if(f.id==formats[0].id || f.id==formats[1].id || f.id==formats[2].id){d[0]=s[i*4+2];d[1]=s[i*4+1];d[2]=s[i*4];d[3]=f.id==formats[2].id?255:s[i*4+3];}
        else if(f.id==formats[3].id || f.id==formats[4].id || f.id==formats[5].id){d[0]=s[i*4];d[1]=s[i*4+1];d[2]=s[i*4+2];d[3]=f.id==formats[5].id?255:s[i*4+3];}
        else if(f.id==formats[6].id){d[0]=s[i*3+2];d[1]=s[i*3+1];d[2]=s[i*3];}
        else if(f.id==formats[7].id){d[0]=s[i*3];d[1]=s[i*3+1];d[2]=s[i*3+2];}
        else if(f.id==formats[8].id){d[0]=d[1]=d[2]=s[i];}
        else if(f.id==formats[9].id){d[0]=d[1]=d[2]=0;d[3]=s[i];}
        if(f.premultiplied)for(unsigned c=0;c<3;++c)d[c]=unpremultiply(d[c],d[3]);
    }
    return out;
}
std::vector<std::uint8_t> from_rgba8(std::span<const std::uint8_t> rgba,const PixelFormat& f,std::size_t count){
    std::vector<std::uint8_t> out(count*(f.bits/8));
    const auto premultiply=[](std::uint8_t c,std::uint8_t a){return static_cast<std::uint8_t>((unsigned(c)*a+127U)/255U);};
    for(std::size_t i=0;i<count;++i){
        const auto* s=rgba.data()+i*4;std::uint8_t r=s[0],g=s[1],b=s[2];const auto a=s[3];
        if(f.premultiplied){r=premultiply(r,a);g=premultiply(g,a);b=premultiply(b,a);}
        if(f.id==formats[0].id || f.id==formats[1].id || f.id==formats[2].id){auto* d=out.data()+i*4;d[0]=b;d[1]=g;d[2]=r;d[3]=f.id==formats[2].id?255:a;}
        else if(f.id==formats[3].id || f.id==formats[4].id || f.id==formats[5].id){auto* d=out.data()+i*4;d[0]=r;d[1]=g;d[2]=b;d[3]=f.id==formats[5].id?255:a;}
        else if(f.id==formats[6].id){auto* d=out.data()+i*3;d[0]=b;d[1]=g;d[2]=r;}
        else if(f.id==formats[7].id){auto* d=out.data()+i*3;d[0]=r;d[1]=g;d[2]=b;}
        else if(f.id==formats[8].id)out[i]=static_cast<std::uint8_t>((299U*r+587U*g+114U*b+500U)/1000U); // BT.601 亮度
        else if(f.id==formats[9].id)out[i]=a;
    }
    return out;
}
bool eight_bit(const PixelFormat& f){for(unsigned i=0;i<10;++i)if(formats[i].id==f.id)return true;return false;}
bool can_convert(const PixelFormat& from,const PixelFormat& to){
    if(from.id==to.id)return true;
    if((eight_bit(from) || from.dxgi) && (eight_bit(to) || to.dxgi))return true;
    return false;
}
Image convert(const Image& source,const PixelFormat& target){
    if(source.format==target.id)return source;
    const auto& from=*find_format(source.format);const std::size_t count=std::size_t(source.width)*source.height;
    Image out;out.width=source.width;out.height=source.height;out.format=target.id;out.dpi_x=source.dpi_x;out.dpi_y=source.dpi_y;
    const std::array<std::uint32_t,3> size{source.width,source.height,1};const std::array<std::uint32_t,6> box{0,0,0,source.width,source.height,1};
    if(from.dxgi && target.dxgi){out.bytes=texture_image::transform(source.bytes,from.dxgi,size,box,target.dxgi,size,1);return out;}
    // 一端沒有 DXGI 對應時經 RGBA8 中介（浮點、16 位元格式先轉成 R8G8B8A8_UNORM）。
    std::vector<std::uint8_t> rgba=eight_bit(from)?to_rgba8(source):texture_image::transform(source.bytes,from.dxgi,size,box,28,size,1);
    if(eight_bit(target))out.bytes=from_rgba8(rgba,target,count);
    else out.bytes=texture_image::transform(rgba,28,size,box,target.dxgi,size,1);
    return out;
}
// WICBitmapInterpolationMode → D3DX 濾鏡：最近鄰→POINT，其餘→LINEAR（Fant／Cubic 縮小時用 BOX 近似平均）。
Image scale(const Image& source,std::uint32_t width,std::uint32_t height,std::uint32_t mode){
    if(width==source.width && height==source.height)return source;
    const auto& f=*find_format(source.format);
    const bool shrink=width<source.width || height<source.height;const std::uint32_t filter=mode==0?2:(shrink && mode>=2)?5:3;
    Image working=f.dxgi?source:convert(source,*find_format(formats[3].id));const auto& wf=*find_format(working.format);
    Image out;out.width=width;out.height=height;out.format=working.format;out.dpi_x=source.dpi_x;out.dpi_y=source.dpi_y;
    out.bytes=texture_image::transform(working.bytes,wf.dxgi,{working.width,working.height,1},{0,0,0,working.width,working.height,1},wf.dxgi,{width,height,1},filter);
    return f.dxgi?out:convert(out,f);
}
// 解碼：依檔頭判斷容器，DDS 壓縮或非 8 位元格式統一展開成 32bpp BGRA；JPEG 回報 24bpp BGR，其餘 32bpp BGRA（與 WIC 內建解碼器相同）。
struct Decoded {Guid container;std::shared_ptr<const Image> frame;};
Decoded decode_bytes(std::span<const std::uint8_t> bytes){
    texture_image::Decoded image;
    try{image=texture_image::decode(bytes);}catch(const texture_image::Error&){throw unknown_format;}
    if(image.pixels.empty())throw unknown_format;
    const auto& d=image.desc;Image frame;frame.width=d.width;frame.height=d.height;
    const auto* native=d.format?[&]()->const PixelFormat*{for(const auto& f:formats)if(f.dxgi==d.format)return &f;return nullptr;}():nullptr;
    Guid container=image.file_format==4?container_dds:image.file_format==1?container_jpeg:image.file_format==3?container_png:image.file_format==10?container_tiff:image.file_format==11?container_gif:container_bmp;
    if(image.file_format==4 && native){frame.format=native->id;frame.bytes=std::move(image.pixels[0]);}
    else {
        // 非 DDS 的共用解碼結果是 R8G8B8A8（直通 alpha）；DDS 的壓縮格式先由共用轉換解壓。
        std::vector<std::uint8_t> rgba=d.format==28?std::move(image.pixels[0]):texture_image::transform(image.pixels[0],d.format,{d.width,d.height,1},{0,0,0,d.width,d.height,1},28,{d.width,d.height,1},1);
        const auto& target=*find_format(container==container_jpeg?fmt_bgr24:fmt_bgra);
        frame.format=target.id;frame.bytes=from_rgba8(rgba,target,std::size_t(d.width)*d.height);
    }
    return {container,std::make_shared<const Image>(std::move(frame))};
}

enum Kind:unsigned {Factory,ClassFactory,Stream,Decoder,Frame,Converter,Scaler,Bitmap,Lock,PixelInfo};
// wide：x86 上佔兩個堆疊槽的 8 位元組參數（LARGE_INTEGER、double）位元遮罩。
struct Method {std::string_view name;unsigned args;std::uint32_t wide{};};
// vtable 順序與 wincodec.h／objidl.h 相同（IUnknown 三個方法另外放在最前面）。
constexpr Method factory_methods[]{{"F_CreateDecoderFromFilename",6},{"F_CreateDecoderFromStream",5},{"F_CreateDecoderFromFileHandle",5},{"F_CreateComponentInfo",3},
    {"F_CreateDecoder",4},{"F_CreateEncoder",4},{"F_CreatePalette",2},{"F_CreateFormatConverter",2},{"F_CreateBitmapScaler",2},{"F_CreateBitmapClipper",2},
    {"F_CreateBitmapFlipRotator",2},{"F_CreateStream",2},{"F_CreateColorContext",2},{"F_CreateColorTransformer",2},{"F_CreateBitmap",6},{"F_CreateBitmapFromSource",4},
    {"F_CreateBitmapFromSourceRect",7},{"F_CreateBitmapFromMemory",8},{"F_CreateBitmapFromHBITMAP",5},{"F_CreateBitmapFromHICON",3},{"F_CreateComponentEnumerator",4},
    {"F_CreateFastMetadataEncoderFromDecoder",3},{"F_CreateFastMetadataEncoderFromFrameDecode",3},{"F_CreateQueryWriter",4},{"F_CreateQueryWriterFromReader",4},
    {"F_CreateImageEncoder",3}};
constexpr Method class_factory_methods[]{{"CF_CreateInstance",4},{"CF_LockServer",2}};
constexpr Method stream_methods[]{{"S_Read",4},{"S_Write",4},{"S_Seek",4,2},{"S_SetSize",2,2},{"S_CopyTo",5,4},{"S_Commit",2},{"S_Revert",1},{"S_LockRegion",4,6},
    {"S_UnlockRegion",4,6},{"S_Stat",3},{"S_Clone",2},{"S_InitializeFromIStream",2},{"S_InitializeFromFilename",3},{"S_InitializeFromMemory",3},{"S_InitializeFromIStreamRegion",4,12}};
constexpr Method decoder_methods[]{{"D_QueryCapability",3},{"D_Initialize",3},{"D_GetContainerFormat",2},{"D_GetDecoderInfo",2},{"D_CopyPalette",2},
    {"D_GetMetadataQueryReader",2},{"D_GetPreview",2},{"D_GetColorContexts",4},{"D_GetThumbnail",2},{"D_GetFrameCount",2},{"D_GetFrame",3}};
// IWICBitmapSource 共用的五個方法，之後接各介面自己的方法。
#define WIC_SOURCE_METHODS(P) {P "GetSize",3},{P "GetPixelFormat",2},{P "GetResolution",3},{P "CopyPalette",2},{P "CopyPixels",5}
constexpr Method frame_methods[]{WIC_SOURCE_METHODS("B_"),{"FR_GetMetadataQueryReader",2},{"FR_GetColorContexts",4},{"FR_GetThumbnail",2}};
constexpr Method converter_methods[]{WIC_SOURCE_METHODS("B_"),{"C_Initialize",7,32},{"C_CanConvert",4}};
constexpr Method scaler_methods[]{WIC_SOURCE_METHODS("B_"),{"SC_Initialize",5}};
constexpr Method bitmap_methods[]{WIC_SOURCE_METHODS("B_"),{"BM_Lock",4},{"BM_SetPalette",2},{"BM_SetResolution",3,6}};
#undef WIC_SOURCE_METHODS
constexpr Method lock_methods[]{{"L_GetSize",3},{"L_GetStride",2},{"L_GetDataPointer",3},{"L_GetPixelFormat",2}};
constexpr Method pixel_info_methods[]{{"P_GetComponentType",2},{"P_GetCLSID",2},{"P_GetSigningStatus",2},{"P_GetAuthor",4},{"P_GetVendorGUID",2},{"P_GetVersion",4},
    {"P_GetSpecVersion",4},{"P_GetFriendlyName",4},{"P_GetFormatGUID",2},{"P_GetColorContext",2},{"P_GetBitsPerPixel",2},{"P_GetChannelCount",2},
    {"P_GetChannelMask",4},{"P_SupportsTransparency",2},{"P_GetNumericRepresentation",2}};
constexpr Method common_methods[]{{"QueryInterface",3},{"AddRef",1},{"Release",1}};
std::span<const Method> methods(Kind k){
    switch(k){case Factory:return factory_methods;case ClassFactory:return class_factory_methods;case Stream:return stream_methods;case Decoder:return decoder_methods;
        case Frame:return frame_methods;case Converter:return converter_methods;case Scaler:return scaler_methods;case Bitmap:return bitmap_methods;case Lock:return lock_methods;
        default:return pixel_info_methods;}
}
}

struct WicState {
    struct Object {
        Kind kind{};std::uint32_t refs{1};
        // Stream：guest 記憶體（InitializeFromMemory）、宿主位元組（檔案）或另一個客體 IStream 的區段。
        std::uint64_t memory_base{},memory_size{},position{};std::shared_ptr<std::vector<std::uint8_t>> host_bytes;bool initialized{},writable{};
        std::uint64_t inner_stream{},region_offset{};
        // Decoder／Frame／Bitmap 等 BitmapSource 的影像；Converter／Scaler 保存來源物件與參數。
        Guid container{};std::shared_ptr<const Image> image;std::shared_ptr<Image> bitmap;std::uint64_t source{};Guid target{};std::uint32_t width{},height{},mode{};
        // Lock：所屬 Bitmap、鎖定區域、guest 緩衝與是否可寫。
        std::uint64_t owner{},data{};std::array<std::uint32_t,4> rect{};bool write{};
        const PixelFormat* info{};
    };
    std::map<std::uint64_t,Object> objects;std::map<Kind,std::uint64_t> tables;
};

std::span<const WindowsServiceDescriptor> wic_service_descriptors(){
    static const auto table=[] {
        std::vector<WindowsServiceDescriptor> result{{"windowscodecs.dll","DllGetClassObject",3},{"windowscodecs.dll","DllCanUnloadNow",0},
            {"windowscodecs.dll","WICConvertBitmapSource",3},{"windowscodecs.dll","WICCreateImagingFactory_Proxy",2}};
        for(const auto& m:common_methods)result.push_back({"internal-wic",m.name,m.args});
        for(unsigned k=0;k<=PixelInfo;++k)for(const auto& m:methods(static_cast<Kind>(k)))
            if(std::none_of(result.begin(),result.end(),[&](const auto& d){return d.family=="internal-wic" && d.symbol==m.name;}))result.push_back({"internal-wic",m.name,m.args,m.wide});
        return result;
    }();return table;
}

std::optional<std::uint32_t> WindowsServices::invoke_wic(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!wic_)wic_=std::make_shared<WicState>();auto& bridge=*wic_;
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const std::uint8_t bits=x86_?32:64;const unsigned unit=bits/8;
    struct PointerError {};
    const auto check=[&](std::uint64_t p,std::size_t n){if(!p)throw PointerError{};memory.check(GuestAddress{p},n,Access::Write);};
    const auto put=[&](std::uint64_t p,std::uint64_t value,std::uint8_t width=32){check(p,width/8);memory.store(GuestAddress{p},width,value);};
    const auto read_guid=[&](std::uint64_t p){if(!p)throw PointerError{};Guid g{};memory.read(GuestAddress{p},g);return g;};
    const auto write_guid=[&](std::uint64_t p,const Guid& g){check(p,16);memory.write(GuestAddress{p},g);};
    const auto table_for=[&](Kind k)->std::uint64_t{
        if(!bridge.tables.contains(k)){
            const auto extra=methods(k);const auto table=runtime_allocate(memory,{},(3+extra.size())*unit);
            for(unsigned i=0;i<3;++i)memory.store(GuestAddress{table+i*unit},bits,resolve("internal-wic",common_methods[i].name).value);
            for(std::size_t i=0;i<extra.size();++i)memory.store(GuestAddress{table+(i+3)*unit},bits,resolve("internal-wic",extra[i].name).value);
            bridge.tables.emplace(k,table);
        }
        return bridge.tables.at(k);
    };
    const auto create=[&](Kind k,WicState::Object object)->std::uint64_t{
        if(bridge.objects.size()>=65536)throw std::bad_alloc{};
        const auto table=table_for(k);const auto p=allocate_heap(memory,unit);if(!p)throw std::bad_alloc{};
        memory.store(GuestAddress{p},bits,table);object.kind=k;bridge.objects.emplace(p,std::move(object));return p;
    };
    // 參考歸零時釋放：可寫鎖把 guest 緩衝寫回點陣圖；轉換器、縮放器與鎖持有的來源參考一併釋放。
    std::function<void(std::uint64_t)> release=[&](std::uint64_t p){
        const auto found=bridge.objects.find(p);if(found==bridge.objects.end() || --found->second.refs)return;
        auto object=std::move(found->second);bridge.objects.erase(found);(void)free_heap(memory,p,state);
        if(object.kind==Lock && object.data){
            if(object.write)if(const auto owner=bridge.objects.find(object.owner);owner!=bridge.objects.end() && owner->second.bitmap){
                auto& image=*owner->second.bitmap;const auto& f=*find_format(image.format);const auto row=(object.rect[2]*f.bits+7)/8,stride=image.stride();
                for(std::uint32_t y=0;y<object.rect[3];++y)memory.read(GuestAddress{object.data+std::uint64_t(y)*row},std::span(image.bytes).subspan(std::size_t(object.rect[1]+y)*stride+(object.rect[0]*f.bits)/8,row));
            }
            (void)free_heap(memory,object.data,state);
        }
        if(object.source)release(object.source);
        if(object.owner)release(object.owner);
    };
    const auto addref=[&](std::uint64_t p){if(const auto found=bridge.objects.find(p);found!=bridge.objects.end() && found->second.refs<UINT32_MAX)++found->second.refs;};
    // BitmapSource 化為影像：轉換器與縮放器每次依目前來源重新計算（來源是可修改的 IWICBitmap 時保持一致）。
    std::function<std::shared_ptr<const Image>(std::uint64_t)> materialize=[&](std::uint64_t p)->std::shared_ptr<const Image>{
        const auto found=bridge.objects.find(p);if(found==bridge.objects.end())throw std::uint32_t{invalid};
        auto& o=found->second;
        switch(o.kind){
            case Frame:return o.image;
            case Bitmap:{
                // 可寫鎖定中的像素在 Lock 釋放前寫回；讀取時先同步目前的 guest 緩衝。
                for(auto& entry:bridge.objects)if(auto& lock=entry.second;lock.kind==Lock && lock.owner==p && lock.write && lock.data){
                    const auto& f=*find_format(o.bitmap->format);const auto row=(lock.rect[2]*f.bits+7)/8,stride=o.bitmap->stride();
                    for(std::uint32_t y=0;y<lock.rect[3];++y)memory.read(GuestAddress{lock.data+std::uint64_t(y)*row},std::span(o.bitmap->bytes).subspan(std::size_t(lock.rect[1]+y)*stride+(lock.rect[0]*f.bits)/8,row));
                }
                return o.bitmap;}
            case Converter:{if(!o.source)throw std::uint32_t{not_initialized};return std::make_shared<const Image>(convert(*materialize(o.source),*find_format(o.target)));}
            case Scaler:{if(!o.source)throw std::uint32_t{not_initialized};return std::make_shared<const Image>(scale(*materialize(o.source),o.width,o.height,o.mode));}
            default:throw std::uint32_t{no_interface};
        }
    };
    const auto read_path=[&](std::uint64_t p)->std::shared_ptr<std::vector<std::uint8_t>>{
        if(!p)throw PointerError{};const auto path=read_utf16(memory,GuestAddress{p},32768);if(path.empty())throw std::uint32_t{invalid};
        const auto opened=filesystem_.open_reader(utf16_to_utf8(normalized_windows_path(path,runtime_.current_directory)));
        if(opened.error)throw std::uint32_t{0x80070000U|(opened.error&0xffffU)};
        const auto info=opened.file->information();if(info.error)throw std::uint32_t{0x80070000U|(info.error&0xffffU)};
        if(info.metadata.size>(UINT64_C(1)<<30))throw std::bad_alloc{};
        auto bytes=std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(info.metadata.size));const auto io=opened.file->read(*bytes,0);
        if(io.error || io.bytes!=bytes->size())throw std::uint32_t{0x80070000U|(io.error?io.error&0xffffU:30U)};
        return bytes;
    };
    // 本家族的串流可直接取得剩餘內容；其他客體 IStream 由協程逐段呼叫其 Read（從目前位置讀到結尾）。
    const auto own_stream_bytes=[&](std::uint64_t p,std::vector<std::uint8_t>& out)->bool{
        const auto found=bridge.objects.find(p);if(found==bridge.objects.end() || found->second.kind!=Stream)return false;
        auto& s=found->second;if(!s.initialized)throw std::uint32_t{wrong_state};
        if(s.inner_stream)return false;
        const auto size=s.host_bytes?s.host_bytes->size():s.memory_size;const auto start=std::min<std::uint64_t>(s.position,size);out.resize(static_cast<std::size_t>(size-start));
        if(s.host_bytes)std::copy(s.host_bytes->begin()+static_cast<std::ptrdiff_t>(start),s.host_bytes->end(),out.begin());else memory.read(GuestAddress{s.memory_base+start},out);
        s.position=size;return true;
    };
    const auto make_decoder=[&](std::span<const std::uint8_t> bytes){
        auto decoded=decode_bytes(bytes);WicState::Object object;object.container=decoded.container;object.image=decoded.frame;object.initialized=true;return create(Decoder,std::move(object));
    };
    const auto foreign_read=[&](std::uint64_t stream,std::uint64_t output,unsigned output_index_for_decoder,bool initialize_decoder)->std::optional<std::uint32_t>{
        // 由協程讀完客體串流後建立解碼器（CreateDecoderFromStream）或初始化既有解碼器（IWICBitmapDecoder::Initialize）。
        (void)output_index_for_decoder;
        return service_callback::invoke<std::optional<std::uint32_t>>(this,threads_.current,state,memory,"WicStreamRead",
            [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);},
            [this,service,stream,output,initialize_decoder](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(state,memory,service);const std::uint8_t bits=x86_?32:64;const unsigned unit=bits/8;
                constexpr std::uint32_t chunk=1U<<16;const auto buffer=allocate_heap(memory,chunk+8);
                std::uint32_t result=ok;std::vector<std::uint8_t> bytes;
                if(!buffer)result=out_of_memory;
                else{
                    const auto vtable=memory.load(GuestAddress{stream},bits),read=memory.load(GuestAddress{vtable+3*unit},bits);
                    for(;;){
                        memory.store(GuestAddress{buffer+chunk},32,0);
                        const auto hr=static_cast<std::uint32_t>(co_await service_callback::call(read,{stream,buffer,chunk,buffer+chunk}));
                        const auto got=static_cast<std::uint32_t>(memory.load(GuestAddress{buffer+chunk},32));
                        if(hr!=ok && hr!=s_false){result=hr;break;}
                        if(got){const auto at=bytes.size();if(at+got>(std::size_t(1)<<30)){result=out_of_memory;break;}bytes.resize(at+got);memory.read(GuestAddress{buffer},std::span(bytes).subspan(at,got));}
                        if(!got || hr==s_false || got<chunk)break;
                    }
                    (void)free_heap(memory,buffer,state);
                }
                if(result==ok){
                    try{
                        auto decoded=decode_bytes(bytes);auto& bridge=*wic_;
                        if(initialize_decoder){auto& object=bridge.objects.at(call(0));object.container=decoded.container;object.image=decoded.frame;object.initialized=true;}
                        else {
                            WicState::Object object;object.kind=Decoder;object.container=decoded.container;object.image=decoded.frame;object.initialized=true;
                            if(!bridge.tables.contains(Decoder))result=fail;
                            else{const auto p=allocate_heap(memory,unit);if(!p)result=out_of_memory;else{memory.store(GuestAddress{p},bits,bridge.tables.at(Decoder));bridge.objects.emplace(p,std::move(object));memory.store(GuestAddress{output},bits,p);}}
                        }
                    }catch(std::uint32_t error){result=error;}catch(const std::bad_alloc&){result=out_of_memory;}
                }
                call.return_to(state,result);co_return std::optional<std::uint32_t>{};
            });
    };
    std::uint32_t result=ok;
    try{
        if(name=="CoCreateInstance" || name=="CoGetClassObject" || name=="DllGetClassObject" || name=="WICCreateImagingFactory_Proxy"){
            // WIC 為 Both 執行緒模型的 in-process 元件；IClassFactory 與工廠介面都由本家族提供。
            if(name=="WICCreateImagingFactory_Proxy"){put(arg(1),0,bits);put(arg(1),create(Factory,{}),bits);}
            else{
                const bool dll=name=="DllGetClassObject",factory_object=name!="CoCreateInstance";const unsigned output=dll?2:4;put(arg(output),0,bits);
                const auto cls=read_guid(arg(0)),iid=read_guid(arg(dll?1:3));
                if(cls!=wic::clsid_factory && cls!=wic::clsid_factory2)result=dll?0x80040111U:class_not_registered;
                else if(!dll && !(static_cast<std::uint32_t>(arg(factory_object?1:2))&1U))result=class_not_registered;
                else if(!factory_object && arg(1))result=no_aggregation;
                else if(factory_object){if(iid!=iid_unknown && iid!=iid_class_factory)result=no_interface;else put(arg(output),create(ClassFactory,{}),bits);}
                else if(iid!=iid_unknown && iid!=iid_factory && iid!=iid_factory2)result=no_interface;
                else put(arg(output),create(Factory,{}),bits);
            }
        }else if(name=="DllCanUnloadNow")result=s_false;
        else if(name=="WICConvertBitmapSource"){
            put(arg(2),0,bits);const auto* target=find_format(read_guid(arg(0)));
            if(!target)result=unsupported_pixel_format;
            else if(!bridge.objects.contains(arg(1)))result=invalid;
            else {const auto source=materialize(arg(1));if(!can_convert(*find_format(source->format),*target))result=unsupported_pixel_format;
                else{WicState::Object object;object.source=arg(1);object.target=target->id;addref(arg(1));put(arg(2),create(Converter,std::move(object)),bits);}}
        }else{
            const auto found=bridge.objects.find(arg(0));if(found==bridge.objects.end())throw PointerError{};
            auto& o=found->second;const auto kind=o.kind;
            if(name=="QueryInterface"){
                put(arg(2),0,bits);const auto iid=read_guid(arg(1));bool match=iid==iid_unknown;
                switch(kind){
                    case Factory:match|=iid==iid_factory || iid==iid_factory2;break;
                    case ClassFactory:match|=iid==iid_class_factory;break;
                    case Stream:match|=iid==iid_stream || iid==iid_istream || iid==iid_sequential;break;
                    case Decoder:match|=iid==iid_decoder;break;
                    case Frame:match|=iid==iid_source || iid==iid_frame;break;
                    case Converter:match|=iid==iid_source || iid==iid_converter;break;
                    case Scaler:match|=iid==iid_source || iid==iid_scaler;break;
                    case Bitmap:match|=iid==iid_source || iid==iid_bitmap;break;
                    case Lock:match|=iid==iid_lock;break;
                    case PixelInfo:match|=iid==iid_component || iid==iid_pixel_info || iid==iid_pixel_info2;break;
                }
                if(match){addref(arg(0));put(arg(2),arg(0),bits);}else result=no_interface;
            }else if(name=="AddRef"){addref(arg(0));result=o.refs;}
            else if(name=="Release"){const auto left=o.refs-1;release(arg(0));result=left;}
            else if(kind==ClassFactory){
                if(name=="CF_LockServer")result=ok;
                else{put(arg(3),0,bits);const auto iid=read_guid(arg(2));if(arg(1))result=no_aggregation;else if(iid!=iid_unknown && iid!=iid_factory && iid!=iid_factory2)result=no_interface;else put(arg(3),create(Factory,{}),bits);}
            }else if(kind==Factory){
                if(name=="F_CreateDecoderFromFilename"){
                    put(arg(5),0,bits);const auto access=static_cast<std::uint32_t>(arg(3));
                    if(access&0x40000000U)result=invalid; // 解碼器只讀
                    else{const auto bytes=read_path(arg(1));put(arg(5),make_decoder(*bytes),bits);}
                }else if(name=="F_CreateDecoderFromStream"){
                    put(arg(4),0,bits);if(!arg(1))throw PointerError{};std::vector<std::uint8_t> bytes;
                    if(own_stream_bytes(arg(1),bytes))put(arg(4),make_decoder(bytes),bits);
                    else{(void)table_for(Decoder); // 協程完成時建立解碼器，先確保 vtable 存在
                        return foreign_read(arg(1),arg(4),4,false);}
                }else if(name=="F_CreateDecoderFromFileHandle"){
                    put(arg(4),0,bits);result=not_implemented; // 需要以 handle 讀取檔案；目前沒有呼叫端使用。
                }else if(name=="F_CreateDecoder"){
                    put(arg(3),0,bits);const auto container=read_guid(arg(1));
                    if(container!=container_png && container!=container_jpeg && container!=container_bmp && container!=container_tiff && container!=container_gif && container!=container_dds)result=component_not_found;
                    else{WicState::Object object;object.container=container;put(arg(3),create(Decoder,std::move(object)),bits);}
                }else if(name=="F_CreateComponentInfo"){
                    put(arg(2),0,bits);const auto* info=find_format(read_guid(arg(1)));
                    if(!info)result=component_not_found;else{WicState::Object object;object.info=info;put(arg(2),create(PixelInfo,std::move(object)),bits);}
                }else if(name=="F_CreateFormatConverter"){put(arg(1),0,bits);put(arg(1),create(Converter,{}),bits);}
                else if(name=="F_CreateBitmapScaler"){put(arg(1),0,bits);put(arg(1),create(Scaler,{}),bits);}
                else if(name=="F_CreateStream"){put(arg(1),0,bits);put(arg(1),create(Stream,{}),bits);}
                else if(name=="F_CreateBitmap" || name=="F_CreateBitmapFromMemory"){
                    const bool from_memory=name=="F_CreateBitmapFromMemory";const unsigned output=from_memory?7:5;put(arg(output),0,bits);
                    const auto width=static_cast<std::uint32_t>(arg(1)),height=static_cast<std::uint32_t>(arg(2));const auto* f=find_format(read_guid(arg(3)));
                    if(!f)result=unsupported_pixel_format;
                    else if(!width || !height || std::uint64_t(width)*height>(UINT64_C(1)<<28))result=invalid;
                    else{
                        auto image=std::make_shared<Image>();image->width=width;image->height=height;image->format=f->id;const auto stride=image->stride();image->bytes.assign(std::size_t(stride)*height,0);
                        if(from_memory){
                            const auto source_stride=static_cast<std::uint32_t>(arg(4)),size=static_cast<std::uint32_t>(arg(5));
                            if(!arg(6) || source_stride<stride || std::uint64_t(source_stride)*(height-1)+stride>size)result=invalid;
                            else for(std::uint32_t y=0;y<height;++y)memory.read(GuestAddress{arg(6)+std::uint64_t(y)*source_stride},std::span(image->bytes).subspan(std::size_t(y)*stride,stride));
                        }
                        if(result==ok){WicState::Object object;object.bitmap=image;put(arg(output),create(Bitmap,std::move(object)),bits);}
                    }
                }else if(name=="F_CreateBitmapFromSource" || name=="F_CreateBitmapFromSourceRect"){
                    const bool rect=name=="F_CreateBitmapFromSourceRect";const unsigned output=rect?6:3;put(arg(output),0,bits);
                    if(!bridge.objects.contains(arg(1)))result=invalid;
                    else{
                        const auto source=materialize(arg(1));auto image=std::make_shared<Image>(*source);
                        if(rect){
                            const auto x=static_cast<std::uint32_t>(arg(2)),y=static_cast<std::uint32_t>(arg(3)),w=static_cast<std::uint32_t>(arg(4)),h=static_cast<std::uint32_t>(arg(5));
                            if(!w || !h || std::uint64_t(x)+w>source->width || std::uint64_t(y)+h>source->height)result=invalid;
                            else{const auto& f=*find_format(source->format);if(f.bits%8)result=unsupported_pixel_format;else{
                                image->width=w;image->height=h;const auto row=image->stride(),from=source->stride();image->bytes.assign(std::size_t(row)*h,0);
                                for(std::uint32_t r=0;r<h;++r)std::copy_n(source->bytes.begin()+static_cast<std::ptrdiff_t>(std::size_t(y+r)*from+std::size_t(x)*(f.bits/8)),row,image->bytes.begin()+static_cast<std::ptrdiff_t>(std::size_t(r)*row));}}
                        }
                        if(result==ok){WicState::Object object;object.bitmap=image;put(arg(output),create(Bitmap,std::move(object)),bits);}
                    }
                }else{
                    // 編碼器、調色盤、色彩管理、中繼資料寫入與 HBITMAP／HICON 轉換尚未實作：明確回報，不建立假物件。
                    if(service.arguments>1 && arg(service.arguments-1))put(arg(service.arguments-1),0,bits);
                    result=not_implemented;
                }
            }else if(kind==Stream){
                const auto size=[&]()->std::uint64_t{return o.host_bytes?o.host_bytes->size():o.memory_size;};
                if(name=="S_InitializeFromMemory"){
                    if(o.initialized)result=wrong_state;else if(!arg(1))result=invalid;
                    else{o.memory_base=arg(1);o.memory_size=static_cast<std::uint32_t>(arg(2));o.writable=true;o.initialized=true;o.position=0;}
                }else if(name=="S_InitializeFromFilename"){
                    if(o.initialized)result=wrong_state;
                    else if(static_cast<std::uint32_t>(arg(2))&0x40000000U)result=access_denied; // 寫入用檔案串流需配合編碼器，目前只支援讀取
                    else{o.host_bytes=read_path(arg(1));o.initialized=true;o.position=0;}
                }else if(name=="S_InitializeFromIStream" || name=="S_InitializeFromIStreamRegion"){
                    if(o.initialized)result=wrong_state;
                    else{std::vector<std::uint8_t> bytes;if(!own_stream_bytes(arg(1),bytes))result=not_implemented;
                        else{
                            if(name=="S_InitializeFromIStreamRegion"){const auto offset=std::min<std::uint64_t>(arg(2),bytes.size());const auto length=std::min<std::uint64_t>(arg(3),bytes.size()-offset);bytes=std::vector<std::uint8_t>(bytes.begin()+static_cast<std::ptrdiff_t>(offset),bytes.begin()+static_cast<std::ptrdiff_t>(offset+length));}
                            o.host_bytes=std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));o.initialized=true;o.position=0;}}
                }else if(!o.initialized)result=wrong_state;
                else if(name=="S_Read"){
                    const auto count=static_cast<std::uint32_t>(arg(2));if(arg(3))put(arg(3),0);
                    const auto available=o.position<size()?size()-o.position:0;const auto n=std::min<std::uint64_t>(count,available);
                    if(n){if(!arg(1))throw PointerError{};check(arg(1),static_cast<std::size_t>(n));
                        if(o.host_bytes)memory.write(GuestAddress{arg(1)},std::span(*o.host_bytes).subspan(static_cast<std::size_t>(o.position),static_cast<std::size_t>(n)));
                        else memory.move(GuestAddress{arg(1)},GuestAddress{o.memory_base+o.position},static_cast<std::size_t>(n));}
                    o.position+=n;if(arg(3))put(arg(3),static_cast<std::uint32_t>(n));result=n<count?s_false:ok;
                }else if(name=="S_Write"){
                    const auto count=static_cast<std::uint32_t>(arg(2));if(arg(3))put(arg(3),0);
                    if(!o.writable || o.host_bytes)result=access_denied;
                    else if(o.position+count>o.memory_size)result=0x80030070U; // STG_E_MEDIUMFULL：記憶體串流大小固定
                    else{if(count)memory.move(GuestAddress{o.memory_base+o.position},GuestAddress{arg(1)},count);o.position+=count;if(arg(3))put(arg(3),count);}
                }else if(name=="S_Seek"){
                    const auto move=static_cast<std::int64_t>(arg(1));const auto origin=static_cast<std::uint32_t>(arg(2));
                    const std::int64_t base=origin==0?0:origin==1?static_cast<std::int64_t>(o.position):origin==2?static_cast<std::int64_t>(size()):-1;
                    if(base<0)result=0x80030001U; // STG_E_INVALIDFUNCTION
                    else{const auto target=base+move;if(target<0 || static_cast<std::uint64_t>(target)>size())result=0x80030019U; // STG_E_INVALIDPOINTER 以外：超出範圍
                        else{o.position=static_cast<std::uint64_t>(target);if(arg(3))put(arg(3),o.position,64);}}
                }else if(name=="S_Stat"){
                    // STATSTG：pwcsName、type（STGTY_STREAM=2）、cbSize 等；STATFLAG_NONAME 以外也不提供名稱。
                    const unsigned size_struct=x86_?72:80;check(arg(1),size_struct);memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(size_struct));
                    memory.store(GuestAddress{arg(1)+unit},32,2);memory.store(GuestAddress{arg(1)+unit+(x86_?4U:8U)},64,size());
                }else if(name=="S_SetSize")result=size()==arg(1)?ok:0x80030070U;
                else if(name=="S_Commit" || name=="S_Revert")result=ok;
                else if(name=="S_LockRegion" || name=="S_UnlockRegion")result=0x80030100U; // STG_E_INVALIDFUNCTION：記憶體串流不支援區域鎖
                else if(name=="S_Clone"){
                    put(arg(1),0,bits);auto copy=o;copy.refs=1;put(arg(1),create(Stream,std::move(copy)),bits);
                }else result=not_implemented; // CopyTo 需要呼叫客體 IStream::Write
            }else if(kind==Decoder){
                if(name=="D_Initialize"){
                    if(o.image)result=wrong_state;else{std::vector<std::uint8_t> bytes;if(!arg(1))throw PointerError{};
                        if(own_stream_bytes(arg(1),bytes)){auto decoded=decode_bytes(bytes);o.container=decoded.container;o.image=decoded.frame;o.initialized=true;}
                        else return foreign_read(arg(1),0,0,true);}
                }else if(name=="D_QueryCapability"){
                    // WICBitmapDecoderCapabilityCanDecodeAllImages|CanDecodeSomeImages（目前支援的容器皆可解碼）。
                    put(arg(2),0);std::vector<std::uint8_t> bytes;if(!arg(1))throw PointerError{};
                    if(!own_stream_bytes(arg(1),bytes))result=not_implemented;else{try{(void)decode_bytes(bytes);put(arg(2),6);}catch(std::uint32_t){put(arg(2),0);}}
                }else if(name=="D_GetContainerFormat")write_guid(arg(1),o.container);
                else if(!o.image)result=not_initialized;
                else if(name=="D_GetFrameCount")put(arg(1),1);
                else if(name=="D_GetFrame"){
                    put(arg(2),0,bits);if(static_cast<std::uint32_t>(arg(1))!=0)result=frame_missing;
                    else{WicState::Object object;object.image=o.image;object.container=o.container;put(arg(2),create(Frame,std::move(object)),bits);}
                }else if(name=="D_GetColorContexts"){if(arg(3))put(arg(3),0);}
                else if(name=="D_GetThumbnail"){put(arg(1),0,bits);result=no_thumbnail;}
                else if(name=="D_CopyPalette")result=palette_unavailable;
                else{if(service.arguments>1 && arg(service.arguments-1))put(arg(service.arguments-1),0,bits);result=unsupported_operation;}
            }else if(kind==Lock){
                const auto& owner=bridge.objects.at(o.owner);const auto& f=*find_format(owner.bitmap->format);const auto row=(o.rect[2]*f.bits+7)/8;
                if(name=="L_GetSize"){put(arg(1),o.rect[2]);put(arg(2),o.rect[3]);}
                else if(name=="L_GetStride")put(arg(1),row);
                else if(name=="L_GetDataPointer"){put(arg(1),row*o.rect[3]);put(arg(2),o.data,bits);}
                else if(name=="L_GetPixelFormat")write_guid(arg(1),owner.bitmap->format);
            }else if(kind==PixelInfo){
                const auto& f=*o.info;
                const auto text=[&](std::u16string_view value,unsigned capacity_index,unsigned buffer_index,unsigned actual_index){
                    const auto capacity=static_cast<std::uint32_t>(arg(capacity_index));const auto needed=static_cast<std::uint32_t>(value.size()+1);
                    if(arg(actual_index))put(arg(actual_index),needed);
                    if(capacity && arg(buffer_index)){if(capacity<needed)result=insufficient_buffer;else{std::u16string copy(value);memory.write(GuestAddress{arg(buffer_index)},utf16_bytes(copy));}}
                };
                if(name=="P_GetComponentType")put(arg(1),0x20);           // WICPixelFormat
                else if(name=="P_GetCLSID" || name=="P_GetFormatGUID")write_guid(arg(1),f.id);
                else if(name=="P_GetVendorGUID")write_guid(arg(1),guid("f0e749ca-edef-4589-a73a-ee0e626a2a2b")); // GUID_VendorMicrosoft
                else if(name=="P_GetSigningStatus")put(arg(1),1);         // WICComponentSigned
                else if(name=="P_GetAuthor")text(u"Microsoft",1,2,3);
                else if(name=="P_GetVersion" || name=="P_GetSpecVersion")text(u"1.0.0.0",1,2,3);
                else if(name=="P_GetFriendlyName")text(f.name,1,2,3);
                else if(name=="P_GetBitsPerPixel")put(arg(1),f.bits);
                else if(name=="P_GetChannelCount")put(arg(1),f.channels);
                else if(name=="P_SupportsTransparency")put(arg(1),f.alpha?1:0);
                else if(name=="P_GetNumericRepresentation")put(arg(1),f.numeric);
                else if(name=="P_GetChannelMask"){if(arg(4))put(arg(4),0);result=not_implemented;}
                else{put(arg(1),0,bits);result=unsupported_operation;}
            }else{
                // Frame／Converter／Scaler／Bitmap 共用 IWICBitmapSource。
                if(name=="C_Initialize"){
                    const auto* target=find_format(read_guid(arg(2)));
                    if(o.source)result=wrong_state;else if(!target)result=unsupported_pixel_format;
                    else if(!bridge.objects.contains(arg(1)))result=invalid;
                    else{const auto source=materialize(arg(1));if(!can_convert(*find_format(source->format),*target))result=unsupported_pixel_format;else{o.source=arg(1);o.target=target->id;addref(arg(1));}}
                }else if(name=="C_CanConvert"){
                    put(arg(3),0);const auto* from=find_format(read_guid(arg(1)));const auto* to=find_format(read_guid(arg(2)));put(arg(3),from && to && can_convert(*from,*to)?1:0);
                }else if(name=="SC_Initialize"){
                    const auto width=static_cast<std::uint32_t>(arg(2)),height=static_cast<std::uint32_t>(arg(3));
                    if(o.source)result=wrong_state;else if(!bridge.objects.contains(arg(1)) || !width || !height || std::uint64_t(width)*height>(UINT64_C(1)<<28))result=invalid;
                    else{o.source=arg(1);o.width=width;o.height=height;o.mode=static_cast<std::uint32_t>(arg(4));addref(arg(1));}
                }else if(name=="BM_Lock"){
                    put(arg(3),0,bits);const auto flags=static_cast<std::uint32_t>(arg(2));const auto& image=*o.bitmap;
                    std::array<std::uint32_t,4> rect{0,0,image.width,image.height};
                    if(arg(1)){std::array<std::int32_t,4> r{};for(unsigned i=0;i<4;++i)r[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+i*4},32));
                        if(r[0]<0 || r[1]<0 || r[2]<=0 || r[3]<=0 || std::uint64_t(r[0])+std::uint64_t(r[2])>image.width || std::uint64_t(r[1])+std::uint64_t(r[3])>image.height)throw std::uint32_t{invalid};
                        rect={std::uint32_t(r[0]),std::uint32_t(r[1]),std::uint32_t(r[2]),std::uint32_t(r[3])};}
                    const auto& f=*find_format(image.format);
                    if(!(flags&3U) || (rect[0]*f.bits)%8)result=invalid;
                    else if(std::any_of(bridge.objects.begin(),bridge.objects.end(),[&](const auto& e){return e.second.kind==Lock && e.second.owner==arg(0) && (e.second.write || (flags&2U));}))result=already_locked;
                    else{
                        // 鎖定區域複製到 guest 緩衝；可寫鎖在讀取來源或 Release 時寫回。
                        const auto row=(rect[2]*f.bits+7)/8,stride=image.stride();const auto data=allocate_heap(memory,std::size_t(row)*rect[3]);if(!data)throw std::bad_alloc{};
                        for(std::uint32_t y=0;y<rect[3];++y)memory.write(GuestAddress{data+std::uint64_t(y)*row},std::span(image.bytes).subspan(std::size_t(rect[1]+y)*stride+(rect[0]*f.bits)/8,row));
                        WicState::Object lock;lock.owner=arg(0);lock.rect=rect;lock.write=(flags&2U)!=0;lock.data=data;addref(arg(0));put(arg(3),create(Lock,std::move(lock)),bits);
                    }
                }else if(name=="BM_SetResolution"){
                    // x64 的前四個參數中 double 走 XMM 暫存器；x86 為兩個 8 位元組堆疊參數。
                    const auto raw=[&](unsigned i)->std::uint64_t{if(x86_)return arg(i);std::uint64_t v=0;for(unsigned b=0;b<8;++b)v|=std::uint64_t(state.xmm[i][b])<<(8*b);return v;};
                    const auto x=std::bit_cast<double>(raw(1)),y=std::bit_cast<double>(raw(2));
                    if(!(x>0) || !(y>0))result=invalid;else{o.bitmap->dpi_x=x;o.bitmap->dpi_y=y;}
                }else if(name=="BM_SetPalette")result=unsupported_operation;
                else if(name=="FR_GetColorContexts"){if(arg(3))put(arg(3),0);}
                else if(name=="FR_GetThumbnail"){put(arg(1),0,bits);result=no_thumbnail;}
                else if(name=="FR_GetMetadataQueryReader"){put(arg(1),0,bits);result=unsupported_operation;}
                else if(name=="B_CopyPalette")result=palette_unavailable;
                else{
                    const auto image=materialize(arg(0));
                    if(name=="B_GetSize"){put(arg(1),image->width);put(arg(2),image->height);}
                    else if(name=="B_GetPixelFormat")write_guid(arg(1),image->format);
                    else if(name=="B_GetResolution"){check(arg(1),8);check(arg(2),8);memory.store(GuestAddress{arg(1)},64,std::bit_cast<std::uint64_t>(image->dpi_x));memory.store(GuestAddress{arg(2)},64,std::bit_cast<std::uint64_t>(image->dpi_y));}
                    else if(name=="B_CopyPixels"){
                        const auto& f=*find_format(image->format);std::array<std::uint32_t,4> rect{0,0,image->width,image->height};
                        if(arg(1)){std::array<std::int32_t,4> r{};for(unsigned i=0;i<4;++i)r[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+i*4},32));
                            if(r[0]<0 || r[1]<0 || r[2]<0 || r[3]<0 || std::uint64_t(r[0])+std::uint64_t(r[2])>image->width || std::uint64_t(r[1])+std::uint64_t(r[3])>image->height)throw std::uint32_t{invalid};
                            rect={std::uint32_t(r[0]),std::uint32_t(r[1]),std::uint32_t(r[2]),std::uint32_t(r[3])};}
                        const auto stride=static_cast<std::uint32_t>(arg(2)),buffer_size=static_cast<std::uint32_t>(arg(3));const auto row=(rect[2]*f.bits+7)/8;
                        if(!rect[2] || !rect[3]){}
                        else if(!arg(4))throw PointerError{};
                        else if(stride<row || (rect[0]*f.bits)%8)result=invalid;
                        else if(std::uint64_t(stride)*(rect[3]-1)+row>buffer_size)result=insufficient_buffer;
                        else{const auto from=image->stride();check(arg(4),static_cast<std::size_t>(std::uint64_t(stride)*(rect[3]-1)+row));
                            for(std::uint32_t y=0;y<rect[3];++y)memory.write(GuestAddress{arg(4)+std::uint64_t(y)*stride},std::span(image->bytes).subspan(std::size_t(rect[1]+y)*from+(rect[0]*f.bits)/8,row));}
                    }else result=not_implemented;
                }
            }
        }
    }catch(const PointerError&){result=pointer_error;}catch(const MemoryFault&){result=pointer_error;}catch(const std::bad_alloc&){result=out_of_memory;}
    catch(std::uint32_t error){result=error;}catch(const texture_image::Error& error){result=error.hresult;}
    arg.return_to(state,result);return std::nullopt;
}
}
