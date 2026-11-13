#include "x64arm/windows_services.hpp"
#include "window_presentation_fault.hpp"
namespace x64arm {
std::optional<std::uint32_t> WindowsServices::invoke_angle(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(!runtime_.network.external_io)throw HostCallFault("GPU context 不支援 deterministic compare");
    if(!native_angle_)throw HostCallFault("ANGLE Metal 代理未啟用");
    const auto commands=angle_commands();const auto found=std::find_if(commands.begin(),commands.end(),[&](const auto& c){return c.name==service.symbol && c.family==service.family;});
    if(found==commands.end())throw HostCallFault("未知 ANGLE 入口");
    const WindowsCall call(state,memory,service);
    const auto result=native_angle_->invoke(state,memory,call,*found,threads_.current,
        [&](auto bytes){const auto pointer=allocate_heap(memory,std::max<std::size_t>(bytes.size(),1));if(!pointer)throw std::bad_alloc{};memory.write(GuestAddress{pointer},bytes);return pointer;},
        [&](std::uint64_t pointer){if(pointer)(void)free_heap(memory,pointer,state);},
        [&](auto name){return resolve_service(name.starts_with("egl")?"libegl.dll":"libglesv2.dll",name,false).value;},
        [&](std::uint64_t window){
            AngleWindow result;
            if(gui_.windows.contains(window)){const auto& w=gui_.windows.at(window);result.width=std::max(1U,w.width);result.height=std::max(1U,w.height);if(native_gui_)result.view=native_gui_->graphics_view(window);return result;}
            // 其他程序的視窗（Windows 上同一桌面的 HWND 可跨程序繪圖；Chromium／CEF 的 GPU 程序即對瀏覽器程序的視窗建立表面）：
            // 以客戶區大小建立離屏表面，每次 swap 經共用影格送到視窗擁有者，沿用 D3D 交換鏈的跨程序呈現。
            if(const auto remote=shared_gui_window(window);remote && remote->process!=process_id()){
                result.width=std::max(1U,remote->width);result.height=std::max(1U,remote->height);
                result.present=[this,window](unsigned width,unsigned height,std::span<const std::uint8_t> pixels){
                    try{publish_remote_pixels(window,width,height,pixels);return true;}
                    catch(const WindowPresentationFault&){return false;}
                    catch(const HostCallFault&){return false;}};
            }
            return result;});
    call.return_to(state,result,found->result==AngleType::U64 || found->result==AngleType::I64);return {};
}
}
