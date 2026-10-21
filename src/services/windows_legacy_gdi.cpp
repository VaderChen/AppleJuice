#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_palette.hpp"
#include "x64arm/windows_region.hpp"
#include "x64arm/windows_transform.hpp"
#include "x64arm/gpu_memory.hpp"
#include "x64arm/resource_usage.hpp"
#include "x64arm/windows_opengl.hpp"
#include "windows_display_identity.hpp"
#include "x64arm/unicode.hpp"
#include <cstdio>
#include <algorithm>
#include <bit>
#include <map>

namespace x64arm {
std::span<const WindowsServiceDescriptor> legacy_gdi_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","GetTextCharsetInfo",3},{"gdi32.dll","GetTextCharset",1},
        {"gdi32.dll","FillRgn",3},{"gdi32.dll","PaintRgn",2},{"gdi32.dll","FrameRgn",5},{"gdi32.dll","InvertRgn",2},
        {"gdi32.dll","SetPixelV",4},{"gdi32.dll","ExtFloodFill",5},{"gdi32.dll","FloodFill",4},
        {"gdi32.dll","GetBoundsRect",3},{"gdi32.dll","SetBoundsRect",3},{"gdi32.dll","GetDCOrgEx",2},
        {"gdi32.dll","CreateDIBitmap",6},
        {"gdi32.dll","SetMetaFileBitsEx",2},{"gdi32.dll","GetMetaFileBitsEx",3},{"gdi32.dll","CopyMetaFileA",2},{"gdi32.dll","CopyMetaFileW",2},
        {"gdi32.dll","GetMetaFileA",1},{"gdi32.dll","GetMetaFileW",1},{"gdi32.dll","DeleteMetaFile",1},
        {"gdi32.dll","D3DKMTOpenAdapterFromHdc",1},{"gdi32.dll","D3DKMTOpenAdapterFromGdiDisplayName",1},{"gdi32.dll","D3DKMTOpenAdapterFromDeviceName",1},
        {"gdi32.dll","D3DKMTOpenAdapterFromLuid",1},{"gdi32.dll","D3DKMTEnumAdapters2",1},{"gdi32.dll","D3DKMTCloseAdapter",1},
        {"gdi32.dll","D3DKMTQueryAdapterInfo",1},{"gdi32.dll","D3DKMTQueryStatistics",1}
    };return table;
}
namespace {
// FONTSIGNATURE 的 code page 位元（fsCsb[0]）與代表性 Unicode 子範圍（fsUsb）；依 Windows charset 對照表。
struct CharsetSignature {std::uint8_t charset;std::int8_t code_page_bit;std::array<std::int8_t,4> unicode_bits;};
constexpr CharsetSignature charset_signatures[]{
    {0,0,{0,1,-1,-1}},{238,1,{0,2,-1,-1}},{204,2,{0,9,-1,-1}},{161,3,{0,7,-1,-1}},{162,4,{0,2,-1,-1}},{177,5,{0,11,-1,-1}},
    {178,6,{0,13,-1,-1}},{186,7,{0,2,-1,-1}},{163,8,{0,2,3,-1}},{222,16,{0,24,-1,-1}},{128,17,{48,49,50,59}},{134,18,{48,59,-1,-1}},
    {129,19,{48,56,28,59}},{136,20,{48,59,-1,-1}},{130,21,{48,56,28,-1}},{2,31,{-1,-1,-1,-1}}};
bool valid_windows_metafile(std::span<const std::uint8_t> bytes){
    if(bytes.size()<18)return false;const auto word=[&](std::size_t at){return static_cast<std::uint32_t>(bytes[at])|static_cast<std::uint32_t>(bytes[at+1])<<8;};
    const auto words=word(6)|word(8)<<16;
    return (word(0)==1 || word(0)==2) && word(2)==9 && (word(4)==0x100 || word(4)==0x300) && words>=9 && std::uint64_t{words}*2<=bytes.size();
}
std::size_t windows_metafile_size(std::span<const std::uint8_t> bytes){return 2*(static_cast<std::size_t>(bytes[6])|static_cast<std::size_t>(bytes[7])<<8|static_cast<std::size_t>(bytes[8])<<16|static_cast<std::size_t>(bytes[9])<<24);}
constexpr std::size_t metafile_limit=64U*1024U*1024U;
}
std::uint64_t WindowsServices::nested_service(GuestState& state,GuestMemory& memory,std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> arguments){
    // 經正式服務入口呼叫，固定 kernel32 表與延伸 descriptor 表共用相同分派、ABI 與追蹤。
    static const auto index=[]{std::map<std::pair<std::string_view,std::string_view>,const WindowsServiceDescriptor*> out;for(const auto& d:runtime_service_descriptors())out.try_emplace({d.family,d.symbol},&d);return out;}();
    if(const auto found=index.find({family,symbol});found!=index.end()){
        const auto& descriptor=*found->second;
        // narrow 參數只截取低 32 位、caller cleanup 只影響返回後的堆疊；兩者不改變此框架的參數位置。
        if(descriptor.arguments!=arguments.size() || descriptor.wide_arguments || descriptor.aggregate_arguments)throw HostCallFault("巢狀服務參數形式不符");
    }
    const auto target=resolve_service(family,symbol,false);if(!target.value)throw HostCallFault("巢狀服務尚未登錄");
    // 返回位址指向 KUSER_SHARED_DATA 內不可執行的保留字，服務若改走客體回呼即可辨識。
    constexpr std::uint64_t sentinel=0x7ffe0ff0;const unsigned unit=x86_?4U:8U;
    const auto frame=allocate_heap(memory,static_cast<std::uint64_t>(unit)*(std::max<std::size_t>(arguments.size(),4)+2)+32);if(!frame)throw std::bad_alloc();
    struct Release {WindowsServices& self;GuestMemory& memory;const GuestState& state;std::uint64_t pointer;~Release(){(void)self.free_heap(memory,pointer,state);}} release{*this,memory,state,frame};
    // x64 入口與真實 CALL 相同：返回位址所在的 RSP 為 16n+8。
    const auto stack=x86_?frame:((frame+15)&~std::uint64_t{15})+8;
    auto local=state;local.rip=target;local.reg(Gpr::Rsp)=stack;memory.store(GuestAddress{stack},static_cast<std::uint8_t>(unit*8),sentinel);
    constexpr std::array registers{Gpr::Rcx,Gpr::Rdx,Gpr::R8,Gpr::R9};std::size_t i=0;
    for(const auto value:arguments){
        if(x86_)memory.store(GuestAddress{stack+4+i*4},32,value&UINT32_MAX);
        else if(i<4)local.reg(registers[i])=value;
        else memory.store(GuestAddress{stack+40+(i-4)*8},64,value);
        ++i;
    }
    if(invoke(local,memory) || local.rip.value!=sentinel)throw HostCallFault("巢狀服務需要客體回呼，不能在宿主端完成");
    return x86_?local.reg(Gpr::Rax)&UINT32_MAX:local.reg(Gpr::Rax);
}
void WindowsServices::accumulate_dc_bounds(GuiDc& dc,const GuiDraw& draw){
    if(!dc.bounds_enabled)return;
    std::int64_t left=std::min(draw.x,draw.right),top=std::min(draw.y,draw.bottom),right=std::max(draw.x,draw.right),bottom=std::max(draw.y,draw.bottom);
    if(draw.kind==GuiDraw::Kind::Text){
        std::int64_t width=0;for(const auto advance:draw.advances)width+=advance;
        if(draw.advances.empty())width=static_cast<std::int64_t>(draw.text.size())*(draw.cell_width?draw.cell_width:std::max(1,std::abs(draw.font.height)/2));
        left=std::min<std::int64_t>(draw.x,draw.x+width);right=std::max<std::int64_t>(draw.x,draw.x+width);top=draw.y;bottom=static_cast<std::int64_t>(draw.y)+std::max(1,std::abs(draw.font.height));
    }else if(draw.kind==GuiDraw::Kind::Line){++right;++bottom;}
    if(draw.kind!=GuiDraw::Kind::Fill && draw.kind!=GuiDraw::Kind::Text && draw.pen_width>1){const auto half=draw.pen_width/2;left-=half;top-=half;right+=half;bottom+=half;}
    const auto clamp=[](std::int64_t v){return static_cast<std::int32_t>(std::clamp<std::int64_t>(v,-(1<<26),(1<<26)-1));};
    GuiRect rect{clamp(left),clamp(top),clamp(right),clamp(bottom)};if(!draw.device_coordinates)rect=gui_transform_bounds(draw.transform,rect);
    if(gui_rect_empty(rect))return;
    if(gui_rect_empty(dc.bounds))dc.bounds=rect;
    else dc.bounds={std::min(dc.bounds[0],rect[0]),std::min(dc.bounds[1],rect[1]),std::max(dc.bounds[2],rect[2]),std::max(dc.bounds[3],rect[3])};
}
std::optional<std::uint32_t> WindowsServices::invoke_legacy_gdi(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(name.starts_with("GetTextCharset")?1U:0U);};
    const auto integer=[&](unsigned i){return static_cast<std::int32_t>(arg(i));};
    const auto known_dc=[&](std::uint64_t h){return h && (gui_.contexts.contains(h) || gui_.dc_objects.contains(h) || (h>=0x10000000 && gui_.windows.contains(h-0x10000000)));};
    const auto window=arg(0)>=0x10000000?gui_.windows.find(arg(0)-0x10000000):gui_.windows.end();
    std::vector<std::uint64_t> scratch;
    struct Release {WindowsServices& self;GuestMemory& memory;const GuestState& state;std::vector<std::uint64_t>& pointers;~Release(){for(const auto p:pointers)(void)self.free_heap(memory,p,state);}} release{*this,memory,state,scratch};
    const auto temporary=[&](std::size_t size){const auto p=allocate_heap(memory,std::max<std::size_t>(size,8));if(!p)throw std::bad_alloc();scratch.push_back(p);return p;};
    const auto invalid_handle=x86_?std::uint64_t{UINT32_MAX}:UINT64_MAX;
    try {
        if(name=="GetTextCharsetInfo" || name=="GetTextCharset"){
            if(!known_dc(arg(0)))return fail(6);
            auto charset=gui_font(gui_,gui_.contexts[arg(0)].font).charset;if(charset==1)charset=0; // 已實現字型不會保留 DEFAULT_CHARSET。
            if(name=="GetTextCharsetInfo" && arg(1)){
                memory.check(GuestAddress{arg(1)},24,Access::Write);std::array<std::uint32_t,6> signature{};
                const auto entry=std::find_if(std::begin(charset_signatures),std::end(charset_signatures),[&](const auto& e){return e.charset==charset;});
                if(entry!=std::end(charset_signatures)){
                    if(entry->code_page_bit>=0)signature[4]|=1U<<entry->code_page_bit;
                    for(const auto bit:entry->unicode_bits)if(bit>=0)signature[static_cast<unsigned>(bit)/32]|=1U<<(bit%32);
                }
                for(unsigned i=0;i<6;++i)memory.store(GuestAddress{arg(1)+i*4},32,signature[i]);
            }
            return finish(charset);
        }
        if(name=="FillRgn" || name=="PaintRgn" || name=="InvertRgn" || name=="FrameRgn"){
            if(!known_dc(arg(0)))return fail(6);const auto found=gui_.regions.find(arg(1));if(found==gui_.regions.end())return fail(6);
            auto region=found->second;
            if(name=="FrameRgn"){
                // 與 Windows 相同：外框是原區域扣除四向平移後仍完整覆蓋的內部。
                const std::int64_t w=std::abs(static_cast<std::int64_t>(integer(3))),h=std::abs(static_cast<std::int64_t>(integer(4)));
                if(w>=(1<<26) || h>=(1<<26))return fail(87);
                auto inner=region;
                for(const auto& [dx,dy]:std::array<std::pair<std::int64_t,std::int64_t>,4>{{{-w,0},{w,0},{0,-h},{0,h}}}){
                    auto moved=region;for(auto& r:moved){r[0]=static_cast<std::int32_t>(r[0]+dx);r[2]=static_cast<std::int32_t>(r[2]+dx);r[1]=static_cast<std::int32_t>(r[1]+dy);r[3]=static_cast<std::int32_t>(r[3]+dy);}
                    inner=gui_region_combine(inner,moved,1);
                }
                region=gui_region_combine(region,inner,4);
            }
            const auto brush=name=="PaintRgn"?gui_.contexts[arg(0)].brush:arg(2);const auto buffer=temporary(16);
            for(const auto& r:region){
                for(unsigned i=0;i<4;++i)memory.store(GuestAddress{buffer+i*4},32,static_cast<std::uint32_t>(r[i]));
                const auto ok=name=="InvertRgn"?nested_service(state,memory,"user32.dll","InvertRect",{arg(0),buffer}):nested_service(state,memory,"user32.dll","FillRect",{arg(0),buffer,brush});
                if(!ok)return finish(0);
            }
            return finish(1);
        }
        if(name=="SetPixelV")return finish(static_cast<std::uint32_t>(nested_service(state,memory,"gdi32.dll","SetPixel",{arg(0),arg(1),arg(2),arg(3)}))!=UINT32_MAX?1:0);
        if(name=="ExtFloodFill" || name=="FloodFill"){
            const auto type=name=="FloodFill"?0U:static_cast<std::uint32_t>(arg(4));if(type>1)return fail(87);
            if(!known_dc(arg(0)))return fail(6);auto& dc=gui_.contexts[arg(0)];
            const auto surface=raster_surface(memory,arg(0));if(!surface)return fail(50);
            const auto start=gui_integer_point(gui_device_transform(dc),integer(1),integer(2));if(!start)return fail(87);
            const auto width=static_cast<std::int64_t>(surface->width()),height=static_cast<std::int64_t>(surface->height());const auto sx=(*start)[0],sy=(*start)[1];
            const auto visible=[&](std::int64_t x,std::int64_t y){
                if(x<0 || y<0 || x>=width || y>=height)return false;
                if(dc.clipped && (x<dc.clip[0] || y<dc.clip[1] || x>=dc.clip[2] || y>=dc.clip[3]))return false;
                return std::none_of(dc.excluded.begin(),dc.excluded.end(),[&](const auto& r){return x>=r[0] && y>=r[1] && x<r[2] && y<r[3];});
            };
            if(!visible(sx,sy))return finish(0);
            const auto snapshot=surface->snapshot();if(snapshot.width!=width || snapshot.height!=height)return fail(50);
            const auto target=gui_resolve_color(gui_,dc,static_cast<std::uint32_t>(arg(3)))&0xffffffU;
            std::vector<std::uint8_t> marked(static_cast<std::size_t>(width*height));
            const auto open=[&](std::int64_t x,std::int64_t y){if(!visible(x,y) || marked[static_cast<std::size_t>(y*width+x)])return false;const auto c=snapshot.pixels[static_cast<std::size_t>(y*width+x)]&0xffffffU;return type?c==target:c!=target;};
            if(!open(sx,sy))return finish(0);
            // 掃描線填色：只讀一次表面快照，填色範圍以水平帶交給共用光柵器繪製。
            std::vector<std::pair<std::int64_t,std::int64_t>> pending{{sx,sy}};std::vector<GuiRect> spans;
            while(!pending.empty()){
                const auto [x,y]=pending.back();pending.pop_back();if(!open(x,y))continue;
                auto left=x,right=x;while(open(left-1,y))--left;while(open(right+1,y))++right;
                for(auto i=left;i<=right;++i)marked[static_cast<std::size_t>(y*width+i)]=1;
                if(spans.size()>=1U<<22)throw std::length_error("填色範圍過大");spans.push_back({static_cast<std::int32_t>(left),static_cast<std::int32_t>(y),static_cast<std::int32_t>(right+1),static_cast<std::int32_t>(y+1)});
                for(const auto row:{y-1,y+1}){bool run=false;for(auto i=left;i<=right;++i){const bool next=open(i,row);if(next && !run)pending.push_back({i,row});run=next;}}
            }
            if(dc.brush==0xb005)return finish(1);
            GuiDraw style;style.kind=GuiDraw::Kind::Fill;style.device_coordinates=true;style.clipped=dc.clipped;style.clip=dc.clip;style.excluded=dc.excluded;
            style.color=dc.brush==0xb012?dc.dc_brush_color:gui_.brushes.contains(dc.brush)?gui_.brushes.at(dc.brush):dc.brush==0xb000?0xffffff:dc.brush==0xb001?0xc0c0c0:dc.brush==0xb002?0x808080:dc.brush==0xb003?0x404040:0;
            style.hatch=gui_.brush_hatches.contains(dc.brush)?gui_.brush_hatches.at(dc.brush):-1;style.pattern_opaque=dc.mode==2;style.pattern_background=dc.background;style.pattern_color=dc.color;style.brush_origin=dc.brush_origin;
            if(gui_.brush_patterns.contains(dc.brush)){style.pattern_brush=dc.brush;style.pattern_monochrome=gui_.brush_patterns.at(dc.brush).bits_per_pixel==1;}
            for(const auto& span:spans){auto draw=style;draw.x=span[0];draw.y=span[1];draw.right=span[2];draw.bottom=span[3];if(!raster_draw(memory,arg(0),draw))return fail(50);}
            if(window!=gui_.windows.end())present_gui(window->first);
            return finish(1);
        }
        if(name=="GetBoundsRect" || name=="SetBoundsRect"){
            if(!known_dc(arg(0)))return fail(6);auto& dc=gui_.contexts[arg(0)];const auto flags=static_cast<std::uint32_t>(arg(2));
            const auto transform=gui_device_transform(dc);const bool empty=gui_rect_empty(dc.bounds);
            // DCB_RESET=1、DCB_ACCUMULATE=2、DCB_SET=3、DCB_ENABLE=4、DCB_DISABLE=8。
            const auto status=(dc.bounds_enabled?4U:8U)|(empty?1U:3U);
            if(name=="GetBoundsRect"){
                if(flags&~1U)return fail(87);
                if(arg(1)){
                    memory.check(GuestAddress{arg(1)},16,Access::Write);GuiRect rect{};
                    if(!empty){const auto inverse=gui_inverse(transform);if(!inverse)return fail(87);rect=gui_transform_bounds(*inverse,dc.bounds);}
                    for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(1)+i*4},32,static_cast<std::uint32_t>(rect[i]));
                }
                if(flags&1U)dc.bounds={};
                return finish(status);
            }
            if((flags&12U)==12U || (flags&~15U))return fail(87);
            if(flags&1U)dc.bounds={};
            if((flags&2U) && arg(1)){
                GuiRect rect{};for(unsigned i=0;i<4;++i)rect[i]=static_cast<std::int32_t>(memory.load(GuestAddress{arg(1)+i*4},32));
                rect={std::min(rect[0],rect[2]),std::min(rect[1],rect[3]),std::max(rect[0],rect[2]),std::max(rect[1],rect[3])};rect=gui_transform_bounds(transform,rect);
                if(!gui_rect_empty(rect))dc.bounds=gui_rect_empty(dc.bounds)?rect:GuiRect{std::min(dc.bounds[0],rect[0]),std::min(dc.bounds[1],rect[1]),std::max(dc.bounds[2],rect[2]),std::max(dc.bounds[3],rect[3])};
            }
            if(flags&4U)dc.bounds_enabled=true;if(flags&8U)dc.bounds_enabled=false;
            return finish(status);
        }
        if(name=="GetDCOrgEx"){
            if(!known_dc(arg(0)))return fail(6);if(!arg(1))return fail(87);
            std::int64_t x=0,y=0;
            if(window!=gui_.windows.end()){
                const auto origin=gui_client_origin(gui_,window->first);x=origin[0];y=origin[1];
                if(const auto context=gui_.contexts.find(arg(0));context!=gui_.contexts.end() && context->second.window_frame){const auto f=gui_frame_insets(window->second);x-=f.left;y-=f.top;}
            }
            memory.check(GuestAddress{arg(1)},8,Access::Write);memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(x));memory.store(GuestAddress{arg(1)+4},32,static_cast<std::uint32_t>(y));
            return finish(1);
        }
        if(name=="CreateDIBitmap"){
            const auto header=arg(1);const auto init=static_cast<std::uint32_t>(arg(2));
            if(!header || (init&~6U))return fail(87);if(arg(0) && !known_dc(arg(0)))return fail(6);
            const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{header},32));std::int64_t width=0,height=0;
            if(size==12){width=static_cast<std::int64_t>(memory.load(GuestAddress{header+4},16));height=static_cast<std::int64_t>(memory.load(GuestAddress{header+6},16));}
            else if(size>=40){width=static_cast<std::int32_t>(memory.load(GuestAddress{header+4},32));height=static_cast<std::int32_t>(memory.load(GuestAddress{header+8},32));}
            else return fail(87);
            if(width<0)return fail(87);height=std::abs(height);
            std::uint64_t bitmap=0;
            if(init&2U){
                // CBM_CREATEDIB：以 BITMAPINFO 的格式建立 DIB section，與 CreateDIBSection 共用配置。
                if(!arg(4))return fail(87);const auto bits=temporary(8);
                bitmap=nested_service(state,memory,"gdi32.dll","CreateDIBSection",{arg(0),arg(4),arg(5),bits,0,0});
            }else if(!width || !height)bitmap=nested_service(state,memory,"gdi32.dll","CreateBitmap",{1,1,1,1,0});
            else bitmap=nested_service(state,memory,"gdi32.dll","CreateCompatibleBitmap",{arg(0),static_cast<std::uint64_t>(width),static_cast<std::uint64_t>(height)});
            if(!bitmap)return finish(0);
            if((init&4U) && arg(3) && width && height){
                const auto error=[&](std::uint32_t code){(void)nested_service(state,memory,"gdi32.dll","DeleteObject",{bitmap});if(code)last_error_=code;return finish(0);};
                if(!arg(4))return error(87);
                if(!nested_service(state,memory,"gdi32.dll","SetDIBits",{arg(0),bitmap,0,static_cast<std::uint64_t>(height),arg(3),arg(4),arg(5)}))return error(0);
            }
            return finish(bitmap);
        }
        if(name.starts_with("D3DKMT")){
            // 顯示核心介面：與 DXGI／DXCore 共用同一個虛擬 UMA 配接器（LUID、vendor 0x106B、共享記憶體額度）。
            // 未提供的查詢類型回 STATUS_NOT_SUPPORTED 並記錄一次診斷。版面依 d3dkmthk.h，指標欄位隨客體位元寬度調整。
            constexpr std::uint32_t ok=0,invalid=0xc000000dU,unsupported=0xc00000bbU,not_found=0xc0000225U;
            (void)unsupported;const auto p=arg(0);if(!p)return finish(invalid);const unsigned unit=x86_?4U:8U;const auto pointer_bits=static_cast<std::uint8_t>(unit*8);
            const auto luid=display_identity::adapter_luid;const bool available=metal_opengl_identity().has_value();
            const auto open=[&](std::uint64_t handle_at,std::uint64_t luid_at)->std::uint64_t{
                if(!available)return not_found;if(gui_.kmt_adapters.size()>=4096)return 0xc000009aU;const auto handle=gui_.next_kmt_adapter++;gui_.kmt_adapters.insert(handle);
                memory.store(GuestAddress{p+handle_at},32,handle);memory.store(GuestAddress{p+luid_at},32,static_cast<std::uint32_t>(luid));memory.store(GuestAddress{p+luid_at+4},32,static_cast<std::uint32_t>(luid>>32));return ok;
            };
            const auto diagnose=[&](const char* api,std::uint32_t type){static std::set<std::pair<std::string,std::uint32_t>> seen;if(seen.emplace(api,type).second)std::fprintf(stderr,"[d3dkmt] %s type=%u 未支援\n",api,type);};
            if(name=="D3DKMTOpenAdapterFromHdc"){
                const auto dc=memory.load(GuestAddress{p},pointer_bits);const bool display=dc>=0x10000000 || (gui_.contexts.contains(dc) && gui_.contexts.at(dc).display);
                if(!display)return finish(invalid);const auto status=open(unit,unit+4);if(!status)memory.store(GuestAddress{p+unit+12},32,0);return finish(status);
            }
            if(name=="D3DKMTOpenAdapterFromGdiDisplayName"){
                std::u16string device;for(unsigned i=0;i<32;++i){const auto c=static_cast<char16_t>(memory.load(GuestAddress{p+i*2},16));if(!c)break;device.push_back(c>=u'a' && c<=u'z'?static_cast<char16_t>(c-32):c);}
                if(device!=u"\\\\.\\DISPLAY1")return finish(invalid);const auto status=open(64,68);if(!status)memory.store(GuestAddress{p+76},32,0);return finish(status);
            }
            if(name=="D3DKMTOpenAdapterFromDeviceName"){
                const auto at=memory.load(GuestAddress{p},pointer_bits);if(!at)return finish(invalid);auto path=read_utf16(memory,GuestAddress{at});for(auto& c:path)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
                // 介面路徑：GUID_DISPLAY_DEVICE_ARRIVAL 或 GUID_DEVINTERFACE_DISPLAY_ADAPTER。
                if(path!=u"\\\\?\\root#x64arm_display#0000#{1ca05180-a699-450a-9a0c-de4fbe3ddd89}" && path!=u"\\\\?\\root#x64arm_display#0000#{5b45201d-f2f2-4f3b-85bb-30ff1f953599}")return finish(invalid);
                return finish(open(unit,unit+4));
            }
            if(name=="D3DKMTOpenAdapterFromLuid"){if((static_cast<std::uint64_t>(memory.load(GuestAddress{p+4},32))<<32|memory.load(GuestAddress{p},32))!=luid)return finish(invalid);return finish(open(8,0));}
            if(name=="D3DKMTEnumAdapters2"){
                const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));const auto adapters=memory.load(GuestAddress{p+unit},pointer_bits);const std::uint32_t count=available?1U:0U;
                if(!adapters){memory.store(GuestAddress{p},32,count);return finish(ok);}if(capacity<count){memory.store(GuestAddress{p},32,count);return finish(0xc0000023U);}
                memory.store(GuestAddress{p},32,count);if(count){memory.fill(GuestAddress{adapters},0,20);const auto handle_store=adapters;(void)handle_store;
                    if(gui_.kmt_adapters.size()>=4096)return finish(0xc000009aU);const auto handle=gui_.next_kmt_adapter++;gui_.kmt_adapters.insert(handle);
                    memory.store(GuestAddress{adapters},32,handle);memory.store(GuestAddress{adapters+4},32,static_cast<std::uint32_t>(luid));memory.store(GuestAddress{adapters+8},32,static_cast<std::uint32_t>(luid>>32));memory.store(GuestAddress{adapters+12},32,1);}
                return finish(ok);
            }
            if(name=="D3DKMTCloseAdapter")return finish(gui_.kmt_adapters.erase(static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)))?ok:invalid);
            const auto memory_limit=gpu_memory_stats();
            if(name=="D3DKMTQueryAdapterInfo"){
                const auto handle=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)),type=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
                const auto data=memory.load(GuestAddress{p+8},pointer_bits);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{p+8+unit},32));
                if(!gui_.kmt_adapters.contains(handle))return finish(invalid);if(!data)return finish(invalid);
                const auto need=[&](std::uint32_t bytes){if(size<bytes){static std::set<std::uint32_t> reported;if(reported.insert(type).second)std::fprintf(stderr,"[d3dkmt] QueryAdapterInfo type=%u size=%u 小於 %u\n",type,size,bytes);}return size>=bytes;};
                const auto text=[&](std::uint64_t at,std::u16string_view value,std::size_t limit){for(std::size_t i=0;i<limit;++i)memory.store(GuestAddress{at+i*2},16,i<value.size()?value[i]:0);};
                const auto identity=metal_opengl_identity();const auto renderer=identity?utf8_to_utf16(identity->first):std::u16string(display_identity::adapter_name);
                switch(type){
                case 3: // KMTQAITYPE_GETSEGMENTSIZE：UMA 沒有獨立顯存，共享系統記憶體＝GPU 記憶體額度。
                    if(!need(24))return finish(invalid);memory.store(GuestAddress{data},64,0);memory.store(GuestAddress{data+8},64,0);memory.store(GuestAddress{data+16},64,memory_limit.limit);return finish(ok);
                case 8: // KMTQAITYPE_ADAPTERREGISTRYINFO：AdapterString／BiosString／DacType／ChipType 各 WCHAR[260]。
                    if(!need(2080))return finish(invalid);text(data,display_identity::adapter_name,260);text(data+520,u"",260);text(data+1040,u"Integrated RAMDAC",260);text(data+1560,renderer,260);return finish(ok);
                case 13: // KMTQAITYPE_DRIVERVERSION：與 D3D12 功能一致的 WDDM 2.0。
                    if(!need(4))return finish(invalid);memory.store(GuestAddress{data},32,2000);return finish(ok);
                case 15: // KMTQAITYPE_ADAPTERTYPE：RenderSupported｜DisplaySupported。
                    if(!need(4))return finish(invalid);memory.store(GuestAddress{data},32,3);return finish(ok);
                case 18:case 49: // UMD／KMD 驅動版本：與 SetupAPI 的 DriverVersion 相同。
                    if(!need(8))return finish(invalid);memory.store(GuestAddress{data},64,display_identity::version_number);return finish(ok);
                case 25:{ // KMTQAITYPE_NODEMETADATA：單一 3D 引擎節點；UMA 經 IOMMU 存取系統記憶體。
                    // D3DKMT_NODEMETADATA：索引(4)＋EngineType(4)＋FriendlyName[32]＋Flags(4)＋GpuMmu／IoMmu 兩個 BOOLEAN，SDK 大小 78。
                    if(!need(78))return finish(invalid);if((memory.load(GuestAddress{data},32)&0xffffU)!=0)return finish(invalid);
                    memory.fill(GuestAddress{data+4},0,74);memory.store(GuestAddress{data+4},32,1);text(data+8,u"3D",32);memory.store(GuestAddress{data+77},8,1);return finish(ok);}
                case 30: // KMTQAITYPE_PHYSICALADAPTERCOUNT
                    if(!need(4))return finish(invalid);memory.store(GuestAddress{data},32,1);return finish(ok);
                case 31: // KMTQAITYPE_PHYSICALADAPTERDEVICEIDS：VendorID 0x106B，非 PCI 匯流排的 root 裝置沒有 DeviceID／子系統。
                    if(!need(28))return finish(invalid);if(memory.load(GuestAddress{data},32))return finish(invalid);memory.fill(GuestAddress{data+4},0,24);memory.store(GuestAddress{data+4},32,display_identity::vendor_id);return finish(ok);
                default:diagnose("QueryAdapterInfo",type);return finish(unsupported);
                }
            }
            // D3DKMTQueryStatistics：1 個系統記憶體 aperture 段、1 個 3D 節點、1 個 VidPn 來源。
            const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));
            if((static_cast<std::uint64_t>(memory.load(GuestAddress{p+8},32))<<32|memory.load(GuestAddress{p+4},32))!=luid || !available)return finish(invalid);
            // QueryResult 含 ULONGLONG，x86 在 hProcess 後對齊到 16；x64 為 24。
            const auto result=p+(x86_?16U:24U);memory.fill(GuestAddress{result},0,776);const auto index=static_cast<std::uint32_t>(memory.load(GuestAddress{result+776},32));
            switch(type){
            case 0:case 2: // ADAPTER／PROCESS_ADAPTER：NbSegments、NodeCount、VidPnSourceCount。
                memory.store(GuestAddress{result},32,1);memory.store(GuestAddress{result+4},32,1);memory.store(GuestAddress{result+8},32,1);return finish(ok);
            case 1: // PROCESS：NodeCount、VidPnSourceCount。
                memory.store(GuestAddress{result},32,1);memory.store(GuestAddress{result+4},32,1);return finish(ok);
            case 3: // SEGMENT：CommitLimit、BytesCommitted、BytesResident、Aperture。
                if(index)return finish(invalid);memory.store(GuestAddress{result},64,memory_limit.limit);memory.store(GuestAddress{result+8},64,memory_limit.used);memory.store(GuestAddress{result+16},64,memory_limit.used);memory.store(GuestAddress{result+40},32,1);return finish(ok);
            case 4: // PROCESS_SEGMENT：BytesCommitted。
                if(index)return finish(invalid);memory.store(GuestAddress{result},64,memory_limit.used);return finish(ok);
            case 5:case 6:{ // NODE／PROCESS_NODE：RunningTime（100 ns）。全域與本程序相同，因為 GPU 只服務此程序。
                if(index)return finish(invalid);enable_gpu_statistics();const auto running=gpu_running_time();memory.store(GuestAddress{result},64,running);return finish(ok);}
            case 7:case 8: // VIDPNSOURCE：沒有 flip 統計時全部為 0。
                if(index)return finish(invalid);return finish(ok);
            default:diagnose("QueryStatistics",type);return finish(unsupported);
            }
        }
        if(name=="SetMetaFileBitsEx"){
            const auto size=static_cast<std::uint32_t>(arg(0));if(!arg(1) || size<18 || size>metafile_limit)return fail(87);
            std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{arg(1)},bytes);
            if(!valid_windows_metafile(bytes))return fail(13);bytes.resize(windows_metafile_size(bytes));
            const auto handle=gui_.next_object++;gui_.windows_metafiles.emplace(handle,std::move(bytes));return finish(handle);
        }
        if(name=="GetMetaFileBitsEx"){
            const auto found=gui_.windows_metafiles.find(arg(0));if(found==gui_.windows_metafiles.end())return fail(6);
            if(!arg(2))return finish(found->second.size());
            const auto count=std::min<std::size_t>(static_cast<std::uint32_t>(arg(1)),found->second.size());
            memory.check(GuestAddress{arg(2)},count,Access::Write);memory.write(GuestAddress{arg(2)},std::span(found->second).first(count));return finish(count);
        }
        if(name=="DeleteMetaFile"){if(!gui_.windows_metafiles.erase(arg(0)))return fail(6);return finish(1);}
        if(name.starts_with("CopyMetaFile") || name.starts_with("GetMetaFile")){
            const bool wide=name.ends_with('W'),copy=name.starts_with("CopyMetaFile");const auto path=arg(copy?1:0);
            std::vector<std::uint8_t> bytes;
            if(copy){const auto found=gui_.windows_metafiles.find(arg(0));if(found==gui_.windows_metafiles.end())return fail(6);bytes=found->second;}
            else if(!path)return fail(87);
            if(path){
                // 磁碟 metafile 經一般檔案服務存取，沿用映射、權限與共用模式。
                const auto file=nested_service(state,memory,"kernel32.dll",wide?"CreateFileW":"CreateFileA",{path,copy?0x40000000U:0x80000000U,copy?0U:1U,0,copy?2U:3U,0x80,0});
                if(file==invalid_handle || !file)return finish(0);
                struct Close {WindowsServices& self;GuestState& state;GuestMemory& memory;std::uint64_t handle;~Close(){try{(void)self.nested_service(state,memory,"kernel32.dll","CloseHandle",{handle});}catch(...){}}} close{*this,state,memory,file};
                const auto count=temporary(8);
                if(copy){
                    const auto buffer=temporary(bytes.size());memory.write(GuestAddress{buffer},bytes);
                    if(!nested_service(state,memory,"kernel32.dll","WriteFile",{file,buffer,bytes.size(),count,0}) || memory.load(GuestAddress{count},32)!=bytes.size())return finish(0);
                }else {
                    if(!nested_service(state,memory,"kernel32.dll","GetFileSizeEx",{file,count}))return finish(0);
                    const auto size=memory.load(GuestAddress{count},64);if(size<18 || size>metafile_limit)return fail(13);
                    const auto buffer=temporary(static_cast<std::size_t>(size));
                    if(!nested_service(state,memory,"kernel32.dll","ReadFile",{file,buffer,size,count,0}) || memory.load(GuestAddress{count},32)!=size)return finish(0);
                    bytes.resize(static_cast<std::size_t>(size));memory.read(GuestAddress{buffer},bytes);
                    if(!valid_windows_metafile(bytes))return fail(13);bytes.resize(windows_metafile_size(bytes));
                }
            }
            const auto handle=gui_.next_object++;gui_.windows_metafiles.emplace(handle,std::move(bytes));return finish(handle);
        }
    }catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(8);}catch(const std::invalid_argument&){return fail(87);}
    throw HostCallFault("尚未實作舊式 GDI 服務");
}
}
