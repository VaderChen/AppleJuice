#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_transform.hpp"
#include "rounded_region.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> graphics_compat_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","CreateRectRgn",4},{"gdi32.dll","CreateRectRgnIndirect",1},{"gdi32.dll","SetRectRgn",5},
        {"gdi32.dll","CreateRoundRectRgn",6},{"gdi32.dll","CreateEllipticRgn",4},{"gdi32.dll","CreateEllipticRgnIndirect",1},
        {"gdi32.dll","CombineRgn",4},{"gdi32.dll","OffsetRgn",3},{"gdi32.dll","GetRgnBox",2},
        {"gdi32.dll","PtInRegion",3},{"gdi32.dll","RectInRegion",2},{"gdi32.dll","EqualRgn",2},{"gdi32.dll","GetRegionData",3},
        {"user32.dll","GetWindowRgn",2},{"user32.dll","GetWindowRgnBox",2},{"user32.dll","SetWindowRgn",3},
        {"user32.dll","RedrawWindow",4},{"user32.dll","ValidateRect",2},
        {"user32.dll","CopyRect",2},{"user32.dll","SetRect",5},{"user32.dll","SetRectEmpty",1},
        {"user32.dll","EqualRect",2},{"user32.dll","IsRectEmpty",1},{"user32.dll","PtInRect",2,2},
        {"user32.dll","InflateRect",3},{"user32.dll","IntersectRect",3},{"user32.dll","UnionRect",3},{"user32.dll","SubtractRect",3},
        { "user32.dll","AdjustWindowRect",3},{"user32.dll","AdjustWindowRectEx",4},{"user32.dll","AdjustWindowRectExForDpi",5},
        {"user32.dll","GetWindowLongA",2},{"user32.dll","GetWindowLongW",2},
        {"user32.dll","SetWindowLongA",3},{"user32.dll","SetWindowLongW",3},
        {"user32.dll","GetUpdateRect",3},
        {"user32.dll","GetWindow",2},{"user32.dll","GetTopWindow",1},
        {"user32.dll","PrintWindow",3},{"user32.dll","UpdateLayeredWindow",9},{"user32.dll","UpdateLayeredWindowIndirect",2},
        {"user32.dll","SetLayeredWindowAttributes",4},{"user32.dll","GetLayeredWindowAttributes",4},
        {"user32.dll","UnregisterClassA",2},{"user32.dll","UnregisterClassW",2}
    };
    return table;
}

