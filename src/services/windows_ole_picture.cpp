// oleaut32 標準圖片物件（StdPicture）：一個宿主物件同時提供 IPicture、IPictureDisp（IDispatch）與 IPersistStream 三個介面。
// - 內容類型：PICTYPE_NONE、BITMAP、METAFILE、ICON、ENHMETAFILE；fOwn 為真時最後一次 Release 刪除 GDI handle。尺寸以 HIMETRIC 記錄，
//   點陣圖與圖示依螢幕 DPI 換算，EMF 取 rclFrame，WMF 取 PICTDESC 或 placeable 標頭。OLE_HANDLE 為 32 位元。
// - 載入（OleLoadPicture／Ex／Path／File 與 IPersistStream::Load）：資料可帶 IPersistStream 標頭（"lt\0\0"＋長度）；BMP、JPEG、GIF 以 WIC／GDI+ 共用的
//   texture_image::decode 解碼成 32 位元 DIB section（含透明像素時預乘並以 AlphaBlend 繪製）；ICO 依期望尺寸選項目後 CreateIconFromResourceEx；
//   placeable WMF 以 SetMetaFileBitsEx、EMF 以 SetEnhMetaFileBits。其他格式 CTL_E_INVALIDPICTURE。
// - Render 的來源矩形為 HIMETRIC、y 軸向上（常見呼叫為 0, hmHeight, hmWidth, -hmHeight）；點陣圖以 StretchBlt，選入 DC 時沿用該 DC。
// - SaveAsFile／IPersistStream::Save：KeepOriginalFormat 為真且保有原始檔時寫原始資料，否則點陣圖寫 32 位元 BMP、圖示寫 ICO、WMF 寫 placeable、EMF 寫原生位元組；
//   OleSavePictureFile 與 Visual Basic 的 SavePicture 相同，JPEG／GIF 改存 BMP。
// - IPictureDisp：Handle（0）、hPal（2）、Type（3）、Width（4）、Height（5）屬性與 Render（6）方法；沒有型別程式庫（GetTypeInfoCount 為 0）。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include "texture_load.hpp"
#include <algorithm>
#include <array>
#include <bit>

