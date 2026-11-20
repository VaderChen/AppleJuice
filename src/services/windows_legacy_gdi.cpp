#include "adapter_multisampling.hpp"
#include "d3d9_format_contract.h"
#include "gpu_budget_request.hpp"
#include "vidpn_ownership.hpp"
#include "monotonic_clock.hpp"
#include "windows_wmf.hpp"
#include "service_callback_task.hpp"
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
        {"gdi32.dll","D3DKMTGetDisplayModeList",1},{"gdi32.dll","D3DKMTGetMultisampleMethodList",1},
        {"gdi32.dll","D3DKMTQueryAdapterInfo",1},{"gdi32.dll","D3DKMTQueryStatistics",1},
        {"gdi32.dll","D3DKMTEnumAdapters",1},{"gdi32.dll","D3DKMTQueryVideoMemoryInfo",1},{"gdi32.dll","D3DKMTCheckOcclusion",1},
        {"d3d8thk.dll","OsThunkDdCreateDirectDrawObject",1},{"d3d8thk.dll","OsThunkDdDeleteDirectDrawObject",1},
        {"d3d8thk.dll","OsThunkDdWaitForVerticalBlank",2},{"gdi32.dll","D3DKMTWaitForVerticalBlankEvent",1},
        {"gdi32.dll","D3DKMTWaitForVerticalBlankEvent2",1},{"gdi32.dll","D3DKMTGetDWMVerticalBlankEvent",1},
        {"d3d8thk.dll","OsThunkDdGetScanLine",2},{"gdi32.dll","D3DKMTGetScanLine",1},
        {"gdi32.dll","D3DKMTCreateDevice",1},{"gdi32.dll","D3DKMTSetQueuedLimit",1},
        {"win32u.dll","NtGdiDdDDIChangeVideoMemoryReservation",1},
        {"win32u.dll","NtGdiDdDDICheckExclusiveOwnership",0},{"win32u.dll","NtGdiDdDDIQueryVidPnExclusiveOwnership",1},
        {"win32u.dll","NtGdiDdDDIReleaseProcessVidPnSourceOwners",1},
        {"gdi32.dll","D3DKMTCheckVidPnExclusiveOwnership",1},{"gdi32.dll","D3DKMTSetVidPnSourceOwner",1},
        {"gdi32.dll","D3DKMTCreateDCFromMemory",1},{"gdi32.dll","D3DKMTDestroyDCFromMemory",1}
    };return table;
}
namespace {
// FONTSIGNATURE 的 code page 位元（fsCsb[0]）與代表性 Unicode 子範圍（fsUsb）；依 Windows charset 對照表。
struct CharsetSignature {std::uint8_t charset;std::int8_t code_page_bit;std::array<std::int8_t,4> unicode_bits;};
constexpr CharsetSignature charset_signatures[]{
    {0,0,{0,1,-1,-1}},{238,1,{0,2,-1,-1}},{204,2,{0,9,-1,-1}},{161,3,{0,7,-1,-1}},{162,4,{0,2,-1,-1}},{177,5,{0,11,-1,-1}},
    {178,6,{0,13,-1,-1}},{186,7,{0,2,-1,-1}},{163,8,{0,2,3,-1}},{222,16,{0,24,-1,-1}},{128,17,{48,49,50,59}},{134,18,{48,59,-1,-1}},
    {129,19,{48,56,28,59}},{136,20,{48,59,-1,-1}},{130,21,{48,56,28,-1}},{2,31,{-1,-1,-1,-1}}};
// WMF 標頭驗證與大小改用 windows_wmf.hpp 的共用實作。
constexpr std::size_t metafile_limit=windows_metafile_limit;
}
std::optional<std::uint32_t> WindowsServices::nested_continuation(GuestState& state,GuestMemory& memory,std::string_view kind,
        std::function<service_callback::Task<std::optional<std::uint32_t>>(GuestState&,GuestMemory&)> factory){
    return service_callback::invoke<std::optional<std::uint32_t>>(this,threads_.current,state,memory,kind,
        [this](GuestState& s,GuestMemory& m,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            GuestCallback call{target,std::move(args)};
            if(auto* context=service_callback::active;context && context->native_pending){call=std::move(context->native);context->native_pending=false;}
            start_callbacks(s,m,{std::move(call)},Continuation::NestedResult,id);},std::move(factory));
}
std::uint64_t WindowsServices::nested_service(GuestState& state,GuestMemory& memory,std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> arguments){
    return nested_service(state,memory,family,symbol,std::span<const std::uint64_t>(arguments.begin(),arguments.size()));
}
// 引數數目在執行期才決定的轉接（例如 ntdll 的 Rtl→Win32 表格轉接）共用同一個宿主端框架。
std::uint64_t WindowsServices::nested_service(GuestState& state,GuestMemory& memory,std::string_view family,std::string_view symbol,std::span<const std::uint64_t> arguments){
    // 經正式服務入口呼叫，固定 kernel32 表與延伸 descriptor 表共用相同分派、ABI 與追蹤。
    static const auto index=[]{std::map<std::pair<std::string_view,std::string_view>,const WindowsServiceDescriptor*> out;for(const auto& d:runtime_service_descriptors())out.try_emplace({d.family,d.symbol},&d);return out;}();
    // GdiFlush 的公開入口解析為客體批次常式（X64ARM_GUEST_GDI_BATCH），不是宿主服務入口，不能在此框架執行；
    // 巢狀呼叫者（緩衝繪製取位元、GDI+ 圖形結束）需要的是宿主端完成呼叫執行緒的批次表面，直接沿用同一實作。
    if(family=="gdi32.dll" && symbol=="GdiFlush"){
        if(arguments.size())throw HostCallFault("巢狀服務參數形式不符");
        return flush_gdi_batch(memory)?1U:0U;
    }
    if(const auto found=index.find({family,symbol});found!=index.end()){
        const auto& descriptor=*found->second;
        // narrow 參數只截取低 32 位、caller cleanup 只影響返回後的堆疊；兩者不改變此框架的參數位置。
        if(descriptor.arguments!=arguments.size() || descriptor.wide_arguments || descriptor.aggregate_arguments)throw HostCallFault("巢狀服務參數形式不符");
    }
    // 優先使用描述元本身的宿主入口：匯出可能改導向客體快速常式（臨界區段、SRW、Interlocked 等），宿主端巢狀呼叫要直接到原服務。
    auto target=host_service_stub(family,symbol);if(!target.value)target=resolve_service(family,symbol,false);
    if(!target.value)throw HostCallFault("巢狀服務尚未登錄");
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
    const auto display_dc=[&](std::uint64_t dc){
        return dc==0x10000000 || (dc>0x10000000 && gui_.windows.contains(dc-0x10000000)) ||
               (gui_.contexts.contains(dc) && gui_.contexts.at(dc).display);
    };
    const auto window=name!="NtGdiDdDDICheckExclusiveOwnership" && arg(0)>=0x10000000?gui_.windows.find(arg(0)-0x10000000):gui_.windows.end();
    std::vector<std::uint64_t> scratch;
    struct Release {WindowsServices& self;GuestMemory& memory;const GuestState& state;std::vector<std::uint64_t>& pointers;~Release(){for(const auto p:pointers)(void)self.free_heap(memory,p,state);}} release{*this,memory,state,scratch};
    const auto temporary=[&](std::size_t size){const auto p=allocate_heap(memory,std::max<std::size_t>(size,8));if(!p)throw std::bad_alloc();scratch.push_back(p);return p;};
    try {
        if(name=="D3DKMTGetDisplayModeList"){
            const auto p=arg(0);
            if(!p || !gui_.kmt_adapters.contains(static_cast<unsigned>(memory.load(GuestAddress{p},32))) ||
               memory.load(GuestAddress{p+4},32))return finish(0xc000000dU);
            const auto output=memory.load(GuestAddress{p+8},x86_?32:64);
            const GuestAddress count{p+(x86_?12U:16U)};
            const auto capacity=memory.load(count,32);
            if(!output && capacity)return finish(0xc000000dU);

            const auto& monitors=gui_monitors();
            const auto primary=std::find_if(monitors.begin(),monitors.end(),[](const auto& m){return m.primary;});
            if(primary==monitors.end())return finish(0xc01e0333U);
            auto modes=native_gui_?native_gui_->display_modes():std::vector<GuiDisplayMode>{};
            if(modes.empty()){
                const GuiDisplayMode fallback{static_cast<unsigned>(primary->rect[2]-primary->rect[0]),
                    static_cast<unsigned>(primary->rect[3]-primary->rect[1]),primary->refresh,1};
                const auto current=native_gui_?native_gui_->current_display_mode():std::optional<GuiDisplayMode>{};
                modes.push_back(current.value_or(fallback));
            }
            // D3DKMT_DISPLAYMODE 的旗標佔兩個 DWORD，兩種客體的每筆步距均為 44。
            constexpr unsigned stride=44;
            memory.check(count,4,Access::Write);
            if(output && capacity>=modes.size())memory.check(GuestAddress{output},modes.size()*stride,Access::Write);
            memory.store(count,32,modes.size());
            if(!output)return finish(0);
            if(capacity<modes.size())return finish(0xc0000023U);
            auto dest=output;
            for(const auto& mode:modes){
                const auto numerator=mode.refresh_numerator && mode.refresh_denominator?mode.refresh_numerator:primary->refresh;
                const auto denominator=mode.refresh_numerator && mode.refresh_denominator?mode.refresh_denominator:1U;
                const auto hz=static_cast<unsigned>((std::uint64_t{numerator}+denominator/2)/denominator);
                // 與 D3D9 的掃描輸出一致，使用 X8R8G8B8、原向與驅動建議模式。
                const std::array<std::uint32_t,11> entry{mode.width,mode.height,22,hz,numerator,denominator,
                    mode.scanline,1,0,0,7};
                memory.write(GuestAddress{dest},std::span(reinterpret_cast<const std::uint8_t*>(entry.data()),stride));
                dest+=stride;
            }
            return finish(0);
        }
        if(name=="D3DKMTGetMultisampleMethodList"){
            const auto p=arg(0);
            if(!p)return finish(0xc000000dU);
            const auto field=[&](unsigned offset){return static_cast<unsigned>(memory.load(GuestAddress{p+offset},32));};
            const auto width=field(8),height=field(12),format=field(16);
            if(!gui_.kmt_adapters.contains(field(0)) || field(4) || !width || !height ||
               width>16384 || height>16384 || !format)return finish(0xc000000dU);
            const auto methods=memory.load(GuestAddress{p+(x86_?20U:24U)},x86_?32:64);
            const auto count_offset=x86_?24U:32U;
            const auto capacity=field(count_offset);
            const auto native=d9_render_color(format)?d9_native_format(format):d9_depth_format(format);
            const auto mask=adapter_multisample_mask(native);
            if(!mask)return finish(0xc00002b6U);
            const auto required=static_cast<unsigned>(std::popcount(*mask));
            memory.check(GuestAddress{p+count_offset},4,Access::Write);
            if(methods && capacity>=required && required)
                memory.check(GuestAddress{methods},required*12,Access::Write);
            memory.store(GuestAddress{p+count_offset},32,required);
            if(!methods)return finish(0);
            if(capacity<required)return finish(0xc0000023U);
            unsigned offset=0;
            for(unsigned samples=2;samples<=16;++samples){
                if(!(*mask & (1U<<samples)))continue;
                memory.store(GuestAddress{methods+offset},32,samples);
                memory.store(GuestAddress{methods+offset+4},32,1);
                memory.store(GuestAddress{methods+offset+8},32,0);
                offset+=12;
            }
            return finish(0);
        }
        if(name=="OsThunkDdCreateDirectDrawObject"){
            if((arg(0) && !display_dc(arg(0))) || gui_.directdraw_objects.size()>=4096)return finish(0);
            const auto handle=gui_.next_kmt_sync+=4;
            gui_.directdraw_objects.insert(handle);return finish(handle);
        }
        if(name=="OsThunkDdDeleteDirectDrawObject")return finish(gui_.directdraw_objects.erase(arg(0)));
        struct Raster {std::uint32_t line;bool blank;std::uint64_t begin,end,period;};
        const auto raster=[this]()->std::optional<Raster>{
            const auto& monitors=gui_monitors();
            const auto primary=std::find_if(monitors.begin(),monitors.end(),[](const auto& m){return m.primary;});
            if(primary==monitors.end())return {};
            const auto mode=native_gui_ && !gui_.display_mode?native_gui_->current_display_mode():std::optional<GuiDisplayMode>{};
            const auto height=mode && mode->height?mode->height:static_cast<std::uint32_t>(primary->rect[3]-primary->rect[1]);
            const bool rational=mode && mode->refresh_numerator && mode->refresh_denominator;
            const auto num=rational?mode->refresh_numerator:(primary->refresh?primary->refresh:60U);
            const auto den=rational?mode->refresh_denominator:1U;
            const auto rate=static_cast<std::uint32_t>((std::uint64_t{num}+den/2)/den);
            const auto lines=std::uint64_t{height}+display_identity::vertical_blank_lines(height,rate);
            const auto ticks=runtime_.host_clock?detail::monotonic_ticks():runtime_.clock_ticks;
            const auto period=UINT64_C(10000000)*den;
            const auto phase=(static_cast<unsigned __int128>(ticks)*num)%period;
            const auto line=static_cast<std::uint32_t>(phase*lines/period);
            const bool blank=line>=height;
            const auto edge=(static_cast<unsigned __int128>(period)*height+lines-1)/lines;
            const auto until_begin=(phase<edge?edge-phase:period+edge-phase);
            return Raster{line,blank,ticks+static_cast<std::uint64_t>((until_begin+num-1)/num),
                          ticks+static_cast<std::uint64_t>((period-phase+num-1)/num),(period+num/2)/num};
        };
        if(name=="OsThunkDdWaitForVerticalBlank" || name=="D3DKMTGetDWMVerticalBlankEvent" || name.starts_with("D3DKMTWaitForVerticalBlankEvent"))
            return nested_continuation(state,memory,"垂直遮沒等待",[this,service,raster](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                const auto finish=[&](std::uint32_t status){call.return_to(s,status);return std::optional<std::uint32_t>{};};
                const auto name=service.symbol;
                const bool legacy=name=="OsThunkDdWaitForVerticalBlank",get=name=="D3DKMTGetDWMVerticalBlankEvent";
                const auto p=call(legacy?1:0);
                const unsigned unit=x86_?4U:8U;
                const auto bits=static_cast<std::uint8_t>(unit*8);
                const auto timing=raster();
                if(!timing)co_return finish(legacy?0U:0xc00002b6U);
                bool end=false;
                std::uint64_t output=0;
                std::uint32_t count=0;
                std::array<std::uint64_t,9> handles{};
                if(legacy){
                    if(!p || !gui_.directdraw_objects.contains(call(0)))co_return finish(0);
                    const auto flag=m.load(GuestAddress{p+unit},32);
                    output=p+2*unit+8;
                    m.check(GuestAddress{output},4,Access::Write);
                    if(flag==0x80000006U){
                        m.store(GuestAddress{p+unit+4},32,timing->blank);m.store(GuestAddress{output},32,0);co_return finish(1);
                    }
                    if(flag!=1 && flag!=4){
                        m.store(GuestAddress{output},32,flag==2?0x80004001U:0x80070057U);co_return finish(1);
                    }
                    end=flag==4;
                }else{
                    if(!p)co_return finish(0xc000000dU);
                    const auto adapter=static_cast<std::uint32_t>(m.load(GuestAddress{p},32));
                    const auto device=static_cast<std::uint32_t>(m.load(GuestAddress{p+4},32));
                    const auto it=gui_.kmt_devices.find(device);
                    if(!gui_.kmt_adapters.contains(adapter) || (device && (it==gui_.kmt_devices.end() || it->second.adapter!=adapter)) ||
                       m.load(GuestAddress{p+8},32))co_return finish(0xc000000dU);
                    if(get){
                        output=m.load(GuestAddress{p+(x86_?12U:16U)},bits);
                        if(!output)co_return finish(0xc000000dU);
                        m.check(GuestAddress{output},unit,Access::Write);
                    }else if(name.ends_with('2')){
                        count=static_cast<std::uint32_t>(m.load(GuestAddress{p+12},32));
                        if(count>8)co_return finish(0xc000000dU);
                        for(unsigned i=0;i<count;++i)handles[1+i]=m.load(GuestAddress{p+16+i*unit},bits);
                    }
                }
                if(runtime_.wait_objects.size()>=process_handle_limit)co_return finish(legacy?0U:0xc0000017U);
                const auto handle=runtime_.allocate_wait_handle();
                WaitObject event;event.kind=WaitObject::Kind::Event;event.display_event=true;event.maximum=1;
                event.due=event.timer_nominal_due=end?timing->end:timing->begin;
                event.period=get?timing->period:0;event.active=true;
                runtime_.wait_states.emplace(handle,event);
                event.identity=handle;event.access=0x100000;
                try{runtime_.wait_objects.emplace(handle,event);}catch(...){runtime_.wait_states.erase(handle);throw;}
                if(get){m.store(GuestAddress{output},bits,handle);co_return finish(0);}
                handles[0]=handle;
                const auto array=allocate_heap(m,(count+1)*unit);
                if(!array){(void)nested_service(s,m,"kernel32.dll","CloseHandle",{handle});co_return finish(legacy?0U:0xc0000017U);}
                for(unsigned i=0;i<=count;++i)m.store(GuestAddress{array+i*unit},bits,handles[i]);
                const auto status=co_await service_callback::call(resolve("ntdll.dll","NtWaitForMultipleObjects").value,{count+1,array,1,0,0});
                (void)free_heap(m,array,s);(void)nested_service(s,m,"kernel32.dll","CloseHandle",{handle});
                if(legacy){m.store(GuestAddress{output},32,status?0x80004005U:0);co_return finish(1);}
                co_return finish(status<=count?static_cast<std::uint32_t>(status):0xc000000dU);
            });
        if(name=="OsThunkDdGetScanLine" || name=="D3DKMTGetScanLine"){
            const bool legacy=name=="OsThunkDdGetScanLine";
            const auto p=arg(legacy?1:0);
            if(legacy){
                if(!p || !gui_.directdraw_objects.contains(arg(0)))return finish(0);
            }else if(!p || !gui_.kmt_adapters.contains(static_cast<std::uint32_t>(memory.load(GuestAddress{p},32))) ||
                     memory.load(GuestAddress{p+4},32))return finish(0xc000000dU);
            const auto timing=raster();
            if(!timing)return finish(legacy?0U:0xc00002b6U);
            const auto line=timing->line;
            const bool blank=timing->blank;
            if(legacy){
                const auto out=p+(x86_?4U:8U);
                memory.check(GuestAddress{out},8,Access::Write);
                if(!blank)memory.store(GuestAddress{out},32,line);
                memory.store(GuestAddress{out+4},32,blank?0x88760219U:0);
                return finish(1);
            }
            memory.check(GuestAddress{p+8},8,Access::Write);
            memory.store(GuestAddress{p+8},8,blank);memory.store(GuestAddress{p+12},32,line);
            return finish(0);
        }
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
        if(name=="D3DKMTSetVidPnSourceOwner" || name=="D3DKMTCheckVidPnExclusiveOwnership" ||
           name=="NtGdiDdDDICheckExclusiveOwnership" || name=="NtGdiDdDDIQueryVidPnExclusiveOwnership" ||
           name=="NtGdiDdDDIReleaseProcessVidPnSourceOwners"){
            constexpr auto invalid=0xc000000dU;
            const auto dir=processes_ && runtime_.network.external_io?mapping_session():std::string{};
            std::uint32_t current{};
            const auto owner=[&](std::uint32_t pid,std::uint32_t device,std::optional<std::uint32_t> type){
                return vidpn_ownership(gui_,dir,process_id(),pid,device,type,current);
            };
            if(name=="NtGdiDdDDICheckExclusiveOwnership"){owner(0,0,{});return finish(current==2);}
            const auto p=arg(0);if(!p)return finish(invalid);
            if(name=="NtGdiDdDDIReleaseProcessVidPnSourceOwners"){
                const auto pid=process_handle_pid(p,0x200);return finish(pid?owner(pid,0,0):invalid);
            }
            const unsigned unit=x86_?4U:8U;
            const auto bits=static_cast<std::uint8_t>(unit*8);
            if(name=="NtGdiDdDDIQueryVidPnExclusiveOwnership"){
                const auto pid=process_handle_pid(memory.load(GuestAddress{p},bits),0x1000);
                if(!pid)return finish(invalid);
                const auto hwnd=memory.load(GuestAddress{p+unit},bits);
                if(hwnd && !(pid==process_id() && (gui_.windows.contains(hwnd) || hwnd==gui_.desktop))){
                    const auto remote=shared_gui_window(hwnd);if(!remote || remote->process!=pid)return finish(invalid);
                }
                const auto status=owner(pid,0,{});if(status)return finish(status);
                const auto out=p+2*unit;memory.check(GuestAddress{out},16,Access::Write);
                memory.store(GuestAddress{out},32,0);memory.store(GuestAddress{out+4},64,display_identity::adapter_luid);
                memory.store(GuestAddress{out+12},32,current);return finish(0);
            }
            const auto handle=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));
            if(name=="D3DKMTCheckVidPnExclusiveOwnership"){
                if(!gui_.kmt_adapters.contains(handle) || memory.load(GuestAddress{p+4},32))return finish(invalid);
                owner(0,0,{});return finish(current==2?0xc01e0006U:0U);
            }
            if(!gui_.kmt_devices.contains(handle))return finish(invalid);
            const auto count=memory.load(GuestAddress{p+3*unit},32);
            const auto types=memory.load(GuestAddress{p+unit},bits),sources=memory.load(GuestAddress{p+2*unit},bits);
            if(!count)return finish(!types && !sources?owner(process_id(),handle,0):0);
            if(count!=1 || !types || !sources || memory.load(GuestAddress{sources},32))return finish(invalid);
            return finish(owner(process_id(),handle,static_cast<std::uint32_t>(memory.load(GuestAddress{types},32))));
        }
        if(name=="D3DKMTQueryVideoMemoryInfo" || name=="NtGdiDdDDIChangeVideoMemoryReservation"){
            const bool change=name=="NtGdiDdDDIChangeVideoMemoryReservation";
            const unsigned unit=x86_?4U:8U;
            const auto p=arg(0);
            if(!p)return finish(0xc000000dU);
            const auto process=memory.load(GuestAddress{p},x86_?32:64);
            const auto adapter=static_cast<std::uint32_t>(memory.load(GuestAddress{p+unit},32));
            const auto group=memory.load(GuestAddress{p+unit+4},32);
            if(!gui_.kmt_adapters.contains(adapter) || group>1 || memory.load(GuestAddress{p+(change?24U:48U)},32))return finish(0xc000000dU);
            if(!change)memory.check(GuestAddress{p+16},32,Access::Write);
            std::array<std::uint64_t,5> values{group,change?memory.load(GuestAddress{p+16},64):0,0,0,0};
            std::uint32_t pid=process_id(),bits=0;
            std::uint64_t birth=0;
            auto error=process?process_memory_target(memory,process,change?0x200U:0x400U,pid,birth):0U;
            if(!error){
                const auto operation=change?gpu_budget_request::change:gpu_budget_request::query;
                error=pid==process_id()?local_process_information(operation,values,bits,-1):
                    remote_process_request(memory,pid,birth,14,operation,values,bits);
            }
            if(error){
                switch(error){
                case 5:return finish(0xc0000022U);
                case 6:return finish(0xc0000008U);
                case 50:return finish(0xc00000bbU);
                case 121:return finish(0xc00000b5U);
                default:return finish(0xc000000dU);
                }
            }
            if(!change)for(unsigned i=0;i<4;++i)memory.store(GuestAddress{p+16+i*8},64,values[i]);
            return finish(0);
        }
        if(name.starts_with("D3DKMT")){
            // 顯示核心介面：與 DXGI／DXCore 共用同一個虛擬 UMA 配接器（LUID、vendor 0x106B、共享記憶體額度）。
            // 未提供的查詢類型回 STATUS_NOT_SUPPORTED 並記錄一次診斷。版面依 d3dkmthk.h，指標欄位隨客體位元寬度調整。
            constexpr std::uint32_t ok=0,invalid=0xc000000dU,unsupported=0xc00000bbU,not_found=0xc0000225U;
            (void)unsupported;const auto p=arg(0);if(!p)return finish(invalid);const unsigned unit=x86_?4U:8U;const auto pointer_bits=static_cast<std::uint8_t>(unit*8);
            const auto luid=display_identity::adapter_luid;const bool available=metal_opengl_identity().has_value();
            const auto open=[&](std::uint64_t handle,std::uint64_t id)->std::uint32_t{
                if(!available)return not_found;
                if(gui_.kmt_adapters.size()>=4096)return 0xc000009aU;
                memory.check(GuestAddress{handle},4,Access::Write);
                memory.check(GuestAddress{id},8,Access::Write);
                const auto token=gui_.next_kmt_adapter++;
                gui_.kmt_adapters.insert(token);
                memory.store(GuestAddress{handle},32,token);
                memory.store(GuestAddress{id},64,luid);
                return ok;
            };
            const auto diagnose=[&](const char* api,std::uint32_t type){static std::set<std::pair<std::string,std::uint32_t>> seen;if(seen.emplace(api,type).second)std::fprintf(stderr,"[d3dkmt] %s type=%u 未支援\n",api,type);};
            if(name=="D3DKMTOpenAdapterFromHdc"){
                const auto dc=memory.load(GuestAddress{p},pointer_bits);const bool display=display_dc(dc);
                if(!display)return finish(invalid);memory.check(GuestAddress{p+unit+12},4,Access::Write);const auto status=open(p+unit,p+unit+4);if(!status)memory.store(GuestAddress{p+unit+12},32,0);return finish(status);
            }
            if(name=="D3DKMTOpenAdapterFromGdiDisplayName"){
                std::u16string device;for(unsigned i=0;i<32;++i){const auto c=static_cast<char16_t>(memory.load(GuestAddress{p+i*2},16));if(!c)break;device.push_back(c>=u'a' && c<=u'z'?static_cast<char16_t>(c-32):c);}
                if(device!=u"\\\\.\\DISPLAY1")return finish(invalid);memory.check(GuestAddress{p+76},4,Access::Write);const auto status=open(p+64,p+68);if(!status)memory.store(GuestAddress{p+76},32,0);return finish(status);
            }
            if(name=="D3DKMTOpenAdapterFromDeviceName"){
                const auto at=memory.load(GuestAddress{p},pointer_bits);if(!at)return finish(invalid);auto path=read_utf16(memory,GuestAddress{at});for(auto& c:path)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
                // 介面路徑：GUID_DISPLAY_DEVICE_ARRIVAL 或 GUID_DEVINTERFACE_DISPLAY_ADAPTER。
                if(path!=u"\\\\?\\pci#ven_106b&dev_0000&subsys_00000000&rev_00#0&0&0000#{1ca05180-a699-450a-9a0c-de4fbe3ddd89}" && path!=u"\\\\?\\pci#ven_106b&dev_0000&subsys_00000000&rev_00#0&0&0000#{5b45201d-f2f2-4f3b-85bb-30ff1f953599}")return finish(invalid);
                return finish(open(p+unit,p+unit+4));
            }
            if(name=="D3DKMTOpenAdapterFromLuid"){if((static_cast<std::uint64_t>(memory.load(GuestAddress{p+4},32))<<32|memory.load(GuestAddress{p},32))!=luid)return finish(invalid);return finish(open(p+8,p));}
            if(name=="D3DKMTCloseAdapter")return finish(gui_.kmt_adapters.erase(static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)))?ok:invalid);
            if(name=="D3DKMTEnumAdapters2" || name=="D3DKMTEnumAdapters"){
                const bool indirect=name=="D3DKMTEnumAdapters2";
                const auto capacity=indirect?memory.load(GuestAddress{p},32):16U;
                const auto entries=indirect?memory.load(GuestAddress{p+unit},pointer_bits):p+4;
                const std::uint32_t count=available?1U:0U;
                memory.check(GuestAddress{p},4,Access::Write);
                if(!entries){memory.store(GuestAddress{p},32,count);return finish(ok);}
                if(capacity<count)return finish(0xc0000023U);
                if(!indirect)memory.check(GuestAddress{entries},16*20,Access::Write);
                if(count){
                    memory.check(GuestAddress{entries},20,Access::Write);
                    const auto status=open(entries,entries+4);
                    if(status)return finish(status);
                    memory.store(GuestAddress{entries+12},32,1);
                    memory.store(GuestAddress{entries+16},32,0);
                }
                if(!indirect)memory.fill(GuestAddress{entries+count*20},0,(16-count)*20);
                memory.store(GuestAddress{p},32,count);
                return finish(ok);
            }
            if(name=="D3DKMTCheckOcclusion"){
                constexpr auto occluded=0xc01e0006U;
                const auto target=memory.load(GuestAddress{p},pointer_bits);
                if(!target)return finish(invalid);
                auto hwnd=target;
                std::uint32_t window_pid=process_id();
                std::set<std::uint64_t> visited;
                while(hwnd && hwnd!=gui_.desktop){
                    if(hwnd==gui_message_parent)return finish(hwnd==target?invalid:occluded);
                    if(!visited.insert(hwnd).second)return finish(invalid);
                    bool hidden,empty;
                    std::uint64_t parent;
                    if(const auto it=gui_.windows.find(hwnd);it!=gui_.windows.end()){
                        const auto& w=it->second;
                        hidden=!w.visible || w.show_command==2;
                        empty=!w.width || !w.height;
                        parent=w.parent;
                    }else{
                        const auto w=shared_gui_window(hwnd);
                        if(!w)return finish(invalid);
                        if(hwnd==target)window_pid=w->process;
                        hidden=!w->visible || w->show==2;
                        empty=!w->width || !w->height;
                        parent=w->parent;
                    }
                    if(empty && hwnd==target)return finish(ok);
                    if(hidden || empty)return finish(occluded);
                    hwnd=parent;
                }
                // 共用同一個 VidPn 來源；讀取同一份快照中的擁有者與類型。
                std::uint32_t type{},pid{};
                const auto dir=processes_ && runtime_.network.external_io?mapping_session():std::string{};
                const auto status=vidpn_ownership(gui_,dir,process_id(),0,0,{},type,&pid);
                if(status)return finish(status);
                return finish(type==2 && pid!=window_pid?occluded:ok);
            }
            if(name=="D3DKMTCreateDevice"){
                // WDDM 2 裝置不配發舊式 DMA 命令緩衝區與配置清單（輸出為空）。
                const auto adapter=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32));if(!gui_.kmt_adapters.contains(adapter))return finish(invalid);
                if(gui_.kmt_devices.size()>=4096)return finish(0xc000009aU);
                // 所有輸出可寫後才發布裝置，避免結構跨入不可寫頁面時遺留 handle。
                memory.check(GuestAddress{p+unit+4},x86_?28U:48U,Access::Write);
                const auto handle=gui_.next_kmt_device++;gui_.kmt_devices.emplace(handle,GuiState::KmtDevice{adapter});
                memory.store(GuestAddress{p+unit+4},32,handle);
                for(const auto offset:{x86_?12U:16U,x86_?20U:32U,x86_?28U:48U})memory.store(GuestAddress{p+offset},pointer_bits,0);
                for(const auto offset:{x86_?16U:24U,x86_?24U:40U,x86_?32U:56U})memory.store(GuestAddress{p+offset},32,0);
                return finish(ok);
            }
            if(name=="D3DKMTSetQueuedLimit"){
                // D3DKMT_SET_QUEUEDLIMIT_PRESENT 1：佇列中的 Present 上限；_VIDPNSOURCE 2：來源的待處理翻頁上限。
                const auto device=gui_.kmt_devices.find(static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)));if(device==gui_.kmt_devices.end())return finish(invalid);
                const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
                if(type==1){device->second.present_limit=static_cast<std::uint32_t>(memory.load(GuestAddress{p+8},32));return finish(ok);}
                if(type==2){if(memory.load(GuestAddress{p+8},32))return finish(invalid);device->second.flip_limit=static_cast<std::uint32_t>(memory.load(GuestAddress{p+12},32));return finish(ok);}
                return finish(invalid);
            }
            if(name=="D3DKMTCreateDCFromMemory"){
                // 以呼叫端記憶體（由上而下、指定 Pitch）建立 DIB 並選入相容的記憶體 DC；位元不複製，GDI 繪製直接寫回 pMemory。
                const auto bits=memory.load(GuestAddress{p},pointer_bits);const auto format=static_cast<std::uint32_t>(memory.load(GuestAddress{p+unit},32));
                const auto width=static_cast<std::uint32_t>(memory.load(GuestAddress{p+unit+4},32)),height=static_cast<std::uint32_t>(memory.load(GuestAddress{p+unit+8},32));
                const auto pitch=static_cast<std::uint32_t>(memory.load(GuestAddress{p+unit+12},32));
                const auto device_dc=memory.load(GuestAddress{p+unit+16},pointer_bits),colors=memory.load(GuestAddress{p+2*unit+16},pointer_bits);
                std::uint16_t depth=0;std::array<std::uint32_t,4> masks{0xff0000,0xff00,0xff,0};
                switch(format){
                case 20:depth=24;break;                                           // D3DDDIFMT_R8G8B8
                case 21:depth=32;masks[3]=0xff000000U;break;                      // A8R8G8B8
                case 22:depth=32;break;                                           // X8R8G8B8
                case 23:depth=16;masks={0xf800,0x7e0,0x1f,0};break;               // R5G6B5
                case 24:depth=16;masks={0x7c00,0x3e0,0x1f,0};break;               // X1R5G5B5
                case 25:depth=16;masks={0x7c00,0x3e0,0x1f,0x8000};break;          // A1R5G5B5
                case 26:case 30:depth=16;masks={0xf00,0xf0,0xf,0};break;          // A4R4G4B4、X4R4G4B4
                case 41:depth=8;break;                                            // P8
                default:return finish(invalid);
                }
                const auto minimum_pitch=((std::uint64_t{width}*depth+31)/32)*4;
                if(!bits || !device_dc || !width || !height || width>16384 || height>16384 ||
                    pitch<minimum_pitch || height>UINT32_MAX/pitch)return finish(invalid);
                const auto output=p+3*unit+16;
                memory.check(GuestAddress{output},2*unit,Access::Write);
                memory.check(GuestAddress{bits},static_cast<std::size_t>(pitch)*height,Access::Read);
                GuiBitmap bitmap{width,height,std::vector<std::uint32_t>(static_cast<std::size_t>(width)*height)};
                bitmap.dib_address=bits;bitmap.dib_external=true;bitmap.stride=pitch;bitmap.bits_per_pixel=depth;
                bitmap.signed_height=-static_cast<std::int32_t>(height);
                bitmap.dib_compression=depth==16?3U:0U;bitmap.dib_masks=masks;
                if(depth==8 && colors){
                    memory.check(GuestAddress{colors},1024,Access::Read);
                    for(unsigned i=0;i<256;++i)bitmap.dib_palette.push_back(static_cast<std::uint32_t>(memory.load(GuestAddress{colors+i*4},32))&0xffffffU);
                }else if(depth==8){
                    // 預設 P8 DIB 使用 3:3:2 色表，首尾各十色沿用系統保留色。
                    bitmap.dib_palette.assign(gui_default_palette.begin(),gui_default_palette.begin()+10);
                    for(unsigned i=10;i<246;++i)
                        bitmap.dib_palette.push_back((i&7U)*32U | ((i>>3)&7U)*0x2000U | (i>>6)*0x400000U);
                    bitmap.dib_palette.insert(bitmap.dib_palette.end(),gui_default_palette.begin()+10,gui_default_palette.end());
                }
                const auto dc=nested_service(state,memory,"gdi32.dll","CreateCompatibleDC",{device_dc});
                if(!dc)return finish(invalid);
                const auto handle=gui_.next_object++;
                try{
                    gui_.bitmaps.emplace(handle,std::move(bitmap));
                    import_dib_bitmap(memory,handle);
                    if(!nested_service(state,memory,"gdi32.dll","SelectObject",{dc,handle})){
                        gui_.bitmaps.erase(handle);
                        (void)nested_service(state,memory,"gdi32.dll","DeleteDC",{dc});
                        return finish(invalid);
                    }
                }catch(...){
                    gui_.bitmaps.erase(handle);
                    (void)nested_service(state,memory,"gdi32.dll","DeleteDC",{dc});
                    throw;
                }
                memory.store(GuestAddress{output},pointer_bits,dc);
                memory.store(GuestAddress{output+unit},pointer_bits,handle);
                return finish(ok);
            }
            if(name=="D3DKMTDestroyDCFromMemory"){
                const auto dc=memory.load(GuestAddress{p},pointer_bits),bitmap=memory.load(GuestAddress{p+unit},pointer_bits);
                if(!gui_.bitmaps.contains(bitmap) || dc==0x10000000U ||
                    nested_service(state,memory,"gdi32.dll","GetObjectType",{dc})!=10)return finish(invalid);
                (void)nested_service(state,memory,"gdi32.dll","DeleteDC",{dc});(void)nested_service(state,memory,"gdi32.dll","DeleteObject",{bitmap});
                return finish(ok);
            }
            const auto memory_limit=gpu_memory_stats();
            if(name=="D3DKMTQueryAdapterInfo"){
                const auto handle=static_cast<std::uint32_t>(memory.load(GuestAddress{p},32)),type=static_cast<std::uint32_t>(memory.load(GuestAddress{p+4},32));
                const auto data=memory.load(GuestAddress{p+8},pointer_bits);const auto size=static_cast<std::uint32_t>(memory.load(GuestAddress{p+8+unit},32));
                if(!gui_.kmt_adapters.contains(handle))return finish(invalid);if(!data)return finish(invalid);
                const auto need=[&](std::uint32_t bytes){if(size<bytes){static std::set<std::uint32_t> reported;if(reported.insert(type).second)std::fprintf(stderr,"[d3dkmt] QueryAdapterInfo type=%u size=%u 小於 %u\n",type,size,bytes);}return size>=bytes;};
                const auto text=[&](std::uint64_t at,std::u16string_view value,std::size_t limit){for(std::size_t i=0;i<limit;++i)memory.store(GuestAddress{at+i*2},16,i<value.size()?value[i]:0);};
                const auto identity=metal_opengl_identity();const auto renderer=identity?utf8_to_utf16(identity->first):std::u16string(display_identity::adapter_name);
                switch(type){
                case 48: // KMTQAITYPE_QUERYREGISTRY：共用 SetupAPI 的裝置、登錄與 DriverStore 後端。
                    return invoke_device_configuration(state,memory,service);
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
            if(copy && path){
                // 複製到磁碟：與 CloseMetaFile 共用寫檔流程，標頭改為磁碟 metafile。
                bytes[0]=2;bytes[1]=0;if(!write_guest_file(state,memory,wide?read_utf16(memory,GuestAddress{path}):utf8_to_utf16(read_ansi(memory,path)),bytes))return finish(0);
            }else if(path){
                // 磁碟 metafile 經一般檔案服務讀入，沿用映射、權限與共用模式（與 GetEnhMetaFile 共用）。
                auto loaded=read_guest_file(state,memory,wide?read_utf16(memory,GuestAddress{path}):utf8_to_utf16(read_ansi(memory,path)),18,metafile_limit);
                if(!loaded)return finish(0);bytes=std::move(*loaded);
                if(!valid_windows_metafile(bytes))return fail(13);bytes.resize(windows_metafile_size(bytes));
            }
            const auto handle=gui_.next_object++;gui_.windows_metafiles.emplace(handle,std::move(bytes));return finish(handle);
        }
    }catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(8);}catch(const std::invalid_argument&){return fail(87);}
    throw HostCallFault("尚未實作舊式 GDI 服務");
}
}