void WindowsServices::clear_layered_content(std::uint64_t handle){
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end() || found->second.layered_pixels.empty())return;
    found->second.layered_pixels.clear();rasters_.erase(0x10000000U+handle);if(native_gui_)native_gui_->graphics_surface(handle,{});
}
std::optional<std::uint32_t> WindowsServices::invoke_graphics_compat(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(invoke_remote_gui(state,memory,service))return std::nullopt;
    const WindowsCall arg(state,memory,service);
    const auto symbol=service.symbol;
    std::uint64_t result{};
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    if(symbol=="CreateRoundRectRgn" || symbol=="CreateEllipticRgn" || symbol=="CreateEllipticRgnIndirect"){
        try{
            GuiRect rect{};for(unsigned i=0;i<4;++i)rect[i]=static_cast<std::int32_t>(symbol.ends_with("Indirect")?memory.load(GuestAddress{arg(0)+i*4},32):arg(i));
            const auto ew=symbol=="CreateRoundRectRgn"?static_cast<std::int32_t>(arg(4)):static_cast<std::int64_t>(rect[2])-rect[0];
            const auto eh=symbol=="CreateRoundRectRgn"?static_cast<std::int32_t>(arg(5)):static_cast<std::int64_t>(rect[3])-rect[1];
            auto region=rounded_region(rect,ew,eh);if(gui_.regions.size()>=65536)fail(8);else {result=gui_.next_object++;gui_.regions.emplace(result,std::move(region));}
        }catch(const std::invalid_argument&){fail(87);}catch(const std::bad_alloc&){fail(8);}catch(const std::length_error&){fail(8);}
    }else if(symbol=="PrintWindow"){
        const auto done=[&](std::uint32_t error){if(error)last_error_=error;arg.return_to(state,error?0:1);return std::optional<std::uint32_t>{};};
        const auto found=gui_.windows.find(arg(0));if(found==gui_.windows.end())return done(1400);const auto flags=static_cast<std::uint32_t>(arg(2));if(flags&~3U)return done(87);
        if(found->second.display_affinity==0x11U)return done(5);
        if(!gui_.contexts.contains(arg(1)) && !gui_.dc_objects.contains(arg(1)) && !(arg(1)>=0x10000000U && gui_.windows.contains(arg(1)-0x10000000U)))return done(6);
        const auto& window=found->second;if(window.thread!=threads_.current)return done(50);
        if(window.display_affinity==1){
            try{
                gui_.contexts.try_emplace(arg(1));const auto target=raster_surface(memory,arg(1));if(!target)return done(50);
                const auto size=flags&1U?std::array<std::uint32_t,2>{window.width,window.height}:gui_outer_size(window);
                std::vector<std::uint32_t> pixels(static_cast<std::size_t>(size[0])*size[1]);
                auto black=make_hardware_raster(size[0],size[1],pixels);const GuiRect rectangle{0,0,static_cast<std::int32_t>(size[0]),static_cast<std::int32_t>(size[1])};
                target->blit(*black,rectangle,rectangle,gui_.contexts.at(arg(1)),UINT32_MAX);return done(0);
            }catch(const std::bad_alloc&){return done(8);}catch(const std::runtime_error&){return done(50);}
        }
        if(window.procedure && stock_gui_class(window.procedure).empty()){
            start_callbacks(state,memory,{{window.procedure,{arg(0),flags&1U?0x318U:0x317U,arg(1),flags&1U?28U:30U}}},Continuation::Return,1);return {};
        }
        const auto frame=gui_frame_insets(window);if(!native_gui_ || (!(flags&1U) && (frame.left || frame.top || frame.right || frame.bottom)))return done(50);
        try{
            const auto image=native_gui_->capture_client(arg(0));if(!image)return done(50);gui_.contexts.try_emplace(arg(1));const auto destination=raster_surface(memory,arg(1));if(!destination)return done(50);
            const auto source=make_hardware_raster(image->width,image->height,image->pixels);const auto rectangle=std::array<std::int32_t,4>{0,0,static_cast<std::int32_t>(image->width),static_cast<std::int32_t>(image->height)};
            destination->blit(*source,rectangle,rectangle,gui_.contexts.at(arg(1)),UINT32_MAX);return done(0);
        }catch(const std::bad_alloc&){return done(8);}catch(const std::runtime_error&){return done(50);}
    }else if(symbol=="UpdateLayeredWindow" || symbol=="UpdateLayeredWindowIndirect"){
        const auto done=[&](std::uint32_t error){if(error)last_error_=error;arg.return_to(state,error?0:1);return std::optional<std::uint32_t>{};};
        const auto window=gui_.windows.find(arg(0));if(window==gui_.windows.end())return done(1400);
        auto& w=window->second;if(!(w.ex_style&0x80000U) || w.layered_initialized)return done(87);
        std::array<std::uint64_t,10> values{};values[0]=arg(0);
        if(symbol.ends_with("Indirect")){
            if(!arg(1))return done(87);const auto info=arg(1);const unsigned size=x86_?40:80;memory.check(GuestAddress{info},size,Access::Read);if(memory.load(GuestAddress{info},32)!=size)return done(87);
            const std::array<unsigned,9> offsets=x86_?std::array<unsigned,9>{4,8,12,16,20,24,28,32,36}:std::array<unsigned,9>{8,16,24,32,40,48,56,64,72};
            for(unsigned i=1;i<10;++i)values[i]=memory.load(GuestAddress{info+offsets[i-1]},i==6 || i==8?32:x86_?32:64);
            if(values[9])return done(50); // dirty RECT 的局部提交另行實作。
        }else for(unsigned i=1;i<9;++i)values[i]=arg(i);
        const auto flags=static_cast<std::uint32_t>(values[8]),mode=flags&7U;
        if((flags&~15U) || (mode && (mode&(mode-1))))return done(87);
        const auto frame=gui_frame_insets(w);if(w.builtin || w.dialog || w.combo_owner || frame.left || frame.top || frame.right || frame.bottom)return done(50);
        const auto pair=[&](std::uint64_t at){return std::array<std::int32_t,2>{static_cast<std::int32_t>(memory.load(GuestAddress{at},32)),static_cast<std::int32_t>(memory.load(GuestAddress{at+4},32))};};
        auto position=std::array<std::int32_t,2>{w.x,w.y};if(values[2]){position=pair(values[2]);if(w.style&0x40000000U){const auto origin=gui_client_origin(gui_,w.parent);const auto x=static_cast<std::int64_t>(position[0])-origin[0],y=static_cast<std::int64_t>(position[1])-origin[1];if(x<INT32_MIN || x>INT32_MAX || y<INT32_MIN || y>INT32_MAX)return done(87);position={static_cast<std::int32_t>(x),static_cast<std::int32_t>(y)};}}
        auto size=std::array<std::int32_t,2>{static_cast<std::int32_t>(w.width),static_cast<std::int32_t>(w.height)};if(values[3])size=pair(values[3]);
        if(size[0]<=0 || size[1]<=0 || size[0]>16384 || size[1]>16384 || static_cast<std::uint64_t>(size[0])*static_cast<unsigned>(size[1])>output_limit/4)return done(87);
        if((flags&8U) && (static_cast<unsigned>(size[0])!=w.width || static_cast<unsigned>(size[1])!=w.height))return done(87);
        if(values[1] && values[1]!=0x10000000U)return done(6);
        std::vector<std::uint32_t> pixels;std::shared_ptr<RasterSurface> surface;
        if(!values[4]){if(values[1] || values[3] || values[5] || values[7] || mode)return done(87);if(w.layered_pixels.empty())return done(87);}
        else {
            if(!mode)return done(87);const auto context=gui_.contexts.find(values[4]);if(context==gui_.contexts.end())return done(6);
            const auto& dc=context->second;const auto bitmap=gui_.bitmaps.find(dc.bitmap);if(bitmap==gui_.bitmaps.end())return done(50);
            if(gui_device_transform(dc)!=gui_identity)return done(50);
            const auto source=values[5]?pair(values[5]):std::array<std::int32_t,2>{};
            if(source[0]<0 || source[1]<0 || static_cast<std::uint64_t>(source[0])+static_cast<unsigned>(size[0])>bitmap->second.width || static_cast<std::uint64_t>(source[1])+static_cast<unsigned>(size[1])>bitmap->second.height)return done(87);
            unsigned constant=255;bool per_pixel=false;
            if(mode==2){if(!values[7])return done(87);const auto blend=static_cast<std::uint32_t>(memory.load(GuestAddress{values[7]},32));if((blend&0xffffU) || (blend>>24)>1)return done(87);constant=(blend>>16)&255;per_pixel=(blend>>24)==1;if(per_pixel && bitmap->second.bits_per_pixel!=32)return done(87);}
            try{
                import_dib_bitmap(memory,dc.bitmap);sync_raster_bitmap(memory,dc.bitmap);pixels.resize(static_cast<std::size_t>(size[0])*static_cast<unsigned>(size[1]));
                for(unsigned y=0;y<static_cast<unsigned>(size[1]);++y)for(unsigned x=0;x<static_cast<unsigned>(size[0]);++x){
                    const auto color=bitmap->second.pixels[(y+static_cast<unsigned>(source[1]))*bitmap->second.width+x+static_cast<unsigned>(source[0])];std::uint32_t output=color|0xff000000U;
                    if(mode==1 && (color&0xffffffU)==(values[6]&0xffffffU))output=0;
                    if(mode==2){output=0;for(unsigned channel=0;channel<3;++channel)output|=((((color>>(channel*8))&255)*constant+127)/255)<<(channel*8);output|=(((per_pixel?color>>24:255)*constant+127)/255)<<24;}
                    pixels[static_cast<std::size_t>(y)*static_cast<unsigned>(size[0])+x]=output;
                }
                if(native_gui_){surface=make_hardware_raster(static_cast<unsigned>(size[0]),static_cast<unsigned>(size[1]),{},native_gui_->graphics_view(arg(0)));surface->upload(pixels,true);surface->set_transparent(true);}
            }catch(const std::bad_alloc&){return done(8);}catch(const std::runtime_error&){return done(50);}
        }
        const bool moved=position[0]!=w.x || position[1]!=w.y,resized=static_cast<unsigned>(size[0])!=w.width || static_cast<unsigned>(size[1])!=w.height;
        w.x=position[0];w.y=position[1];w.width=static_cast<unsigned>(size[0]);w.height=static_cast<unsigned>(size[1]);
        if(values[4]){w.layered_pixels=std::move(pixels);w.drawing.clear();gui_.contexts.try_emplace(0x10000000U+arg(0));gui_.dc_objects.try_emplace(0x10000000U+arg(0),0xb011);if(surface){rasters_[0x10000000U+arg(0)]=surface;native_gui_->graphics_surface(arg(0),surface);}}
        present_gui(arg(0));if(surface)surface->flush(true);
        if(w.procedure && (moved || resized)){std::vector<GuestCallback> callbacks;if(moved)callbacks.push_back({w.procedure,{arg(0),3,0,gui_move_lparam(w)}});if(resized)callbacks.push_back({w.procedure,{arg(0),5,0,(w.width&65535U)|((w.height&65535U)<<16)}});start_callbacks(state,memory,std::move(callbacks),Continuation::Return,1);return {};}
        return done(0);
    }else if(symbol=="SetWindowRgn"){

        const auto window=gui_.windows.find(arg(0));const auto region=gui_.regions.find(arg(1));
        if(window==gui_.windows.end())fail(1400);else if(arg(1) && region==gui_.regions.end())fail(6);
        else {
            std::optional<GuiRegion> shape;if(arg(1))shape=region->second;
            if(native_gui_ && !native_gui_->set_region(arg(0),shape))fail(50);
            else {
                auto& w=window->second;w.window_region=std::move(shape);
                if(arg(1))gui_.regions.erase(region); // 成功後所有權移交系統，客體不再持有 HRGN。
                if(arg(2)){invalidate_window(arg(0));w.paint_frame=true;}present_gui(arg(0));result=1;
                if(w.procedure){
                    const unsigned unit=x86_?4U:8U;const std::uint8_t bits=x86_?32:64;const auto data=runtime_allocate(memory,{},x86_?28:40);const auto outer=gui_outer_size(w);
                    memory.store(GuestAddress{data},bits,arg(0));memory.store(GuestAddress{data+2*unit},32,static_cast<std::uint32_t>(w.x));memory.store(GuestAddress{data+2*unit+4},32,static_cast<std::uint32_t>(w.y));memory.store(GuestAddress{data+2*unit+8},32,outer[0]);memory.store(GuestAddress{data+2*unit+12},32,outer[1]);memory.store(GuestAddress{data+2*unit+16},32,0x37);
                    start_callbacks(state,memory,{{w.procedure,{arg(0),0x46,0,data}},{w.procedure,{arg(0),0x47,0,data}}},Continuation::Return,1);return std::nullopt;
                }
            }
        }
    } else if(symbol=="CreateRectRgn" || symbol=="CreateRectRgnIndirect" || symbol=="SetRectRgn" || symbol=="CombineRgn" || symbol=="OffsetRgn" || symbol=="GetRgnBox" || symbol=="PtInRegion" || symbol=="RectInRegion" || symbol=="EqualRgn" || symbol=="GetRegionData" || symbol=="GetWindowRgn" || symbol=="GetWindowRgnBox"){
        const auto read_rect=[&](std::uint64_t p){GuiRect r{};for(unsigned i=0;i<4;++i)r[i]=static_cast<std::int32_t>(memory.load(GuestAddress{p+i*4},32));return r;};
        const auto write_rect=[&](std::uint64_t p,const GuiRect& r){memory.check(GuestAddress{p},16,Access::Write);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{p+i*4},32,static_cast<std::uint32_t>(r[i]));};
        try{
            const auto found=gui_.regions.find(arg(0));
            if(symbol=="CreateRectRgn" || symbol=="CreateRectRgnIndirect" || symbol=="SetRectRgn"){
                const bool set=symbol=="SetRectRgn";GuiRect r{};
                if(symbol=="CreateRectRgnIndirect")r=read_rect(arg(0));else for(unsigned i=0;i<4;++i)r[i]=static_cast<std::int32_t>(arg(i+(set?1:0)));
                if(r[0]>r[2])std::swap(r[0],r[2]);if(r[1]>r[3])std::swap(r[1],r[3]);
                if(set && found==gui_.regions.end())fail(6);
                else if(!gui_region_coordinates({r}))fail(87);
                else {GuiRegion region;if(!gui_rect_empty(r))region.push_back(r);if(set){found->second=std::move(region);result=1;}else if(gui_.regions.size()>=65536)fail(8);else {result=gui_.next_object++;gui_.regions.emplace(result,std::move(region));}}
            }else if(symbol=="GetWindowRgn" || symbol=="GetWindowRgnBox"){
                if(!gui_.windows.contains(arg(0)))fail(1400);
                else if(symbol=="GetWindowRgn" && !gui_.regions.contains(arg(1)))fail(6);
                else if(const auto& shape=gui_.windows.at(arg(0)).window_region;shape){
                    if(symbol=="GetWindowRgn")gui_.regions.at(arg(1))=*shape;else write_rect(arg(1),gui_region_bounds(*shape));result=gui_region_kind(*shape);
                }else {last_error_=0;result=0;} // 未指定自訂外形時 Windows 回 ERROR。
            }else if(found==gui_.regions.end())fail(6);
            else if(symbol=="CombineRgn"){
                const auto mode=static_cast<unsigned>(arg(3));const auto a=gui_.regions.find(arg(1)),b=gui_.regions.find(arg(2));
                if(mode<1 || mode>5)fail(87);else if(a==gui_.regions.end() || (mode!=5 && b==gui_.regions.end()))fail(6);
                else {const auto combined=gui_region_combine(a->second,mode==5?GuiRegion{}:b->second,mode);found->second=combined;result=gui_region_kind(combined);}
            }else if(symbol=="OffsetRgn"){
                auto region=found->second;bool valid=true;
                for(auto& r:region)for(unsigned i=0;i<4;++i){const auto value=static_cast<std::int64_t>(r[i])+static_cast<std::int32_t>(arg(1+i%2));if(value<-(1<<26) || value>=(1<<26))valid=false;else r[i]=static_cast<std::int32_t>(value);}
                if(!valid)fail(87);else {found->second=std::move(region);result=gui_region_kind(found->second);}
            }else if(symbol=="GetRgnBox"){write_rect(arg(1),gui_region_bounds(found->second));result=gui_region_kind(found->second);}
            else if(symbol=="EqualRgn"){const auto other=gui_.regions.find(arg(1));if(other==gui_.regions.end())fail(6);else result=found->second==other->second;}
            else if(symbol=="PtInRegion"){
                const auto x=static_cast<std::int32_t>(arg(1)),y=static_cast<std::int32_t>(arg(2));result=std::any_of(found->second.begin(),found->second.end(),[&](const auto& r){return x>=r[0] && x<r[2] && y>=r[1] && y<r[3];});
            }else if(symbol=="RectInRegion"){
                const auto rect=read_rect(arg(1));result=!gui_rect_empty(rect) && std::any_of(found->second.begin(),found->second.end(),[&](const auto& r){return rect[0]<r[2] && rect[2]>r[0] && rect[1]<r[3] && rect[3]>r[1];});
            }else {
                const auto& region=found->second;const auto bytes=32+region.size()*16;
                if(!arg(2))result=bytes;else if(arg(1)<bytes)fail(122);
                else {memory.check(GuestAddress{arg(2)},bytes,Access::Write);memory.store(GuestAddress{arg(2)},32,32);memory.store(GuestAddress{arg(2)+4},32,1);memory.store(GuestAddress{arg(2)+8},32,region.size());memory.store(GuestAddress{arg(2)+12},32,region.size()*16);write_rect(arg(2)+16,gui_region_bounds(region));for(std::size_t i=0;i<region.size();++i)write_rect(arg(2)+32+i*16,region[i]);result=bytes;}
            }
        }catch(const std::bad_alloc&){fail(8);}catch(const std::length_error&){fail(8);}
    } else if(symbol=="RedrawWindow" || symbol=="ValidateRect"){
        const auto flags=symbol=="ValidateRect"?0x48U:static_cast<std::uint32_t>(arg(3));
        const bool desktop=!arg(0) || arg(0)==gui_.desktop;
        if(flags&~0xfffU)fail(87);
        else if(!desktop && !gui_.windows.contains(arg(0)))fail(1400);
        else if(symbol=="RedrawWindow" && arg(2))fail(50); // HRGN 後端尚未提供。
        else {
            std::optional<std::array<std::int32_t,4>> rect;
            if(arg(1)){std::array<std::int32_t,4> r{};for(unsigned i=0;i<4;++i)r[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+i*4},32));rect=r;}
            std::vector<std::uint64_t> windows;
            if(desktop){for(const auto& [h,w]:gui_.windows)if(!w.parent)windows.push_back(h);}else windows.push_back(arg(0));
            for(std::size_t i=0;i<windows.size();++i){const auto& parent=gui_.windows.at(windows[i]);if(!(flags&0x40) && ((flags&0x80) || !(parent.style&0x02000000U)))for(const auto& [h,w]:gui_.windows)if(w.parent==windows[i] && std::find(windows.begin(),windows.end(),h)==windows.end())windows.push_back(h);}
            bool supported=true,empty=false;
            if(rect){
                empty=(*rect)[0]>=(*rect)[2] || (*rect)[1]>=(*rect)[3];
                // 現有 GUI 更新狀態以整個 client area 為單位；不把部分 Validate 假裝成全區成功。
                if(!empty && (flags&9U)){
                    if(desktop)supported=false;
                    else {const auto& w=gui_.windows.at(arg(0));
                        if((*rect)[2]<=0 || (*rect)[3]<=0 || (*rect)[0]>=static_cast<std::int64_t>(w.width) || (*rect)[1]>=static_cast<std::int64_t>(w.height))empty=true;
                        else supported=(*rect)[0]<=0 && (*rect)[1]<=0 && (*rect)[2]>=static_cast<std::int64_t>(w.width) && (*rect)[3]>=static_cast<std::int64_t>(w.height);
                    }
                }
            }
            if((flags&0x300) && std::any_of(windows.begin(),windows.end(),[&](auto h){return gui_.windows.at(h).thread!=threads_.current;}))supported=false;
            if(!supported)fail(50);
            else {
                std::vector<GuestCallback> callbacks;
                for(const auto h:windows){
                    auto& w=gui_.windows.at(h);const auto proc=w.procedure;
                    if(!empty && (flags&1)){invalidate_window(h);if(flags&4)w.paint_erase=true;if(flags&0x400)w.paint_frame=true;}
                    if(flags&0x20)w.paint_erase=false;if(flags&0x800)w.paint_frame=false;
                    if(flags&2)w.internal_paint=true;if(flags&0x10)w.internal_paint=false;
                    if(!empty && (flags&8)){w.invalid=false;w.paint_erase=false;std::erase_if(gui_.queue,[&](const auto& msg){return msg.window==h && msg.message==15 && !w.internal_paint;});}
                    if(flags&0x10)std::erase_if(gui_.queue,[&](const auto& msg){return msg.window==h && msg.message==15 && !w.invalid;});
                    if(w.internal_paint && w.visible && std::none_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& msg){return msg.window==h && msg.message==15;})){GuiMessage msg{h,0,0,15};msg.queue_priority=2;enqueue_gui_message(std::move(msg));}
                    if((flags&0x300) && proc && w.invalid){
                        if(w.paint_frame)callbacks.push_back({proc,{h,0x85,1,0}});
                        if(w.paint_erase)callbacks.push_back({proc,{h,0x14,0x10000000+h,0}});
                        w.paint_frame=false;w.paint_erase=false;
                    }
                    if((flags&0x100) && proc && (w.invalid || w.internal_paint)){
                        w.internal_paint=false;std::erase_if(gui_.queue,[&](const auto& msg){return msg.window==h && msg.message==15;});callbacks.push_back({proc,{h,15,0,0}});
                    }
                }
                if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,1);return std::nullopt;}
                result=1;
            }
        }
    } else if(symbol=="SetLayeredWindowAttributes" || symbol=="GetLayeredWindowAttributes") {
        const auto found=gui_.windows.find(arg(0));
        if(found==gui_.windows.end())fail(1400);
        else if(!(found->second.ex_style&0x80000U))fail(87);
        else if(symbol=="SetLayeredWindowAttributes") {
            const auto flags=static_cast<std::uint32_t>(arg(3));const auto alpha=static_cast<std::uint8_t>(arg(2));
            if(!flags || (flags&~3U))fail(87);
            else if(flags&1U)fail(50); // 色鍵需要逐像素合成，不能用 constant alpha 假裝成功。
            else if(found->second.combo_owner || (native_gui_ && !native_gui_->set_opacity(arg(0),alpha)))fail(50);
            else {auto& window=found->second;window.layered_initialized=true;window.layered_color=static_cast<std::uint32_t>(arg(1));window.layered_alpha=alpha;result=1;}
        }else if(!found->second.layered_initialized)fail(87);
        else {
            // 三個輸出皆可為 NULL；先檢查全部非 NULL buffer，避免錯誤造成部分寫入。
            for(unsigned i=1;i<4;++i)if(arg(i))memory.check(GuestAddress{arg(i)},i==2?1:4,Access::Write);
            if(arg(1))memory.store(GuestAddress{arg(1)},32,found->second.layered_color);
            if(arg(2))memory.store(GuestAddress{arg(2)},8,found->second.layered_alpha);
            if(arg(3))memory.store(GuestAddress{arg(3)},32,2);
            result=1;
        }
    } else if(symbol=="GetWindow" || symbol=="GetTopWindow") {
        const auto hwnd=arg(0);const auto command=symbol=="GetTopWindow"?5U:static_cast<std::uint32_t>(arg(1));
        const bool desktop=hwnd==gui_.desktop || (!hwnd && symbol=="GetTopWindow");
        const auto found=gui_.windows.find(hwnd);
        if(!desktop && found==gui_.windows.end())fail(1400);
        else if(command>6)fail(87);
        else if(command==4)result=desktop?0:found->second.owner;
        else if(command==6) {
            result=hwnd;
            for(const auto h:gui_.z_order){const auto w=gui_.windows.find(h);if(w!=gui_.windows.end() && w->second.owner==hwnd && w->second.enabled && (w->second.style&0x80000000U)){result=h;break;}}
        } else {
            const auto parent=command==5?(desktop?0:hwnd):(desktop?0:found->second.parent);
            std::uint64_t previous{};bool seen=false;
            for(const auto h:gui_.z_order){const auto w=gui_.windows.find(h);if(w==gui_.windows.end() || w->second.parent!=parent)continue;
                if(command==0 || command==5){result=h;break;}
                if(command==1)result=h;
                else if(command==2 && seen){result=h;break;}
                else if(command==3 && h==hwnd){result=previous;break;}
                if(h==hwnd)seen=true;previous=h;
            }
        }
    } else if(symbol=="AdjustWindowRect" || symbol=="AdjustWindowRectEx" || symbol=="AdjustWindowRectExForDpi") {
        if(!arg(0) || (symbol=="AdjustWindowRectExForDpi" && !static_cast<std::uint32_t>(arg(4))))fail(87);
        else {
            const auto inset=gui_frame_insets(static_cast<std::uint32_t>(arg(1)),symbol!="AdjustWindowRect"?static_cast<std::uint32_t>(arg(3)):0,arg(2)!=0,false);
            const auto scaled=symbol=="AdjustWindowRectExForDpi"?gui_frame_insets_for_dpi(static_cast<std::uint32_t>(arg(1)),static_cast<std::uint32_t>(arg(3)),arg(2)!=0,static_cast<std::uint32_t>(arg(4))):inset;
            const std::array<std::int64_t,4> delta{-static_cast<std::int64_t>(scaled.left),-static_cast<std::int64_t>(scaled.top),scaled.right,scaled.bottom};
            std::array<std::uint32_t,4> rect{};bool valid=true;
            for(unsigned i=0;i<4;++i){const auto value=static_cast<std::int64_t>(static_cast<std::int32_t>(memory.load(GuestAddress{arg(0)+i*4},32)))+delta[i];valid&=value>=INT32_MIN && value<=INT32_MAX;rect[i]=static_cast<std::uint32_t>(value);}
            if(!valid)fail(534);else {memory.check(GuestAddress{arg(0)},16,Access::Write);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(0)+i*4},32,rect[i]);result=1;}
        }
    } else if(symbol=="CopyRect" || symbol=="SetRect" || symbol=="SetRectEmpty") {
        if(!arg(0) || (symbol=="CopyRect" && !arg(1)))fail(87);
        else {
            // 先讀入暫存，允許來源與目的矩形重疊；RECT 一律為四個 LONG。
            std::array<std::uint8_t,16> rect{};
            if(symbol=="CopyRect")memory.read(GuestAddress{arg(1)},rect);
            else if(symbol=="SetRect")for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)rect[i*4+j]=static_cast<std::uint8_t>(arg(i+1)>>(j*8));
            memory.write(GuestAddress{arg(0)},rect);result=1;
        }
    } else if(symbol=="EqualRect" || symbol=="IsRectEmpty" || symbol=="PtInRect" || symbol=="InflateRect" || symbol=="IntersectRect" || symbol=="UnionRect" || symbol=="SubtractRect") {
        using Rect=std::array<std::int32_t,4>;
        const auto read=[&](std::uint64_t p){Rect r{};for(unsigned i=0;i<4;++i)r[i]=static_cast<std::int32_t>(memory.load(GuestAddress{p+i*4},32));return r;};
        const auto empty=[](const Rect& r){return r[0]>=r[2] || r[1]>=r[3];};
        const auto write=[&](const Rect& r){memory.check(GuestAddress{arg(0)},16,Access::Write);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(0)+i*4},32,static_cast<std::uint32_t>(r[i]));};
        if(!arg(0))result=symbol=="IsRectEmpty";
        else if(symbol=="EqualRect")result=arg(1) && read(arg(0))==read(arg(1));
        else if(symbol=="IsRectEmpty")result=empty(read(arg(0)));
        else if(symbol=="PtInRect"){
            const auto r=read(arg(0));const auto x=static_cast<std::int32_t>(arg(1)),y=static_cast<std::int32_t>(arg(1)>>32);
            result=x>=r[0] && x<r[2] && y>=r[1] && y<r[3];
        }else if(symbol=="InflateRect"){
            auto r=read(arg(0));for(unsigned i=0;i<4;++i){const auto value=static_cast<std::uint32_t>(r[i]),delta=static_cast<std::uint32_t>(arg(1+i%2));r[i]=static_cast<std::int32_t>(i<2?value-delta:value+delta);}write(r);result=1;
        }else if(arg(1) && arg(2)){
            // 先讀取全部來源，允許原地運算；SubtractRect 回傳差集的外接矩形。
            const auto a=read(arg(1)),b=read(arg(2));Rect out{};
            Rect intersection{std::max(a[0],b[0]),std::max(a[1],b[1]),std::min(a[2],b[2]),std::min(a[3],b[3])};
            if(symbol=="IntersectRect"){if(!empty(intersection))out=intersection;}
            else if(symbol=="UnionRect"){
                if(empty(a)){if(!empty(b))out=b;}else if(empty(b))out=a;
                else out={std::min(a[0],b[0]),std::min(a[1],b[1]),std::max(a[2],b[2]),std::max(a[3],b[3])};
            }else if(!empty(a)){
                out=a;
                if(!empty(intersection)){
                    if(intersection==a)out={};
                    else if(intersection[0]==a[0] && intersection[2]==a[2]){
                        if(intersection[1]==a[1])out[1]=intersection[3];else if(intersection[3]==a[3])out[3]=intersection[1];
                    }else if(intersection[1]==a[1] && intersection[3]==a[3]){
                        if(intersection[0]==a[0])out[0]=intersection[2];else if(intersection[2]==a[2])out[2]=intersection[0];
                    }
                }
            }
            write(out);result=!empty(out);
        }

    } else if(symbol.starts_with("UnregisterClass")) {
        return invoke_gui_support(state,memory,service);
    } else {
        const auto found=gui_.windows.find(arg(0));
        if(found==gui_.windows.end())fail(1400);
        else {
            auto& window=found->second;
            if(symbol=="GetUpdateRect") {
                result=window.invalid?1:0;
                if(arg(1)) {
                    memory.write(GuestAddress{arg(1)},std::array<std::uint8_t,16>{});
                    if(window.invalid){memory.store(GuestAddress{arg(1)+8},32,window.width);memory.store(GuestAddress{arg(1)+12},32,window.height);}
                }
                if(result && arg(2) && window.procedure) {
                    // 擦除是客體 WNDPROC 的同步 callback；不能用成功回傳代替。
                    start_callbacks(state,memory,{{window.procedure,{arg(0),0x14,UINT64_C(0x10000000)+arg(0),0}}},Continuation::Return,1);
                    return std::nullopt;
                }
            } else if(symbol.starts_with("GetWindowLong") || symbol.starts_with("SetWindowLong")) {
                const auto index=static_cast<std::int32_t>(arg(1));
                const bool set=symbol.starts_with("Set"),wide=symbol.ends_with("W");
                if((x86_ && index==-4) || index==-16 || index==-20)return invoke_gui(state,memory,{"user32.dll",set?(wide?"SetWindowLongPtrW":"SetWindowLongPtrA"):(wide?"GetWindowLongPtrW":"GetWindowLongPtrA"),set?3U:2U});
                const auto value=set?static_cast<std::uint32_t>(arg(2)):0;
                if(x86_ && window.dialog && index==4) {
                    auto& procedure=index==4?window.dialog_proc:window.procedure;result=procedure;
                    if(set){if(value && !handles(GuestAddress{value}))memory.check(GuestAddress{value},1,Access::Execute);procedure=value;}
                } else if(x86_ && index==-6){result=window.instance;if(set)window.instance=value;}
                else if(index>=0 && static_cast<std::uint64_t>(index)+4<=window.extra.size()) {
                    for(unsigned i=0;i<4;++i)result|=static_cast<std::uint64_t>(window.extra[static_cast<std::size_t>(index)+i])<<(i*8);
                    if(set)for(unsigned i=0;i<4;++i)window.extra[static_cast<std::size_t>(index)+i]=static_cast<std::uint8_t>(value>>(i*8));
                } else if(index==-12) {result=window.id;if(set)window.id=value;}
                else if(index==-21) {result=window.user_data;if(set)window.user_data=value;}
                // x64 中的指標或 handle 必須透過 LongPtr API，避免截斷寫入。
                else fail(1413);
                result=static_cast<std::uint32_t>(result);
            } else throw HostCallFault("尚未實作此圖形相容服務");
        }
    }
    arg.return_to(state,result);return std::nullopt;
}
}