namespace x64arm {
namespace {
using Guid=std::array<std::uint8_t,16>;
constexpr Guid make_guid(std::uint32_t a,std::uint16_t b,std::uint16_t c,std::array<std::uint8_t,8> d){
    Guid out{};for(unsigned i=0;i<4;++i)out[i]=static_cast<std::uint8_t>(a>>(8*i));
    for(unsigned i=0;i<2;++i){out[4+i]=static_cast<std::uint8_t>(b>>(8*i));out[6+i]=static_cast<std::uint8_t>(c>>(8*i));}
    for(unsigned i=0;i<8;++i)out[8+i]=d[i];return out;
}
constexpr auto iid_unknown=make_guid(0,0,0,{0xc0,0,0,0,0,0,0,0x46}),iid_dispatch=make_guid(0x00020400,0,0,{0xc0,0,0,0,0,0,0,0x46});
constexpr auto iid_persist=make_guid(0x0000010c,0,0,{0xc0,0,0,0,0,0,0,0x46}),iid_persist_stream=make_guid(0x00000109,0,0,{0xc0,0,0,0,0,0,0,0x46});
constexpr auto iid_picture=make_guid(0x7bf80980,0xbf32,0x101a,{0x8b,0xbb,0x00,0xaa,0x00,0x30,0x0c,0xab});
constexpr auto iid_picture_disp=make_guid(0x7bf80981,0xbf32,0x101a,{0x8b,0xbb,0x00,0xaa,0x00,0x30,0x0c,0xab});
constexpr auto clsid_std_picture=make_guid(0x0be35204,0x8f91,0x11ce,{0x9d,0xe3,0x00,0xaa,0x00,0x4b,0xb8,0x51});
constexpr std::uint32_t s_ok=0,s_false=1,e_pointer=0x80004003U,e_nointerface=0x80004002U,e_invalidarg=0x80070057U,e_outofmemory=0x8007000eU,
    e_fail=0x80004005U,e_unexpected=0x8000ffffU,ctl_invalid_picture=0x800a01e1U,ctl_file_not_found=0x800a0035U,
    disp_member_not_found=0x80020003U,disp_type_mismatch=0x80020005U,disp_unknown_name=0x80020006U,disp_bad_index=0x8002000bU,disp_bad_param_count=0x8002000eU;
constexpr std::uint32_t persist_magic=0x0000746cU; // IPersistStream 標頭 "lt\0\0"，其後是 DWORD 資料長度
enum Format:std::uint32_t {format_none,format_bmp,format_jpeg,format_gif,format_ico,format_wmf,format_emf};
constexpr std::size_t picture_data_limit=256U*1024U*1024U;
std::uint32_t le32(std::span<const std::uint8_t> b,std::size_t at){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t{b[at+i]}<<(8*i);return v;}
std::uint16_t le16(std::span<const std::uint8_t> b,std::size_t at){return static_cast<std::uint16_t>(b[at]|(b[at+1]<<8));}
void put32(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t v){for(unsigned i=0;i<4;++i)b[at+i]=static_cast<std::uint8_t>(v>>(8*i));}
void put16(std::vector<std::uint8_t>& b,std::size_t at,std::uint16_t v){b[at]=static_cast<std::uint8_t>(v);b[at+1]=static_cast<std::uint8_t>(v>>8);}
std::int64_t scale(std::int64_t value,std::int64_t numerator,std::int64_t denominator){
    if(!denominator)return 0;const auto product=value*numerator;const auto half=std::abs(denominator)/2;
    return (product>=0)==(denominator>0)?(product+(denominator>0?half:-half))/denominator:(product-(denominator>0?half:-half))/denominator;
}
std::uint64_t signed_arg(std::int64_t v){return static_cast<std::uint64_t>(v);}
std::int32_t long_arg(std::uint64_t v){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(v));}
std::u16string folded(std::u16string text){for(auto& c:text)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return text;}
}
std::span<const WindowsServiceDescriptor> ole_picture_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"oleaut32.dll","OleCreatePictureIndirect",4,0,0,0x4},{"oleaut32.dll","OleLoadPicture",5,0,0,0x6},{"oleaut32.dll","OleLoadPictureEx",8,0,0,0x76},
        {"oleaut32.dll","OleLoadPicturePath",6,0,0,0xc},{"oleaut32.dll","OleLoadPictureFile",2,0,0x1},{"oleaut32.dll","OleSavePictureFile",2},
        {"oleaut32.dll","OleIconToCursor",2},
        {"internal-oleaut32-picture","QueryInterface",3},{"internal-oleaut32-picture","AddRef",1},{"internal-oleaut32-picture","Release",1},
        {"internal-oleaut32-picture","get_Handle",2},{"internal-oleaut32-picture","get_hPal",2},{"internal-oleaut32-picture","get_Type",2},
        {"internal-oleaut32-picture","get_Width",2},{"internal-oleaut32-picture","get_Height",2},{"internal-oleaut32-picture","Render",11,0,0,0x3fc},
        {"internal-oleaut32-picture","set_hPal",2,0,0,0x2},{"internal-oleaut32-picture","get_CurDC",2},{"internal-oleaut32-picture","SelectPicture",4},
        {"internal-oleaut32-picture","get_KeepOriginalFormat",2},{"internal-oleaut32-picture","put_KeepOriginalFormat",2,0,0,0x2},
        {"internal-oleaut32-picture","PictureChanged",1},{"internal-oleaut32-picture","SaveAsFile",4,0,0,0x4},{"internal-oleaut32-picture","get_Attributes",2},
        {"internal-oleaut32-picture-dispatch","QueryInterface",3},{"internal-oleaut32-picture-dispatch","AddRef",1},{"internal-oleaut32-picture-dispatch","Release",1},
        {"internal-oleaut32-picture-dispatch","GetTypeInfoCount",2},{"internal-oleaut32-picture-dispatch","GetTypeInfo",4,0,0,0x6},
        {"internal-oleaut32-picture-dispatch","GetIDsOfNames",6,0,0,0x18},{"internal-oleaut32-picture-dispatch","Invoke",9,0,0,0x1a},
        {"internal-oleaut32-picture-persist","QueryInterface",3},{"internal-oleaut32-picture-persist","AddRef",1},{"internal-oleaut32-picture-persist","Release",1},
        {"internal-oleaut32-picture-persist","GetClassID",2},{"internal-oleaut32-picture-persist","IsDirty",1},{"internal-oleaut32-picture-persist","Load",2},
        {"internal-oleaut32-picture-persist","Save",3,0,0,0x4},{"internal-oleaut32-picture-persist","GetSizeMax",2}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_ole_picture(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"oleaut32 圖片",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        using service_callback::Task;
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto family=service.family;const auto name=service.symbol;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto bytes=[&](std::span<const std::uint8_t> value){const auto block=scratch(value.size());m.write(GuestAddress{block},value);return block;};
        const auto api=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments)->Task<std::uint64_t>{
            co_return co_await service_callback::call_value(resolve(dll,symbol).value,std::move(arguments));};
        const auto method=[&](std::uint64_t object,unsigned index){return m.load(GuestAddress{m.load(GuestAddress{object},bits)+std::uint64_t{index}*unit},bits);};
        const auto com=[&](std::uint64_t object,unsigned index,std::vector<std::uint64_t> arguments)->Task<std::uint64_t>{
            arguments.insert(arguments.begin(),object);co_return co_await service_callback::call_value(method(object,index),std::move(arguments));};
        const auto load_guid=[&](std::uint64_t at){Guid v{};m.read(GuestAddress{at},v);return v;};
        const auto picture=[&](std::uint64_t base)->OlePicture*{const auto found=ole_pictures_.find(base);return found==ole_pictures_.end()?nullptr:&found->second;};
        const auto screen_dpi=[&]()->Task<std::pair<std::int64_t,std::int64_t>>{
            std::int64_t x=96,y=96;const auto dc=co_await api("user32.dll","GetDC",{0});
            if(dc){x=static_cast<std::int32_t>(co_await api("gdi32.dll","GetDeviceCaps",{dc,88}));y=static_cast<std::int32_t>(co_await api("gdi32.dll","GetDeviceCaps",{dc,90}));(void)co_await api("user32.dll","ReleaseDC",{0,dc});}
            co_return std::pair{x>0?x:96,y>0?y:96};
        };
        const auto bitmap_size=[&](std::uint64_t handle)->Task<std::pair<std::int64_t,std::int64_t>>{
            const auto info=scratch(32);if(!static_cast<std::uint32_t>(co_await api("gdi32.dll","GetObjectW",{handle,x86_?24U:32U,info})))co_return std::pair<std::int64_t,std::int64_t>{0,0};
            co_return std::pair<std::int64_t,std::int64_t>{long_arg(m.load(GuestAddress{info+4},32)),std::abs(long_arg(m.load(GuestAddress{info+8},32)))};
        };
        // ICONINFO：fIcon、xHotspot、yHotspot、hbmMask、hbmColor；呼叫者刪除兩個點陣圖。
        struct IconParts {std::uint64_t mask{},color{};std::uint32_t hot_x{},hot_y{};bool valid{};};
        const auto icon_parts=[&](std::uint64_t icon)->Task<IconParts>{
            const auto info=scratch(32);IconParts parts;if(!static_cast<std::uint32_t>(co_await api("user32.dll","GetIconInfo",{icon,info})))co_return parts;
            parts.valid=true;parts.hot_x=static_cast<std::uint32_t>(m.load(GuestAddress{info+4},32));parts.hot_y=static_cast<std::uint32_t>(m.load(GuestAddress{info+8},32));
            parts.mask=m.load(GuestAddress{info+(x86_?12U:16U)},bits);parts.color=m.load(GuestAddress{info+(x86_?16U:24U)},bits);co_return parts;
        };
        const auto drop_parts=[&](const IconParts& parts)->Task<std::uint32_t>{
            if(parts.mask)(void)co_await api("gdi32.dll","DeleteObject",{parts.mask});if(parts.color)(void)co_await api("gdi32.dll","DeleteObject",{parts.color});co_return 0;};
        const auto icon_size=[&](std::uint64_t icon)->Task<std::pair<std::int64_t,std::int64_t>>{
            const auto parts=co_await icon_parts(icon);if(!parts.valid)co_return std::pair<std::int64_t,std::int64_t>{0,0};
            auto size=co_await bitmap_size(parts.color?parts.color:parts.mask);if(!parts.color)size.second/=2;
            (void)co_await drop_parts(parts);co_return size;
        };
        // 依 handle 重新計算 HIMETRIC 尺寸（WMF 沿用建立時的範圍）。
        const auto measure=[&](std::uint64_t base)->Task<std::uint32_t>{
            auto* p=picture(base);if(!p)co_return 0;const auto type=p->type;const auto handle=p->handle;
            std::int64_t w=0,h=0;
            if(type==1 || type==3){
                const auto [px,py]=type==1?co_await bitmap_size(handle):co_await icon_size(handle);const auto [dx,dy]=co_await screen_dpi();
                w=scale(px,2540,dx);h=scale(py,2540,dy);
            }else if(type==4){
                const auto header=scratch(100);
                if(static_cast<std::uint32_t>(co_await api("gdi32.dll","GetEnhMetaFileHeader",{handle,100,header}))){
                    w=long_arg(m.load(GuestAddress{header+32},32))-long_arg(m.load(GuestAddress{header+24},32));
                    h=long_arg(m.load(GuestAddress{header+36},32))-long_arg(m.load(GuestAddress{header+28},32));
                }
            }else co_return 0;
            if((p=picture(base))){p->width=static_cast<std::int32_t>(w);p->height=static_cast<std::int32_t>(h);}
            co_return 0;
        };
        const auto destroy_content=[&](std::uint64_t base)->Task<std::uint32_t>{
            auto* p=picture(base);if(!p)co_return 0;const auto type=p->type;const auto handle=p->handle;const bool own=p->own;
            p->type=0;p->handle=0;p->palette=0;p->width=p->height=p->logical_width=p->logical_height=0;p->transparent=false;p->original.clear();p->format=format_none;
            if(!own || !handle)co_return 0;
            if(type==1)(void)co_await api("gdi32.dll","DeleteObject",{handle});else if(type==2)(void)co_await api("gdi32.dll","DeleteMetaFile",{handle});
            else if(type==3)(void)co_await api("user32.dll","DestroyIcon",{handle});else if(type==4)(void)co_await api("gdi32.dll","DeleteEnhMetaFile",{handle});
            co_return 0;
        };
        // 由檔案位元組建立內容；want 為 OleLoadPictureEx 的期望尺寸（0＝預設）。
        const auto load_bytes=[&](std::uint64_t base,std::vector<std::uint8_t> data,std::int64_t want_x,std::int64_t want_y,std::uint32_t lp_flags)->Task<std::uint32_t>{
            if(data.size()>=8 && le32(data,0)==persist_magic){
                const auto size=le32(data,4);if(size>data.size()-8)co_return ctl_invalid_picture;
                data=std::vector<std::uint8_t>(data.begin()+8,data.begin()+8+size);
            }
            (void)co_await destroy_content(base);
            if(data.empty()){if(auto* p=picture(base)){p->type=0;p->own=true;}co_return s_ok;}
            Format format=format_none;
            if(data.size()>=2 && data[0]=='B' && data[1]=='M')format=format_bmp;
            else if(data.size()>=3 && data[0]==0xff && data[1]==0xd8 && data[2]==0xff)format=format_jpeg;
            else if(data.size()>=6 && data[0]=='G' && data[1]=='I' && data[2]=='F' && data[3]=='8')format=format_gif;
            else if(data.size()>=6 && le16(data,0)==0 && le16(data,2)==1 && le16(data,4))format=format_ico;
            else if(data.size()>=22 && le32(data,0)==0x9ac6cdd7U)format=format_wmf;
            else if(data.size()>=88 && le32(data,0)==1 && le32(data,40)==0x464d4520U)format=format_emf;
            else co_return ctl_invalid_picture;
            std::int16_t type=0;std::uint64_t handle=0;bool transparent=false;std::int32_t logical_w=0,logical_h=0,himetric_w=0,himetric_h=0;
            if(format==format_bmp || format==format_jpeg || format==format_gif){
                texture_image::Decoded image;
                try{image=texture_image::decode(data);if(image.desc.format!=28)texture_image::convert_format(image,28);}catch(const texture_image::Error&){co_return ctl_invalid_picture;}
                if(image.pixels.empty())co_return ctl_invalid_picture;
                const std::uint32_t w=image.desc.width,h=image.desc.height;auto& pixels=image.pixels.front();
                if(!w || !h || std::uint64_t{w}*h>64U*1024U*1024U || pixels.size()<std::uint64_t{w}*h*4)co_return ctl_invalid_picture;
                // BMP 的 32 位元 BI_RGB 第四個位元組是保留欄位，與 CreateDIBitmap 相同視為不透明；GIF 的透明色才算透明。
                for(std::size_t i=0;i+3<pixels.size();i+=4){std::swap(pixels[i],pixels[i+2]);if(format==format_bmp)pixels[i+3]=255;else if(pixels[i+3]!=255)transparent=true;}
                if(transparent)for(std::size_t i=0;i+3<pixels.size();i+=4)for(unsigned c=0;c<3;++c)pixels[i+c]=static_cast<std::uint8_t>((pixels[i+c]*pixels[i+3]+127)/255);
                const auto header=scratch(48);m.store(GuestAddress{header},32,40);m.store(GuestAddress{header+4},32,w);
                m.store(GuestAddress{header+8},32,static_cast<std::uint32_t>(-static_cast<std::int32_t>(h)));m.store(GuestAddress{header+12},16,1);m.store(GuestAddress{header+14},16,32);
                const auto bits_at=scratch(unit);
                handle=co_await api("gdi32.dll","CreateDIBSection",{0,header,0,bits_at,0,0});const auto target=m.load(GuestAddress{bits_at},bits);
                if(!handle || !target){if(handle)(void)co_await api("gdi32.dll","DeleteObject",{handle});co_return e_outofmemory;}
                m.write(GuestAddress{target},std::span<const std::uint8_t>(pixels.data(),std::size_t{w}*h*4));sync_raster_bitmap(m,handle);type=1;
            }else if(format==format_ico){
                // ICONDIR：reserved、type、count；每項 16 bytes（寬、高為 0 代表 256）。有期望尺寸選最接近者，否則選系統圖示大小中色深最高者，再否則第一項。
                const auto count=le16(data,4);if(data.size()<6+std::size_t{count}*16)co_return ctl_invalid_picture;
                std::int64_t target_x=want_x,target_y=want_y;bool prefer_depth=false;
                if(!target_x || !target_y){target_x=static_cast<std::int32_t>(co_await api("user32.dll","GetSystemMetrics",{11}));target_y=static_cast<std::int32_t>(co_await api("user32.dll","GetSystemMetrics",{12}));prefer_depth=true;}
                std::size_t best=0;std::int64_t best_score=INT64_MAX;
                for(std::size_t i=0;i<count;++i){
                    const auto at=6+i*16;const std::int64_t w=data[at]?data[at]:256,h=data[at+1]?data[at+1]:256;const auto depth=le16(data,at+6);
                    const auto score=(std::abs(w-target_x)+std::abs(h-target_y))*64-(prefer_depth?depth:0);
                    if(score<best_score){best_score=score;best=i;}
                }
                const auto at=6+best*16;const auto size=le32(data,at+8),offset=le32(data,at+12);
                if(!size || offset>data.size() || size>data.size()-offset)co_return ctl_invalid_picture;
                const std::int64_t w=data[at]?data[at]:256,h=data[at+1]?data[at+1]:256;
                const auto image=bytes(std::span<const std::uint8_t>(data).subspan(offset,size));
                handle=co_await api("user32.dll","CreateIconFromResourceEx",{image,size,1,0x00030000U,static_cast<std::uint64_t>(w),static_cast<std::uint64_t>(h),(lp_flags&1)?1U:0U});
                if(!handle)co_return ctl_invalid_picture;type=3;
            }else if(format==format_wmf){
                // placeable 標頭：key、hmf、left、top、right、bottom（int16）、inch、reserved、checksum；繪製時以標頭範圍為邏輯座標。
                const auto left=static_cast<std::int16_t>(le16(data,6)),top=static_cast<std::int16_t>(le16(data,8)),right=static_cast<std::int16_t>(le16(data,10)),bottom=static_cast<std::int16_t>(le16(data,12));
                const auto inch=le16(data,14);if(!inch)co_return ctl_invalid_picture;
                const auto body=bytes(std::span<const std::uint8_t>(data).subspan(22));
                handle=co_await api("gdi32.dll","SetMetaFileBitsEx",{data.size()-22,body});if(!handle)co_return ctl_invalid_picture;
                logical_w=right-left;logical_h=bottom-top;himetric_w=static_cast<std::int32_t>(scale(logical_w,2540,inch));himetric_h=static_cast<std::int32_t>(scale(logical_h,2540,inch));type=2;
            }else{
                handle=co_await api("gdi32.dll","SetEnhMetaFileBits",{data.size(),bytes(data)});if(!handle)co_return ctl_invalid_picture;type=4;
            }
            auto* p=picture(base);if(!p)co_return e_unexpected;
            p->type=type;p->handle=handle;p->own=true;p->transparent=transparent;p->logical_width=logical_w;p->logical_height=logical_h;
            p->width=himetric_w;p->height=himetric_h;p->format=format;p->original=std::move(data);
            (void)co_await measure(base);co_return s_ok;
        };
        // 序列化：original_allowed 時沿用原始檔。回傳 HRESULT 與資料。
        const auto serialize=[&](std::uint64_t base,bool original_allowed)->Task<std::pair<std::uint32_t,std::vector<std::uint8_t>>>{
            using Result=std::pair<std::uint32_t,std::vector<std::uint8_t>>;
            const auto* p=picture(base);if(!p)co_return Result{e_unexpected,{}};
            if(original_allowed && p->keep_original && !p->original.empty())co_return Result{s_ok,p->original};
            const auto type=p->type;const auto handle=p->handle;
            if(type==0)co_return Result{s_ok,{}};
            if(type<0)co_return Result{e_unexpected,{}};
            if(type==4){
                const auto size=static_cast<std::uint32_t>(co_await api("gdi32.dll","GetEnhMetaFileBits",{handle,0,0}));if(!size)co_return Result{e_fail,{}};
                const auto buffer=scratch(size);(void)co_await api("gdi32.dll","GetEnhMetaFileBits",{handle,size,buffer});
                std::vector<std::uint8_t> out(size);m.read(GuestAddress{buffer},std::span<std::uint8_t>(out));co_return Result{s_ok,std::move(out)};
            }
            if(type==2){
                const auto size=static_cast<std::uint32_t>(co_await api("gdi32.dll","GetMetaFileBitsEx",{handle,0,0}));if(!size)co_return Result{e_fail,{}};
                const auto buffer=scratch(size);(void)co_await api("gdi32.dll","GetMetaFileBitsEx",{handle,size,buffer});
                std::int64_t w=p->logical_width?p->logical_width:p->width,h=p->logical_height?p->logical_height:p->height;std::uint16_t inch=p->logical_width?0:2540;
                if(!inch){inch=static_cast<std::uint16_t>(std::clamp<std::int64_t>(p->width?scale(w,2540,p->width):1440,1,65535));}
                while((std::abs(w)>32767 || std::abs(h)>32767) && inch>1){w/=2;h/=2;inch=static_cast<std::uint16_t>(std::max(1,inch/2));}
                std::vector<std::uint8_t> out(22+size);put32(out,0,0x9ac6cdd7U);put16(out,10,static_cast<std::uint16_t>(w));put16(out,12,static_cast<std::uint16_t>(h));put16(out,14,inch);
                std::uint16_t checksum=0;for(unsigned i=0;i<10;++i)checksum^=le16(out,i*2);put16(out,20,checksum);
                m.read(GuestAddress{buffer},std::span<std::uint8_t>(out).subspan(22));co_return Result{s_ok,std::move(out)};
            }
            const auto dc=co_await api("user32.dll","GetDC",{0});
            struct Dc {std::uint64_t value;} screen{dc};
            const auto dib_rows=[&](std::uint64_t bitmap,std::int64_t w,std::int64_t h,unsigned depth,std::vector<std::uint8_t>& palette)->Task<std::vector<std::uint8_t>>{
                const auto stride=((w*depth+31)/32)*4;const auto info=scratch(40+8);m.store(GuestAddress{info},32,40);m.store(GuestAddress{info+4},32,static_cast<std::uint32_t>(w));
                m.store(GuestAddress{info+8},32,static_cast<std::uint32_t>(h));m.store(GuestAddress{info+12},16,1);m.store(GuestAddress{info+14},16,depth);
                const auto buffer=scratch(static_cast<std::size_t>(stride*h));
                if(static_cast<std::uint32_t>(co_await api("gdi32.dll","GetDIBits",{screen.value,bitmap,0,static_cast<std::uint64_t>(h),buffer,info,0}))!=h)co_return std::vector<std::uint8_t>{};
                if(depth==1){palette.assign(8,0);m.read(GuestAddress{info+40},std::span<std::uint8_t>(palette));}
                std::vector<std::uint8_t> out(static_cast<std::size_t>(stride*h));m.read(GuestAddress{buffer},std::span<std::uint8_t>(out));co_return out;
            };
            Result result{e_fail,{}};
            if(type==1){
                const auto [w,h]=co_await bitmap_size(handle);std::vector<std::uint8_t> palette;
                const auto rows=w && h?co_await dib_rows(handle,w,h,32,palette):std::vector<std::uint8_t>{};
                if(!rows.empty()){
                    std::vector<std::uint8_t> out(54+rows.size());out[0]='B';out[1]='M';put32(out,2,static_cast<std::uint32_t>(out.size()));put32(out,10,54);
                    put32(out,14,40);put32(out,18,static_cast<std::uint32_t>(w));put32(out,22,static_cast<std::uint32_t>(h));put16(out,26,1);put16(out,28,32);put32(out,34,static_cast<std::uint32_t>(rows.size()));
                    std::copy(rows.begin(),rows.end(),out.begin()+54);result={s_ok,std::move(out)};
                }
            }else if(type==3){
                // ICO：ICONDIR＋一個項目＋BITMAPINFOHEADER（高度為兩倍）＋XOR 與 AND 位元；單色圖示的遮罩已是「XOR 在下半、AND 在上半」的由下往上順序。
                const auto parts=co_await icon_parts(handle);
                if(parts.valid){
                    std::vector<std::uint8_t> palette,xor_rows,and_rows;std::int64_t w=0,h=0;unsigned depth=32;
                    if(parts.color){const auto size=co_await bitmap_size(parts.color);w=size.first;h=size.second;xor_rows=co_await dib_rows(parts.color,w,h,32,palette);std::vector<std::uint8_t> unused;and_rows=co_await dib_rows(parts.mask,w,h,1,unused);palette.clear();}
                    else {const auto size=co_await bitmap_size(parts.mask);w=size.first;h=size.second/2;depth=1;xor_rows=co_await dib_rows(parts.mask,w,h*2,1,palette);}
                    (void)co_await drop_parts(parts);
                    if(!xor_rows.empty() && (depth==1 || !and_rows.empty()) && w>0 && h>0 && w<=256 && h<=256){
                        const std::size_t image=40+palette.size()+xor_rows.size()+and_rows.size();std::vector<std::uint8_t> out(22+image);
                        put16(out,2,1);put16(out,4,1);out[6]=static_cast<std::uint8_t>(w&255);out[7]=static_cast<std::uint8_t>(h&255);out[8]=depth==1?2:0;put16(out,10,1);put16(out,12,static_cast<std::uint16_t>(depth));
                        put32(out,14,static_cast<std::uint32_t>(image));put32(out,18,22);
                        put32(out,22,40);put32(out,26,static_cast<std::uint32_t>(w));put32(out,30,static_cast<std::uint32_t>(h*2));put16(out,34,1);put16(out,36,static_cast<std::uint16_t>(depth));
                        auto at=out.begin()+62;at=std::copy(palette.begin(),palette.end(),at);at=std::copy(xor_rows.begin(),xor_rows.end(),at);std::copy(and_rows.begin(),and_rows.end(),at);
                        result={s_ok,std::move(out)};
                    }
                }
            }
            (void)co_await api("user32.dll","ReleaseDC",{0,screen.value});
            co_return result;
        };
        const auto render=[&](std::uint64_t base,std::uint64_t dc,std::int64_t x,std::int64_t y,std::int64_t cx,std::int64_t cy,std::int64_t xs,std::int64_t ys,std::int64_t cxs,std::int64_t cys)->Task<std::uint32_t>{
            const auto* p=picture(base);if(!p)co_return e_unexpected;
            if(!dc)co_return e_invalidarg;
            const auto type=p->type;const auto handle=p->handle;const auto current=p->current_dc;const bool transparent=p->transparent;
            const std::int64_t hm_w=p->width,hm_h=p->height,lw=p->logical_width?p->logical_width:p->width,lh=p->logical_height?p->logical_height:p->height;
            if(type<0)co_return e_unexpected;
            if(type==0)co_return s_ok;
            if(!cx || !cy)co_return e_invalidarg;
            if(type==3)co_return static_cast<std::uint32_t>(co_await api("user32.dll","DrawIconEx",{dc,signed_arg(x),signed_arg(y),handle,signed_arg(cx),signed_arg(cy),0,0,3}))?s_ok:e_fail;
            if(type==4){
                const auto rect=scratch(16);for(const auto [i,v]:std::array<std::pair<unsigned,std::int64_t>,4>{{{0,x},{1,y},{2,x+cx},{3,y+cy}}})m.store(GuestAddress{rect+i*4},32,static_cast<std::uint32_t>(v));
                co_return static_cast<std::uint32_t>(co_await api("gdi32.dll","PlayEnhMetaFile",{dc,handle,rect}))?s_ok:e_fail;
            }
            if(type==2){
                // WMF：以 MM_ANISOTROPIC 把邏輯範圍對應到目的矩形。
                const auto saved=co_await api("gdi32.dll","SaveDC",{dc});
                (void)co_await api("gdi32.dll","SetMapMode",{dc,8});(void)co_await api("gdi32.dll","SetWindowOrgEx",{dc,0,0,0});
                (void)co_await api("gdi32.dll","SetWindowExtEx",{dc,signed_arg(lw),signed_arg(lh),0});(void)co_await api("gdi32.dll","SetViewportOrgEx",{dc,signed_arg(x),signed_arg(y),0});
                (void)co_await api("gdi32.dll","SetViewportExtEx",{dc,signed_arg(cx),signed_arg(cy),0});
                const auto played=static_cast<std::uint32_t>(co_await api("gdi32.dll","PlayMetaFile",{dc,handle}));
                if(saved)(void)co_await api("gdi32.dll","RestoreDC",{dc,signed_arg(-1)});
                co_return played?s_ok:e_fail;
            }
            const auto [pw,ph]=co_await bitmap_size(handle);if(!pw || !ph || !hm_w || !hm_h)co_return e_fail;
            const auto sx=scale(xs,pw,hm_w),sy=scale(hm_h-ys,ph,hm_h),scx=scale(cxs,pw,hm_w),scy=scale(-cys,ph,hm_h);
            std::uint64_t source=current,memory_dc=0,previous=0;
            if(!source){memory_dc=co_await api("gdi32.dll","CreateCompatibleDC",{dc});if(!memory_dc)co_return e_fail;previous=co_await api("gdi32.dll","SelectObject",{memory_dc,handle});source=memory_dc;}
            bool drawn;
            if(transparent && cx>0 && cy>0 && scx>0 && scy>0)
                drawn=static_cast<std::uint32_t>(co_await api("gdi32.dll","GdiAlphaBlend",{dc,signed_arg(x),signed_arg(y),signed_arg(cx),signed_arg(cy),source,signed_arg(sx),signed_arg(sy),signed_arg(scx),signed_arg(scy),0x01ff0000U}))!=0;
            else drawn=static_cast<std::uint32_t>(co_await api("gdi32.dll","StretchBlt",{dc,signed_arg(x),signed_arg(y),signed_arg(cx),signed_arg(cy),source,signed_arg(sx),signed_arg(sy),signed_arg(scx),signed_arg(scy),0x00cc0020U}))!=0;
            if(memory_dc){if(previous)(void)co_await api("gdi32.dll","SelectObject",{memory_dc,previous});(void)co_await api("gdi32.dll","DeleteDC",{memory_dc});}
            co_return drawn?s_ok:e_fail;
        };
        const auto vtable=[&](unsigned which){
            auto& table=ole_picture_vtables_[which];if(table)return table;
            static constexpr std::array<std::string_view,3> families{"internal-oleaut32-picture","internal-oleaut32-picture-dispatch","internal-oleaut32-picture-persist"};
            std::vector<std::uint64_t> entries;for(const auto& d:ole_picture_service_descriptors())if(d.family==families[which])entries.push_back(resolve(d.family,d.symbol).value);
            table=runtime_allocate(m,{},entries.size()*unit);for(std::size_t i=0;i<entries.size();++i)m.store(GuestAddress{table+i*unit},bits,entries[i]);
            return table;
        };
        const auto create=[&]()->std::uint64_t{
            if(ole_pictures_.size()>=65536)throw std::bad_alloc{};
            const auto object=allocate_heap(m,3*unit);if(!object)throw std::bad_alloc{};
            for(unsigned i=0;i<3;++i)m.store(GuestAddress{object+i*unit},bits,vtable(i));
            ole_pictures_.emplace(object,OlePicture{});return object;
        };
        const auto interface_of=[&](std::uint64_t base,const Guid& iid)->std::uint64_t{
            if(iid==iid_unknown || iid==iid_picture)return base;
            if(iid==iid_picture_disp || iid==iid_dispatch)return base+unit;
            if(iid==iid_persist || iid==iid_persist_stream)return base+2*unit;
            return 0;
        };
        const auto release_object=[&](std::uint64_t base)->Task<std::uint32_t>{
            auto* p=picture(base);if(!p)co_return 0;if(p->references>1)co_return --p->references;
            (void)co_await destroy_content(base);ole_pictures_.erase(base);(void)free_heap(m,base,s);co_return 0;
        };
        // 新建物件後依 riid 回傳介面；失敗時釋放物件（含擁有的 handle）。
        const auto hand_out=[&](std::uint64_t base,std::uint64_t riid,std::uint64_t output)->Task<std::uint32_t>{
            const auto pointer=riid?interface_of(base,load_guid(riid)):0;
            if(!pointer){(void)co_await release_object(base);co_return e_nointerface;}
            m.store(GuestAddress{output},bits,pointer);co_return s_ok;
        };
        const auto read_stream=[&](std::uint64_t stream,std::uint64_t limit)->Task<std::pair<std::uint32_t,std::vector<std::uint8_t>>>{
            std::vector<std::uint8_t> data;constexpr std::uint32_t chunk=65536;const auto buffer=scratch(chunk),count=scratch(4);
            while(!limit || data.size()<limit){
                const auto want=static_cast<std::uint32_t>(limit?std::min<std::uint64_t>(chunk,limit-data.size()):chunk);
                m.store(GuestAddress{count},32,0);const auto status=static_cast<std::uint32_t>(co_await com(stream,3,{buffer,want,count}));
                if(status&0x80000000U)co_return std::pair{status,std::move(data)};
                const auto got=std::min<std::uint32_t>(static_cast<std::uint32_t>(m.load(GuestAddress{count},32)),want);
                if(data.size()+got>picture_data_limit)co_return std::pair{e_outofmemory,std::move(data)};
                const auto old=data.size();data.resize(old+got);if(got)m.read(GuestAddress{buffer},std::span<std::uint8_t>(data).subspan(old,got));
                if(got<want)break;
            }
            co_return std::pair{s_ok,std::move(data)};
        };
        const auto write_stream=[&](std::uint64_t stream,std::span<const std::uint8_t> data)->Task<std::uint32_t>{
            if(data.empty())co_return s_ok;const auto buffer=bytes(data),written=scratch(4);
            const auto status=static_cast<std::uint32_t>(co_await com(stream,4,{buffer,data.size(),written}));
            if(status&0x80000000U)co_return status;co_return m.load(GuestAddress{written},32)==data.size()?s_ok:0x80030070U; // STG_E_MEDIUMFULL
        };
        const auto load_into_new=[&](std::uint64_t stream,std::uint64_t limit,bool keep,std::uint64_t riid,std::uint64_t output,std::int64_t want_x,std::int64_t want_y,std::uint32_t lp_flags)->Task<std::uint32_t>{
            auto [status,data]=co_await read_stream(stream,limit);if(status)co_return status;
            const auto object=create();picture(object)->keep_original=keep;
            if(const auto loaded=co_await load_bytes(object,std::move(data),want_x,want_y,lp_flags)){(void)co_await release_object(object);co_return loaded;}
            co_return co_await hand_out(object,riid,output);
        };
        // 路徑或 URL 開成串流：http／https／ftp 走 URLOpenBlockingStreamW，file: URL 先轉成路徑。
        const auto open_path=[&](std::u16string path,std::uint64_t caller,std::uint64_t stream_at)->Task<std::uint32_t>{
            const auto lower=folded(path);
            if(lower.starts_with(u"file:")){
                const auto converted=scratch(2*32768),length=scratch(4);m.store(GuestAddress{length},32,32768);
                if(const auto status=static_cast<std::uint32_t>(co_await api("shlwapi.dll","PathCreateFromUrlW",{bytes(utf16_bytes(path)),converted,length,0})))co_return status;
                path=read_utf16(m,GuestAddress{converted},32768);
            }else if(lower.find(u"://")!=lower.npos)
                co_return static_cast<std::uint32_t>(co_await api("urlmon.dll","URLOpenBlockingStreamW",{caller,bytes(utf16_bytes(path)),stream_at,0,0}));
            co_return static_cast<std::uint32_t>(co_await api("shlwapi.dll","SHCreateStreamOnFileW",{bytes(utf16_bytes(path)),0x20,stream_at}));
        };
        try{
            if(family!="oleaut32.dll"){
                // 介面方法：由 vtable 指標判定介面位移後找回物件。
                const auto self=call(0);const auto table=m.load(GuestAddress{self},bits);
                const auto base=table==ole_picture_vtables_[0]?self:table==ole_picture_vtables_[1]?self-unit:table==ole_picture_vtables_[2]?self-2*unit:0;
                if(!base || !picture(base))co_return finish(e_unexpected);
                if(name=="QueryInterface"){
                    if(!call(2))co_return finish(e_pointer);m.store(GuestAddress{call(2)},bits,0);if(!call(1))co_return finish(e_invalidarg);
                    const auto pointer=interface_of(base,load_guid(call(1)));if(!pointer)co_return finish(e_nointerface);
                    ++picture(base)->references;m.store(GuestAddress{call(2)},bits,pointer);co_return finish(s_ok);
                }
                if(name=="AddRef")co_return finish(++picture(base)->references);
                if(name=="Release")co_return finish(co_await release_object(base));
                auto* p=picture(base);
                if(family=="internal-oleaut32-picture"){
                    const auto out=call(1);
                    if(name=="get_Handle" || name=="get_hPal" || name=="get_Type" || name=="get_Width" || name=="get_Height" || name=="get_CurDC" || name=="get_KeepOriginalFormat" || name=="get_Attributes"){
                        if(!out)co_return finish(e_pointer);
                        if(name=="get_Handle"){m.store(GuestAddress{out},32,static_cast<std::uint32_t>(p->handle));co_return finish(p->type<0?e_unexpected:s_ok);}
                        if(name=="get_hPal"){if(p->type!=1)co_return finish(e_fail);m.store(GuestAddress{out},32,static_cast<std::uint32_t>(p->palette));co_return finish(s_ok);}
                        if(name=="get_Type"){m.store(GuestAddress{out},16,static_cast<std::uint16_t>(p->type));co_return finish(s_ok);}
                        if(name=="get_Width" || name=="get_Height"){m.store(GuestAddress{out},32,static_cast<std::uint32_t>(name=="get_Width"?p->width:p->height));co_return finish(s_ok);}
                        if(name=="get_CurDC"){m.store(GuestAddress{out},bits,p->current_dc);co_return finish(s_ok);}
                        if(name=="get_KeepOriginalFormat"){m.store(GuestAddress{out},32,p->keep_original?1:0);co_return finish(s_ok);}
                        // PICTURE_SCALABLE 1（中繼檔）、PICTURE_TRANSPARENT 2（圖示、中繼檔、含透明像素的點陣圖）。
                        const std::uint32_t attributes=(p->type==2 || p->type==4?3U:0U)|(p->type==3 || (p->type==1 && p->transparent)?2U:0U);
                        m.store(GuestAddress{out},32,attributes);co_return finish(s_ok);
                    }
                    if(name=="set_hPal"){if(p->type!=1)co_return finish(e_fail);p->palette=static_cast<std::uint32_t>(call(1));co_return finish(s_ok);}
                    if(name=="put_KeepOriginalFormat"){p->keep_original=static_cast<std::uint32_t>(call(1))!=0;co_return finish(s_ok);}
                    if(name=="PictureChanged"){p->dirty=true;(void)co_await measure(base);co_return finish(s_ok);}
                    if(name=="SelectPicture"){
                        // (hDCIn, phDCOut, phBmpOut)：只限點陣圖；回傳先前的目前 DC 與 hDCIn 原本選入的點陣圖。
                        if(p->type!=1)co_return finish(e_fail);if(!call(1))co_return finish(e_invalidarg);
                        const auto handle=p->handle;const auto previous_dc=p->current_dc;
                        const auto previous=co_await api("gdi32.dll","SelectObject",{call(1),handle});if(!previous)co_return finish(e_fail);
                        if((p=picture(base)))p->current_dc=call(1);
                        if(call(2))m.store(GuestAddress{call(2)},bits,previous_dc);if(call(3))m.store(GuestAddress{call(3)},32,static_cast<std::uint32_t>(previous));
                        co_return finish(s_ok);
                    }
                    if(name=="Render")
                        co_return finish(co_await render(base,call(1),long_arg(call(2)),long_arg(call(3)),long_arg(call(4)),long_arg(call(5)),long_arg(call(6)),long_arg(call(7)),long_arg(call(8)),long_arg(call(9))));
                    if(name=="SaveAsFile"){
                        // (pStream, fSaveMemCopy, pCbSize)
                        if(!call(1))co_return finish(e_pointer);
                        auto [status,data]=co_await serialize(base,true);if(status)co_return finish(status);
                        if(const auto written=co_await write_stream(call(1),data))co_return finish(written);
                        if(call(3))m.store(GuestAddress{call(3)},32,static_cast<std::uint32_t>(data.size()));co_return finish(s_ok);
                    }
                }else if(family=="internal-oleaut32-picture-dispatch"){
                    if(name=="GetTypeInfoCount"){if(!call(1))co_return finish(e_invalidarg);m.store(GuestAddress{call(1)},32,0);co_return finish(s_ok);}
                    if(name=="GetTypeInfo"){if(call(3))m.store(GuestAddress{call(3)},bits,0);co_return finish(disp_bad_index);}
                    if(name=="GetIDsOfNames"){
                        // (riid, rgszNames, cNames, lcid, rgDispId)
                        const auto names=call(2),ids=call(5);const auto count=static_cast<std::uint32_t>(call(3));if(!names || !ids || !count)co_return finish(e_invalidarg);
                        static constexpr std::array<std::pair<std::u16string_view,std::int32_t>,6> members{{{u"handle",0},{u"hpal",2},{u"type",3},{u"width",4},{u"height",5},{u"render",6}}};
                        const auto first=folded(read_utf16(m,GuestAddress{m.load(GuestAddress{names},bits)},256));std::int32_t id=-1;
                        for(const auto& [member,dispid]:members)if(first==member)id=dispid;
                        m.store(GuestAddress{ids},32,static_cast<std::uint32_t>(id));for(std::uint32_t i=1;i<count && i<64;++i)m.store(GuestAddress{ids+i*4},32,0xffffffffU);
                        co_return finish(id<0 || count>1?disp_unknown_name:s_ok);
                    }
                    if(name=="Invoke"){
                        // (dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr)
                        const auto dispid=long_arg(call(1));const auto flags=static_cast<std::uint32_t>(call(4))&0xffffU;const auto params=call(5),result=call(6);
                        if(!params)co_return finish(e_invalidarg);
                        const auto args=m.load(GuestAddress{params},bits);const auto count=static_cast<std::uint32_t>(m.load(GuestAddress{params+2*unit},32));const auto named=static_cast<std::uint32_t>(m.load(GuestAddress{params+2*unit+4},32));
                        const unsigned variant=x86_?16U:24U;
                        const auto as_long=[&](std::uint32_t index)->Task<std::pair<std::uint32_t,std::int32_t>>{
                            const auto converted=scratch(variant);
                            const auto status=static_cast<std::uint32_t>(co_await api("oleaut32.dll","VariantChangeType",{converted,args+std::uint64_t{index}*variant,0,3}));
                            co_return std::pair{status,status?0:long_arg(m.load(GuestAddress{converted+8},32))};
                        };
                        const auto store_result=[&](std::uint16_t vt,std::uint32_t value){if(!result)return;m.fill(GuestAddress{result},0,variant);m.store(GuestAddress{result},16,vt);m.store(GuestAddress{result+8},vt==2?16:32,value);};
                        if(dispid==6){
                            if(!(flags&1))co_return finish(disp_member_not_found);if(count!=10 || named)co_return finish(disp_bad_param_count);
                            std::array<std::int32_t,10> values{};
                            for(std::uint32_t i=0;i<10;++i){const auto [status,value]=co_await as_long(9-i);if(status){if(call(8))m.store(GuestAddress{call(8)},32,9-i);co_return finish(disp_type_mismatch);}values[i]=value;}
                            co_return finish(co_await render(base,static_cast<std::uint32_t>(values[0]),values[1],values[2],values[3],values[4],values[5],values[6],values[7],values[8]));
                        }
                        if(dispid==2 && (flags&4)){
                            if(count!=1)co_return finish(disp_bad_param_count);if(p->type!=1)co_return finish(e_fail);
                            const auto [status,value]=co_await as_long(0);if(status)co_return finish(disp_type_mismatch);
                            if((p=picture(base)))p->palette=static_cast<std::uint32_t>(value);co_return finish(s_ok);
                        }
                        if(!(flags&2) || (dispid!=0 && dispid!=2 && dispid!=3 && dispid!=4 && dispid!=5))co_return finish(disp_member_not_found);
                        if(count)co_return finish(disp_bad_param_count);
                        if(dispid==0)store_result(3,static_cast<std::uint32_t>(p->handle));else if(dispid==2)store_result(3,static_cast<std::uint32_t>(p->palette));
                        else if(dispid==3)store_result(2,static_cast<std::uint16_t>(p->type));else store_result(3,static_cast<std::uint32_t>(dispid==4?p->width:p->height));
                        co_return finish(s_ok);
                    }
                }else{
                    if(name=="GetClassID"){if(!call(1))co_return finish(e_pointer);m.write(GuestAddress{call(1)},clsid_std_picture);co_return finish(s_ok);}
                    if(name=="IsDirty")co_return finish(p->dirty?s_ok:s_false);
                    if(name=="Load"){
                        if(!call(1))co_return finish(e_pointer);
                        // 有 IPersistStream 標頭時只讀標頭宣告的長度，否則讀到串流結尾。
                        auto [status,head]=co_await read_stream(call(1),8);if(status)co_return finish(status);
                        std::vector<std::uint8_t> data=head;
                        if(head.size()==8 && le32(head,0)==persist_magic){auto [more,body]=co_await read_stream(call(1),le32(head,4));if(more)co_return finish(more);data.insert(data.end(),body.begin(),body.end());}
                        else if(head.size()==8){auto [more,body]=co_await read_stream(call(1),0);if(more)co_return finish(more);data.insert(data.end(),body.begin(),body.end());}
                        const auto loaded=co_await load_bytes(base,std::move(data),0,0,0);if(!loaded)if((p=picture(base)))p->dirty=false;
                        co_return finish(loaded);
                    }
                    if(name=="Save" || name=="GetSizeMax"){
                        auto [status,data]=co_await serialize(base,true);if(status)co_return finish(status);
                        if(name=="GetSizeMax"){if(!call(1))co_return finish(e_pointer);m.store(GuestAddress{call(1)},64,data.size()+8);co_return finish(s_ok);}
                        if(!call(1))co_return finish(e_pointer);
                        std::vector<std::uint8_t> header(8);put32(header,0,persist_magic);put32(header,4,static_cast<std::uint32_t>(data.size()));
                        if(const auto written=co_await write_stream(call(1),header))co_return finish(written);
                        if(const auto written=co_await write_stream(call(1),data))co_return finish(written);
                        if(static_cast<std::uint32_t>(call(2)))if((p=picture(base)))p->dirty=false;
                        co_return finish(s_ok);
                    }
                }
                throw HostCallFault("未知的圖片介面方法");
            }
            if(name=="OleCreatePictureIndirect"){
                // (lpPictDesc, riid, fOwn, lplpvObj)：PICTDESC 為 cbSizeofstruct、picType 與聯集（自位移 8 起）。
                const auto output=call(3);if(!output)co_return finish(e_pointer);m.store(GuestAddress{output},bits,0);
                const auto desc=call(0);std::int32_t type=-1;std::uint64_t handle=0,palette=0;std::int32_t xext=0,yext=0;
                if(desc){
                    const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{desc},32));type=long_arg(m.load(GuestAddress{desc+4},32));
                    const auto needed=type==1?8+2*unit:type==2?8+unit+8:type==3 || type==4?8+unit:8;
                    if(type<-1 || type>4 || size<needed)co_return finish(e_invalidarg);
                    if(type>0)handle=m.load(GuestAddress{desc+8},bits);
                    if(type==1)palette=m.load(GuestAddress{desc+8+unit},bits);
                    if(type==2){xext=long_arg(m.load(GuestAddress{desc+8+unit},32));yext=long_arg(m.load(GuestAddress{desc+12+unit},32));}
                }
                const auto object=create();auto* p=picture(object);
                p->type=static_cast<std::int16_t>(type);p->handle=handle;p->palette=palette;p->own=static_cast<std::uint32_t>(call(2))!=0;p->width=xext;p->height=yext;
                (void)co_await measure(object);
                co_return finish(co_await hand_out(object,call(1),output));
            }
            if(name=="OleLoadPicture" || name=="OleLoadPictureEx"){
                // (lpstream, lSize, fRunmode, riid, [xSizeDesired, ySizeDesired, dwFlags,] lplpvObj)；fRunmode 為真時不保留原始格式。
                const bool ex=name=="OleLoadPictureEx";const auto output=call(ex?7:4);if(!output)co_return finish(e_pointer);m.store(GuestAddress{output},bits,0);
                if(!call(0))co_return finish(e_pointer);
                const auto limit=static_cast<std::uint64_t>(std::max<std::int32_t>(0,long_arg(call(1))));
                co_return finish(co_await load_into_new(call(0),limit,!static_cast<std::uint32_t>(call(2)),call(3),output,ex?long_arg(call(4)):0,ex?long_arg(call(5)):0,ex?static_cast<std::uint32_t>(call(6)):0));
            }
            if(name=="OleLoadPicturePath" || name=="OleLoadPictureFile"){
                // 路徑：(szURLorPath, punkCaller, dwReserved, clrReserved, riid, ppvRet)；檔案：(VARIANT varFileName, LPDISPATCH*)。
                const bool file=name=="OleLoadPictureFile";const auto output=call(file?1:5);if(!output)co_return finish(e_pointer);m.store(GuestAddress{output},bits,0);
                std::u16string path;
                if(file){
                    auto variant=call(0);auto vt=static_cast<std::uint32_t>(m.load(GuestAddress{variant},16));
                    std::uint64_t text=0;if(vt==8)text=m.load(GuestAddress{variant+8},bits);else if(vt==(0x4000U|8U))text=m.load(GuestAddress{m.load(GuestAddress{variant+8},bits)},bits);else co_return finish(disp_type_mismatch);
                    if(text)path=read_utf16(m,GuestAddress{text},32768);
                    // 空檔名回傳空圖片（PICTYPE_NONE），與 Visual Basic 的 LoadPicture("") 相同。
                    if(path.empty()){const auto object=create();picture(object)->type=0;picture(object)->own=true;co_return finish(co_await hand_out(object,bytes(iid_picture_disp),output));}
                }else{if(!call(0))co_return finish(e_invalidarg);path=read_utf16(m,GuestAddress{call(0)},32768);}
                const auto stream_at=scratch(unit);
                auto status=co_await open_path(path,file?0:call(1),stream_at);
                if(status){if(file && (status==0x80070002U || status==0x80070003U))status=ctl_file_not_found;co_return finish(status);}
                const auto stream=m.load(GuestAddress{stream_at},bits);
                const auto riid=file?bytes(iid_picture_disp):call(4);
                const auto loaded=co_await load_into_new(stream,0,true,riid,output,0,0,0);
                (void)co_await com(stream,2,{});co_return finish(loaded);
            }
            if(name=="OleSavePictureFile"){
                // (LPDISPATCH lpdispPicture, BSTR bstrFileName)：JPEG／GIF 原始檔改存 BMP，其他沿用 SaveAsFile 的格式。
                if(!call(0) || !call(1))co_return finish(e_invalidarg);
                const auto self=call(0);const auto table=m.load(GuestAddress{self},bits);
                const auto base=table==ole_picture_vtables_[1]?self-unit:table==ole_picture_vtables_[0]?self:table==ole_picture_vtables_[2]?self-2*unit:0;
                std::vector<std::uint8_t> data;std::uint64_t foreign=0;
                if(base && picture(base)){
                    const auto* p=picture(base);const bool convert=p->format==format_jpeg || p->format==format_gif;
                    auto [status,serialized]=co_await serialize(base,!convert);if(status)co_return finish(status);data=std::move(serialized);
                }else{
                    const auto picture_at=scratch(unit);
                    if(const auto status=static_cast<std::uint32_t>(co_await com(self,0,{bytes(iid_picture),picture_at})))co_return finish(status);
                    foreign=m.load(GuestAddress{picture_at},bits);
                }
                const auto stream_at=scratch(unit);
                const auto status=static_cast<std::uint32_t>(co_await api("shlwapi.dll","SHCreateStreamOnFileW",{call(1),0x1011U,stream_at})); // STGM_CREATE|STGM_SHARE_EXCLUSIVE|STGM_WRITE
                if(status){if(foreign)(void)co_await com(foreign,2,{});co_return finish(status);}
                const auto stream=m.load(GuestAddress{stream_at},bits);std::uint32_t result=0;
                if(foreign){result=static_cast<std::uint32_t>(co_await com(foreign,15,{stream,1,scratch(4)}));(void)co_await com(foreign,2,{});}
                else result=co_await write_stream(stream,data);
                if(!result)result=static_cast<std::uint32_t>(co_await com(stream,8,{0})); // IStream::Commit
                (void)co_await com(stream,2,{});co_return finish(result);
            }
            if(name=="OleIconToCursor"){
                // (hinstExe, hIcon)：以圖示的點陣圖建立游標，熱點在中心。
                const auto parts=co_await icon_parts(call(1));if(!parts.valid)co_return finish(0);
                auto size=co_await bitmap_size(parts.color?parts.color:parts.mask);if(!parts.color)size.second/=2;
                const auto info=scratch(32);m.store(GuestAddress{info},32,0);m.store(GuestAddress{info+4},32,static_cast<std::uint32_t>(size.first/2));m.store(GuestAddress{info+8},32,static_cast<std::uint32_t>(size.second/2));
                m.store(GuestAddress{info+(x86_?12U:16U)},bits,parts.mask);m.store(GuestAddress{info+(x86_?16U:24U)},bits,parts.color);
                const auto cursor=co_await api("user32.dll","CreateIconIndirect",{info});(void)co_await drop_parts(parts);co_return finish(cursor);
            }
        }catch(const MemoryFault&){co_return finish(family=="oleaut32.dll" && name=="OleIconToCursor"?0:e_invalidarg);}
        catch(const std::bad_alloc&){co_return finish(name=="OleIconToCursor"?0:e_outofmemory);}
        throw HostCallFault("未知的 oleaut32 圖片服務");
    });
}
}
