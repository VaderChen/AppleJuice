#include "x64arm/windows_services.hpp"
#include "texture_load.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <map>
#include <optional>

namespace x64arm {
// GDI+ 平面 API：啟動／關閉、記憶體配置、記憶體點陣圖與繪製到 HDC。
// 點陣圖像素保留在客體記憶體（Scan0 由呼叫端提供或由此配置），繪製經共用 raster surface 合成。
// 參考：https://learn.microsoft.com/windows/win32/gdiplus/-gdiplus-flatapi-flat
struct GdiplusState {
    // LockBits 期間的暫存區：格式相同時直接指向像素，否則轉換到暫存緩衝並於 UnlockBits 寫回。
    struct Lock {std::uint64_t buffer{};std::uint32_t mode{},format{};std::array<std::int32_t,4> rect{};std::int64_t stride{};bool owned{};};
    struct Bitmap {std::uint32_t width{},height{},format{};std::int64_t stride{};std::uint64_t scan0{};bool owned{};std::optional<Lock> lock{};};
    std::map<std::uint64_t,Bitmap> bitmaps;
    std::map<std::uint64_t,std::uint64_t> graphics; // GpGraphics → HDC。
    // GdipGetImageGraphicsContext：以 32bpp 預乘 DIB section 承接繪圖，刪除 Graphics 時寫回點陣圖。
    struct ImageTarget {std::uint64_t bitmap{},dc{},dib{},previous{},bits{};};
    std::map<std::uint64_t,ImageTarget> image_graphics;
    std::uint64_t next_token{1};
    unsigned startups{};
    // GdipLoadImageFromStream：以 IStream::Read 分段讀入，完成後由共用影像解碼器轉成 32bppARGB。
    struct StreamLoad {std::uint64_t stream{},output{},buffer{};std::vector<std::uint8_t> bytes;};
    std::map<std::uint64_t,StreamLoad> loads;std::uint64_t next_load{1};
};
namespace {
// GpStatus。
constexpr std::uint32_t ok=0,generic_error=1,invalid_parameter=2,out_of_memory=3,object_busy=4,insufficient_buffer=5,not_implemented=6,wrong_state=8,unknown_image_format=13,unsupported_version=17,not_initialized=18;
constexpr std::uint32_t stream_chunk=65536;constexpr std::size_t stream_limit=64U*1024U*1024U;
// PixelFormat：32bppARGB、32bppPARGB、32bppRGB、24bppRGB。
constexpr std::uint32_t argb32=0x0026200aU,pargb32=0x000e200bU,rgb32=0x00022009U,rgb24=0x00021808U;
unsigned pixel_bytes(std::uint32_t format){return format==rgb24?3U:format==argb32 || format==pargb32 || format==rgb32?4U:0U;}
std::uint32_t premultiply(std::uint32_t argb){
    const auto a=argb>>24;if(a==255)return argb;
    const auto scale=[&](unsigned shift){return ((((argb>>shift)&255U)*a+127U)/255U)<<shift;};
    return (a<<24)|scale(16)|scale(8)|scale(0);
}
std::uint32_t unpremultiply(std::uint32_t pargb){
    const auto a=pargb>>24;if(a==255 || !a)return a?pargb:0;
    const auto scale=[&](unsigned shift){return std::min(255U,(((pargb>>shift)&255U)*255U+a/2U)/a)<<shift;};
    return (a<<24)|scale(16)|scale(8)|scale(0);
}
}
std::span<const WindowsServiceDescriptor> gdiplus_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdiplus.dll","GdiplusStartup",3},{"gdiplus.dll","GdiplusShutdown",1},
        {"gdiplus.dll","GdiplusNotificationHook",1},{"gdiplus.dll","GdiplusNotificationUnhook",1},
        {"gdiplus.dll","GdipAlloc",1},{"gdiplus.dll","GdipFree",1},
        {"gdiplus.dll","GdipCreateBitmapFromScan0",6},{"gdiplus.dll","GdipBitmapSetPixel",4},{"gdiplus.dll","GdipBitmapGetPixel",4},
        {"gdiplus.dll","GdipCloneImage",2},{"gdiplus.dll","GdipDisposeImage",1},
        {"gdiplus.dll","GdipGetImageWidth",2},{"gdiplus.dll","GdipGetImageHeight",2},{"gdiplus.dll","GdipGetImagePixelFormat",2},
        {"gdiplus.dll","GdipCreateFromHDC",2},{"gdiplus.dll","GdipDeleteGraphics",1},
        {"gdiplus.dll","GdipDrawImageI",4},{"gdiplus.dll","GdipDrawImageRectI",6},
        {"gdiplus.dll","GdipLoadImageFromStream",2},{"gdiplus.dll","GdipLoadImageFromStreamICM",2},
        {"gdiplus.dll","GdipCreateBitmapFromStream",2},{"gdiplus.dll","GdipCreateBitmapFromStreamICM",2},
        {"gdiplus.dll","GdipBitmapLockBits",5},{"gdiplus.dll","GdipBitmapUnlockBits",2},
        {"gdiplus.dll","GdipGetImagePaletteSize",2},{"gdiplus.dll","GdipGetImagePalette",3},{"gdiplus.dll","GdipGetImageGraphicsContext",2}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_gdiplus(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    if(!gdiplus_)gdiplus_=std::make_shared<GdiplusState>();
    else if(gdiplus_.use_count()>1)gdiplus_=std::make_shared<GdiplusState>(*gdiplus_);
    auto& g=*gdiplus_;
    const auto pointer_bits=static_cast<std::uint8_t>(x86_?32:64);
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto u32=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const auto s32=[&](unsigned n){return std::bit_cast<std::int32_t>(u32(n));};
    // 物件 handle 指向客體堆積中的小區塊，保證唯一且是有效位址；客體只當作不透明指標傳回。
    const auto new_handle=[&]{return allocate_heap(memory,16);};
    const auto release_handle=[&](std::uint64_t handle){(void)free_heap(memory,handle,state);};
    try{
        if(symbol=="GdipAlloc")return finish(allocate_heap(memory,std::max<std::uint64_t>(arg(0),1)));
        if(symbol=="GdipFree"){if(arg(0))(void)free_heap(memory,arg(0),state);return finish(0);}
        if(symbol=="GdiplusStartup"){
            // GdiplusStartupInput：版本、DebugEventCallback、SuppressBackgroundThread、SuppressExternalCodecs。
            if(!arg(0) || !arg(1))return finish(invalid_parameter);
            const auto version=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));
            if(version<1 || version>3)return finish(unsupported_version);
            const auto suppress=memory.load(GuestAddress{arg(1)+(x86_?8U:16U)},32)!=0;
            if(suppress){
                // 背景執行緒由呼叫端驅動：輸出 NotificationHook／Unhook 函式指標。
                if(!arg(2))return finish(invalid_parameter);
                memory.store(GuestAddress{arg(2)},pointer_bits,bind_import("gdiplus.dll","GdiplusNotificationHook",true).value);
                memory.store(GuestAddress{arg(2)+(x86_?4U:8U)},pointer_bits,bind_import("gdiplus.dll","GdiplusNotificationUnhook",true).value);
            }
            memory.store(GuestAddress{arg(0)},pointer_bits,g.next_token++);++g.startups;return finish(ok);
        }
        if(symbol=="GdiplusShutdown"){if(g.startups)--g.startups;return finish(0);}
        if(symbol=="GdiplusNotificationHook"){if(!arg(0))return finish(invalid_parameter);memory.store(GuestAddress{arg(0)},pointer_bits,g.next_token++);return finish(ok);}
        if(symbol=="GdiplusNotificationUnhook")return finish(0);
        if(!g.startups)return finish(not_initialized);
        // 解碼後的影像固定是點陣圖；CreateBitmapFromStream 與 LoadImageFromStream 共用同一 IStream 讀取流程。
        if(symbol=="GdipLoadImageFromStream" || symbol=="GdipLoadImageFromStreamICM" || symbol=="GdipCreateBitmapFromStream" || symbol=="GdipCreateBitmapFromStreamICM"){
            if(!arg(0) || !arg(1))return finish(invalid_parameter);
            memory.store(GuestAddress{arg(1)},pointer_bits,0);
            const auto buffer=allocate_heap(memory,stream_chunk+8);if(!buffer)return finish(out_of_memory);
            const auto id=g.next_load++;g.loads.emplace(id,GdiplusState::StreamLoad{arg(0),arg(1),buffer,{}});
            const auto table=memory.load(GuestAddress{arg(0)},pointer_bits),read=memory.load(GuestAddress{table+(x86_?12U:24U)},pointer_bits);
            memory.store(GuestAddress{buffer+stream_chunk},32,0);
            start_callbacks(state,memory,{{read,{arg(0),buffer,stream_chunk,buffer+stream_chunk}}},Continuation::GdiplusStream,id);return std::nullopt;
        }
        if(symbol=="GdipCreateBitmapFromScan0"){
            const auto width=s32(0),height=s32(1),stride=s32(2);const auto format=u32(3);const auto scan0=arg(4);
            if(!arg(5) || width<=0 || height<=0)return finish(invalid_parameter);
            const auto bytes=pixel_bytes(format);if(!bytes)return finish(not_implemented);
            GdiplusState::Bitmap bitmap{static_cast<std::uint32_t>(width),static_cast<std::uint32_t>(height),format,stride,scan0,false};
            if(!scan0){
                // 自行配置時 stride 對齊 4 bytes 並清零，等同完全透明的黑色。
                bitmap.stride=(static_cast<std::int64_t>(width)*bytes+3)/4*4;
                const auto size=static_cast<std::uint64_t>(bitmap.stride)*static_cast<std::uint64_t>(height);
                bitmap.scan0=allocate_heap(memory,size);if(!bitmap.scan0)return finish(out_of_memory);
                memory.fill(GuestAddress{bitmap.scan0},0,static_cast<std::size_t>(size));bitmap.owned=true;
            }else if(std::abs(stride)<static_cast<std::int64_t>(width)*bytes)return finish(invalid_parameter);
            const auto handle=new_handle();if(!handle){if(bitmap.owned)(void)free_heap(memory,bitmap.scan0,state);return finish(out_of_memory);}
            g.bitmaps[handle]=bitmap;memory.store(GuestAddress{arg(5)},pointer_bits,handle);return finish(ok);
        }
        const auto bitmap_of=[&](std::uint64_t handle)->GdiplusState::Bitmap*{const auto found=g.bitmaps.find(handle);return found==g.bitmaps.end()?nullptr:&found->second;};
        const auto address_of=[&](const GdiplusState::Bitmap& b,std::uint32_t x,std::uint32_t y){
            return b.scan0+static_cast<std::uint64_t>(static_cast<std::int64_t>(y)*b.stride)+static_cast<std::uint64_t>(x)*pixel_bytes(b.format);
        };
        // 以 0xAARRGGBB 讀取一個像素（非預乘）。
        const auto read_pixel=[&](const GdiplusState::Bitmap& b,std::uint32_t x,std::uint32_t y)->std::uint32_t{
            const auto at=address_of(b,x,y);
            if(b.format==rgb24){const auto bgr=static_cast<std::uint32_t>(memory.load(GuestAddress{at},16))|(static_cast<std::uint32_t>(memory.load(GuestAddress{at+2},8))<<16);return 0xff000000U|bgr;}
            const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));
            return b.format==rgb32?(value|0xff000000U):b.format==pargb32?unpremultiply(value):value;
        };
        // 以 0xAARRGGBB（非預乘）寫入一個像素。
        const auto store_pixel=[&](std::uint32_t format,std::uint64_t at,std::uint32_t color){
            if(format==rgb24){memory.store(GuestAddress{at},16,color&0xffffU);memory.store(GuestAddress{at+2},8,(color>>16)&255U);}
            else memory.store(GuestAddress{at},32,format==pargb32?premultiply(color):format==rgb32?(color|0xff000000U):color);
        };
        const auto load_pixel=[&](std::uint32_t format,std::uint64_t at)->std::uint32_t{
            if(format==rgb24)return 0xff000000U|static_cast<std::uint32_t>(memory.load(GuestAddress{at},16))|(static_cast<std::uint32_t>(memory.load(GuestAddress{at+2},8))<<16);
            const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));return format==rgb32?(value|0xff000000U):format==pargb32?unpremultiply(value):value;
        };
        if(symbol=="GdipBitmapLockBits"){
            auto* b=bitmap_of(arg(0));const auto data=arg(4);if(!b || !data)return finish(invalid_parameter);if(b->lock)return finish(wrong_state);
            std::array<std::int32_t,4> rect{0,0,static_cast<std::int32_t>(b->width),static_cast<std::int32_t>(b->height)};
            if(arg(1))for(unsigned i=0;i<4;++i)rect[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+i*4},32));
            const auto mode=u32(2),format=u32(3);
            if(rect[0]<0 || rect[1]<0 || rect[2]<=0 || rect[3]<=0 || static_cast<std::int64_t>(rect[0])+rect[2]>b->width || static_cast<std::int64_t>(rect[1])+rect[3]>b->height || !(mode&3U) || (mode&~7U))return finish(invalid_parameter);
            const auto bytes=pixel_bytes(format);if(!bytes)return finish(not_implemented);
            const unsigned data_size=x86_?24U:32U;memory.check(GuestAddress{data},data_size,Access::Write);
            GdiplusState::Lock lock{0,mode,format,rect,0,false};
            if(mode&4U){lock.stride=static_cast<std::int32_t>(memory.load(GuestAddress{data+8},32));lock.buffer=memory.load(GuestAddress{data+16},pointer_bits);
                if(!lock.buffer || std::abs(lock.stride)<static_cast<std::int64_t>(rect[2])*bytes)return finish(invalid_parameter);}
            else if(format==b->format){lock.buffer=address_of(*b,static_cast<std::uint32_t>(rect[0]),static_cast<std::uint32_t>(rect[1]));lock.stride=b->stride;}
            else {lock.stride=(static_cast<std::int64_t>(rect[2])*bytes+3)/4*4;lock.buffer=allocate_heap(memory,static_cast<std::uint64_t>(lock.stride)*static_cast<std::uint32_t>(rect[3]));if(!lock.buffer)return finish(out_of_memory);lock.owned=true;}
            const bool direct=!(mode&4U) && format==b->format;
            if((mode&1U) && !direct)
                for(std::int32_t y=0;y<rect[3];++y)for(std::int32_t x=0;x<rect[2];++x)
                    store_pixel(format,lock.buffer+static_cast<std::uint64_t>(y*lock.stride)+static_cast<std::uint64_t>(x)*bytes,read_pixel(*b,static_cast<std::uint32_t>(rect[0]+x),static_cast<std::uint32_t>(rect[1]+y)));
            memory.store(GuestAddress{data},32,static_cast<std::uint32_t>(rect[2]));memory.store(GuestAddress{data+4},32,static_cast<std::uint32_t>(rect[3]));
            memory.store(GuestAddress{data+8},32,static_cast<std::uint32_t>(lock.stride));memory.store(GuestAddress{data+12},32,format);
            memory.store(GuestAddress{data+16},pointer_bits,lock.buffer);memory.store(GuestAddress{data+(x86_?20U:24U)},pointer_bits,0);
            b->lock=lock;return finish(ok);
        }
        if(symbol=="GdipBitmapUnlockBits"){
            auto* b=bitmap_of(arg(0));if(!b || !arg(1))return finish(invalid_parameter);if(!b->lock)return finish(wrong_state);
            const auto lock=*b->lock;b->lock.reset();const bool direct=!(lock.mode&4U) && lock.format==b->format;const auto bytes=pixel_bytes(lock.format);
            if((lock.mode&2U) && !direct)
                for(std::int32_t y=0;y<lock.rect[3];++y)for(std::int32_t x=0;x<lock.rect[2];++x)
                    store_pixel(b->format,address_of(*b,static_cast<std::uint32_t>(lock.rect[0]+x),static_cast<std::uint32_t>(lock.rect[1]+y)),load_pixel(lock.format,lock.buffer+static_cast<std::uint64_t>(y*lock.stride)+static_cast<std::uint64_t>(x)*bytes));
            if(lock.owned)(void)free_heap(memory,lock.buffer,state);return finish(ok);
        }
        if(symbol=="GdipGetImagePaletteSize" || symbol=="GdipGetImagePalette"){
            // 目前支援的像素格式皆非索引色；ColorPalette 只有 Flags 與 Count=0。
            if(!bitmap_of(arg(0)) || !arg(1))return finish(invalid_parameter);
            if(symbol=="GdipGetImagePaletteSize"){memory.store(GuestAddress{arg(1)},32,12);return finish(ok);}
            if(s32(2)<8)return finish(insufficient_buffer);memory.store(GuestAddress{arg(1)},32,0);memory.store(GuestAddress{arg(1)+4},32,0);return finish(ok);
        }
        if(symbol=="GdipGetImageGraphicsContext"){
            auto* b=bitmap_of(arg(0));if(!b || !arg(1))return finish(invalid_parameter);if(b->lock)return finish(object_busy);
            const auto dc=nested_service(state,memory,"gdi32.dll","CreateCompatibleDC",{0});if(!dc)return finish(out_of_memory);
            const auto header=allocate_heap(memory,48+8);if(!header){(void)nested_service(state,memory,"gdi32.dll","DeleteDC",{dc});return finish(out_of_memory);}
            memory.fill(GuestAddress{header},0,56);memory.store(GuestAddress{header},32,40);memory.store(GuestAddress{header+4},32,b->width);memory.store(GuestAddress{header+8},32,static_cast<std::uint32_t>(-static_cast<std::int32_t>(b->height)));
            memory.store(GuestAddress{header+12},16,1);memory.store(GuestAddress{header+14},16,32);
            const auto dib=nested_service(state,memory,"gdi32.dll","CreateDIBSection",{dc,header,0,header+48,0,0});const auto bits=memory.load(GuestAddress{header+48},pointer_bits);(void)free_heap(memory,header,state);
            if(!dib || !bits){(void)nested_service(state,memory,"gdi32.dll","DeleteDC",{dc});return finish(out_of_memory);}
            for(std::uint32_t y=0;y<b->height;++y)for(std::uint32_t x=0;x<b->width;++x)memory.store(GuestAddress{bits+(static_cast<std::uint64_t>(y)*b->width+x)*4},32,premultiply(read_pixel(*b,x,y)));
            const auto previous=nested_service(state,memory,"gdi32.dll","SelectObject",{dc,dib});
            const auto handle=new_handle();if(!handle){(void)nested_service(state,memory,"gdi32.dll","SelectObject",{dc,previous});(void)nested_service(state,memory,"gdi32.dll","DeleteObject",{dib});(void)nested_service(state,memory,"gdi32.dll","DeleteDC",{dc});return finish(out_of_memory);}
            g.graphics[handle]=dc;g.image_graphics[handle]={arg(0),dc,dib,previous,bits};memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);
        }
        if(symbol=="GdipBitmapSetPixel" || symbol=="GdipBitmapGetPixel"){
            auto* b=bitmap_of(arg(0));if(!b)return finish(invalid_parameter);
            const auto x=s32(1),y=s32(2);
            if(x<0 || y<0 || static_cast<std::uint32_t>(x)>=b->width || static_cast<std::uint32_t>(y)>=b->height)return finish(invalid_parameter);
            if(symbol=="GdipBitmapGetPixel"){if(!arg(3))return finish(invalid_parameter);memory.store(GuestAddress{arg(3)},32,read_pixel(*b,static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y)));return finish(ok);}
            const auto color=u32(3);const auto at=address_of(*b,static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y));
            if(b->format==rgb24){memory.store(GuestAddress{at},16,color&0xffffU);memory.store(GuestAddress{at+2},8,(color>>16)&255U);}
            else memory.store(GuestAddress{at},32,b->format==pargb32?premultiply(color):b->format==rgb32?(color|0xff000000U):color);
            return finish(ok);
        }
        if(symbol=="GdipGetImageWidth" || symbol=="GdipGetImageHeight" || symbol=="GdipGetImagePixelFormat"){
            const auto* b=bitmap_of(arg(0));if(!b || !arg(1))return finish(invalid_parameter);
            memory.store(GuestAddress{arg(1)},32,symbol=="GdipGetImageWidth"?b->width:symbol=="GdipGetImageHeight"?b->height:b->format);return finish(ok);
        }
        if(symbol=="GdipCloneImage"){
            const auto* b=bitmap_of(arg(0));if(!b || !arg(1))return finish(invalid_parameter);
            GdiplusState::Bitmap copy=*b;copy.stride=(static_cast<std::int64_t>(b->width)*pixel_bytes(b->format)+3)/4*4;
            const auto size=static_cast<std::uint64_t>(copy.stride)*b->height;
            copy.scan0=allocate_heap(memory,size);if(!copy.scan0)return finish(out_of_memory);copy.owned=true;
            std::vector<std::uint8_t> row(static_cast<std::size_t>(b->width)*pixel_bytes(b->format));
            for(std::uint32_t y=0;y<b->height;++y){memory.read(GuestAddress{address_of(*b,0,y)},row);memory.write(GuestAddress{copy.scan0+static_cast<std::uint64_t>(copy.stride)*y},row);}
            const auto handle=new_handle();if(!handle){(void)free_heap(memory,copy.scan0,state);return finish(out_of_memory);}
            g.bitmaps[handle]=copy;memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);
        }
        if(symbol=="GdipDisposeImage"){
            const auto found=g.bitmaps.find(arg(0));if(found==g.bitmaps.end())return finish(invalid_parameter);
            if(found->second.owned)(void)free_heap(memory,found->second.scan0,state);
            g.bitmaps.erase(found);release_handle(arg(0));return finish(ok);
        }
        if(symbol=="GdipCreateFromHDC"){
            if(!arg(1) || !gui_.contexts.contains(arg(0)))return finish(invalid_parameter);
            const auto handle=new_handle();if(!handle)return finish(out_of_memory);
            g.graphics[handle]=arg(0);memory.store(GuestAddress{arg(1)},pointer_bits,handle);return finish(ok);
        }
        if(symbol=="GdipDeleteGraphics"){
            if(const auto target=g.image_graphics.find(arg(0));target!=g.image_graphics.end()){
                // 先讓 GPU raster 寫回 DIB backing，再把預乘像素轉回點陣圖本身的格式。
                const auto image=target->second;g.image_graphics.erase(target);(void)nested_service(state,memory,"gdi32.dll","GdiFlush",{});
                if(auto* b=bitmap_of(image.bitmap))
                    for(std::uint32_t y=0;y<b->height;++y)for(std::uint32_t x=0;x<b->width;++x)
                        {const auto raw=static_cast<std::uint32_t>(memory.load(GuestAddress{image.bits+(static_cast<std::uint64_t>(y)*b->width+x)*4},32));store_pixel(b->format,address_of(*b,x,y),b->format==argb32 || b->format==pargb32?unpremultiply(raw):raw|0xff000000U);}
                (void)nested_service(state,memory,"gdi32.dll","SelectObject",{image.dc,image.previous});(void)nested_service(state,memory,"gdi32.dll","DeleteObject",{image.dib});(void)nested_service(state,memory,"gdi32.dll","DeleteDC",{image.dc});
            }
            if(!g.graphics.erase(arg(0)))return finish(invalid_parameter);
            release_handle(arg(0));return finish(ok);
        }
        if(symbol=="GdipDrawImageI" || symbol=="GdipDrawImageRectI"){
            const auto graphics=g.graphics.find(arg(0));const auto* b=bitmap_of(arg(1));
            if(graphics==g.graphics.end() || !b)return finish(invalid_parameter);
            const auto dc=gui_.contexts.find(graphics->second);if(dc==gui_.contexts.end())return finish(invalid_parameter);
            const auto x=s32(2),y=s32(3);
            const auto width=symbol=="GdipDrawImageRectI"?s32(4):static_cast<std::int32_t>(b->width);
            const auto height=symbol=="GdipDrawImageRectI"?s32(5):static_cast<std::int32_t>(b->height);
            if(!width || !height)return finish(ok);
            // 預設合成模式為 SourceOver：ARGB 先轉成預乘後以 AC_SRC_ALPHA 混合；不透明格式直接複製。
            const bool alpha=b->format==argb32 || b->format==pargb32;
            std::vector<std::uint32_t> pixels(static_cast<std::size_t>(b->width)*b->height);
            for(std::uint32_t row=0;row<b->height;++row)for(std::uint32_t column=0;column<b->width;++column){
                const auto value=read_pixel(*b,column,row);
                pixels[static_cast<std::size_t>(row)*b->width+column]=alpha?premultiply(value):value;
            }
            const auto destination=raster_surface(memory,graphics->second);if(!destination)return finish(not_implemented);
            const auto source=make_hardware_raster(b->width,b->height,pixels);
            auto target=dc->second;target.stretch_mode=3;
            RasterBlit options;options.operation=0xcc;
            destination->blit(*source,{x,y,x+width,y+height},{0,0,static_cast<std::int32_t>(b->width),static_cast<std::int32_t>(b->height)},target,
                              alpha?0x01ff0000U:UINT32_MAX,options);
            return finish(ok);
        }
    }catch(const MemoryFault&){return finish(invalid_parameter);}catch(const std::bad_alloc&){return finish(out_of_memory);}
    return finish(not_implemented);
}
std::optional<std::uint32_t> WindowsServices::resume_gdiplus_stream(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t value){
    if(!gdiplus_){complete_call(state,memory,generic_error);return std::nullopt;}
    if(gdiplus_.use_count()>1)gdiplus_=std::make_shared<GdiplusState>(*gdiplus_);
    auto& g=*gdiplus_;const auto found=g.loads.find(id);if(found==g.loads.end()){complete_call(state,memory,generic_error);return std::nullopt;}
    auto& load=found->second;const auto pointer_bits=static_cast<std::uint8_t>(x86_?32:64);
    const auto finish=[&](std::uint32_t status){(void)free_heap(memory,load.buffer,state);g.loads.erase(found);complete_call(state,memory,status);return std::optional<std::uint32_t>{};};
    try{
        const auto status=static_cast<std::uint32_t>(value);if(status&0x80000000U)return finish(generic_error);
        const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{load.buffer+stream_chunk},32));
        if(count>stream_chunk || load.bytes.size()+count>stream_limit)return finish(count>stream_chunk?generic_error:out_of_memory);
        if(count){const auto at=load.bytes.size();load.bytes.resize(at+count);memory.read(GuestAddress{load.buffer},std::span(load.bytes).subspan(at));}
        if(count && status==0){
            // 尚未到資料結尾，繼續讀下一段。
            const auto table=memory.load(GuestAddress{load.stream},pointer_bits),read=memory.load(GuestAddress{table+(x86_?12U:24U)},pointer_bits);
            memory.store(GuestAddress{load.buffer+stream_chunk},32,0);
            start_callbacks(state,memory,{{read,{load.stream,load.buffer,stream_chunk,load.buffer+stream_chunk}}},Continuation::GdiplusStream,id);return std::nullopt;
        }
        texture_image::Decoded image;
        try{image=texture_image::decode(load.bytes);}catch(const texture_image::Error&){return finish(unknown_image_format);}
        if(image.desc.format!=28 || image.pixels.empty())return finish(unknown_image_format);
        const auto width=image.desc.width,height=image.desc.height;auto& pixels=image.pixels.front();
        // RGBA（非預乘）轉成 GDI+ 32bppARGB 的 BGRA 記憶體順序。
        for(std::size_t i=0;i+3<pixels.size();i+=4)std::swap(pixels[i],pixels[i+2]);
        GdiplusState::Bitmap bitmap{width,height,argb32,static_cast<std::int64_t>(width)*4,0,true};
        bitmap.scan0=allocate_heap(memory,pixels.size());if(!bitmap.scan0)return finish(out_of_memory);
        memory.write(GuestAddress{bitmap.scan0},pixels);
        const auto handle=allocate_heap(memory,16);if(!handle){(void)free_heap(memory,bitmap.scan0,state);return finish(out_of_memory);}
        g.bitmaps[handle]=bitmap;memory.store(GuestAddress{load.output},pointer_bits,handle);return finish(ok);
    }catch(const MemoryFault&){return finish(invalid_parameter);}catch(const std::bad_alloc&){return finish(out_of_memory);}
}
void WindowsServices::discard_gdiplus_stream(GuestState& state,GuestMemory& memory,std::uint64_t id){
    if(!gdiplus_)return;const auto found=gdiplus_->loads.find(id);if(found==gdiplus_->loads.end())return;
    (void)free_heap(memory,found->second.buffer,state);gdiplus_->loads.erase(found);
}
}
