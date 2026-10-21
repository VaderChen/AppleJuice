#include "x64arm/windows_services.hpp"
#include "x64arm/windows_keyboard.hpp"
#include "x64arm/windows_pointer.hpp"
#include <algorithm>

namespace x64arm {
// LockWindowUpdate、ActivateKeyboardLayout 與觸控注入：共用 GUI 狀態、輸入佇列與 WM_POINTER 管線。
namespace {
// 注入觸控裝置的固定識別；不對應任何宿主硬體。
constexpr input::Guid injected_touch_device{'x','6','4','a','r','m','-','t','o','u','c','h','-','i','n','j'};
}
std::span<const WindowsServiceDescriptor> user_session_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","LockWindowUpdate",1},{"user32.dll","ActivateKeyboardLayout",2},
        {"user32.dll","InitializeTouchInjection",2},{"user32.dll","InjectTouchInput",2}
    };return table;
}
bool WindowsServices::window_update_locked(std::uint64_t handle)const{
    for(unsigned depth=0;handle && depth<256;++depth){
        if(handle==gui_.update_lock)return true;const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())return false;handle=found->second.parent;
    }
    return false;
}
std::uint64_t WindowsServices::selected_keyboard_layout(std::uint64_t thread)const{
    if(const auto found=gui_.thread_keyboard_layouts.find(thread);found!=gui_.thread_keyboard_layouts.end())return found->second;
    return gui_.process_keyboard_layout;
}
std::optional<std::uint32_t> WindowsServices::invoke_user_session(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    std::uint64_t result=0;const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    try{
        if(symbol=="LockWindowUpdate"){
            if(!arg(0)){
                // 解除鎖定：重新呈現整個視窗樹，並讓客體重新繪製被擋下的內容。
                const auto locked=gui_.update_lock;if(!locked){arg.return_to(state,0);return std::nullopt;}
                gui_.update_lock=0;
                if(gui_.windows.contains(locked)){
                    std::vector<std::uint64_t> tree;
                    for(const auto& [handle,window]:gui_.windows){(void)window;std::uint64_t cursor=handle;for(unsigned depth=0;cursor && depth<256;++depth){if(cursor==locked){tree.push_back(handle);break;}cursor=gui_.windows.at(cursor).parent;if(cursor && !gui_.windows.contains(cursor))break;}}
                    for(const auto handle:tree){invalidate_window(handle);if(gui_.windows.at(handle).redraw)present_native_window(handle,gui_.windows.at(handle));}
                }
                result=1;
            }else {
                const auto found=gui_.windows.find(arg(0));
                if(found==gui_.windows.end())fail(1400);
                else if(gui_.update_lock && gui_.update_lock!=arg(0))fail(5); // 同時只允許鎖定一個視窗。
                else {gui_.update_lock=arg(0);result=1;}
            }
        }else if(symbol=="ActivateKeyboardLayout"){
            const auto flags=u32(1);
            if(flags&~(0x8U|0x100U|0x10000U|0x40000000U)){fail(87);arg.return_to(state,0);return std::nullopt;} // KLF_REORDER、SETFORPROCESS、SHIFTLOCK、RESET
            std::vector<std::uint64_t> layouts{0x04090409};
            if(native_gui_)for(const auto& method:native_input_methods())if(std::find(layouts.begin(),layouts.end(),method.layout)==layouts.end())layouts.push_back(method.layout);
            auto current=selected_keyboard_layout(threads_.current);
            if(!current){current=0x04090409;if(native_gui_)for(const auto& method:native_input_methods())if(method.current){current=method.layout;break;}}
            const auto mask=x86_?UINT64_C(0xffffffff):UINT64_MAX;auto target=arg(0)&mask;
            const auto position=std::find(layouts.begin(),layouts.end(),current);const auto index=position==layouts.end()?0:static_cast<std::size_t>(position-layouts.begin());
            if(target==1)target=layouts[(index+1)%layouts.size()];                      // HKL_NEXT
            else if(target==0)target=layouts[(index+layouts.size()-1)%layouts.size()];   // HKL_PREV
            if(std::find(layouts.begin(),layouts.end(),target)==layouts.end()){fail(87);arg.return_to(state,0);return std::nullopt;}
            if(flags&0x100U){gui_.process_keyboard_layout=target;gui_.thread_keyboard_layouts.clear();} // KLF_SETFORPROCESS
            else gui_.thread_keyboard_layouts[threads_.current]=target;
            result=current;
            if(target!=current && gui_.focus && gui_.windows.contains(gui_.focus) && gui_.windows.at(gui_.focus).thread==threads_.current){
                // 依 Windows 契約通知焦點視窗輸入語言已切換。
                GuiMessage message{gui_.focus,0,target,0x51};message.thread=threads_.current;enqueue_gui_message(std::move(message));
            }
        }else if(symbol=="InitializeTouchInjection"){
            const auto count=u32(0),mode=u32(1);
            if(!count || count>256 || mode<1 || mode>3)fail(87);
            else {
                if(!pointer_input_)pointer_input_=std::make_shared<PointerInputState>();auto& bridge=*pointer_input_;
                if(const auto existing=bridge.identities.find(injected_touch_device);existing!=bridge.identities.end()){
                    std::erase_if(bridge.current,[&](const auto& item){return item.second.device==existing->second;});
                    bridge.devices.erase(existing->second);bridge.identities.erase(existing);
                }
                if(bridge.devices.size()>=256 || bridge.next_id+count>2562)fail(8);
                else {
                    input::PointerDevice device;device.device=injected_touch_device;device.type=3;device.name="Touch Injection";
                    device.logical_rectangle={0,0,1439,899};device.max_contacts=static_cast<std::uint16_t>(count);
                    const auto handle=runtime_.allocate_wait_handle();bridge.identities.emplace(injected_touch_device,handle);
                    PointerDeviceState entry{device,true,false,bridge.next_id,true};bridge.devices.emplace(handle,std::move(entry));bridge.next_id+=count;
                    touch_injection_feedback_=mode;result=1;
                }
            }
        }else if(symbol=="InjectTouchInput"){
            const auto count=u32(0);const auto device=pointer_input_?pointer_input_->identities.find(injected_touch_device):std::map<input::Guid,std::uint64_t>::iterator{};
            if(!pointer_input_ || device==pointer_input_->identities.end()){fail(21);arg.return_to(state,0);return std::nullopt;} // ERROR_NOT_READY
            const auto& entry=pointer_input_->devices.at(device->second);
            const unsigned size=x86_?136:144,info=x86_?88:96,points=x86_?24:32;
            if(!count || count>entry.device.max_contacts || !arg(1)){fail(87);arg.return_to(state,0);return std::nullopt;}
            memory.check(GuestAddress{arg(1)},std::size_t{size}*count,Access::Read);
            if(!interactive_station()){fail(5);arg.return_to(state,0);return std::nullopt;}
            std::vector<input::PointerSample> samples;std::vector<std::uint32_t> seen;
            for(std::uint32_t i=0;i<count;++i){
                const auto at=arg(1)+std::uint64_t{i}*size;const auto load=[&](unsigned offset){return static_cast<std::uint32_t>(memory.load(GuestAddress{at+offset},32));};
                const auto type=load(0),id=load(4),flags=load(12);
                // PT_TOUCH；DOWN 必須帶 INRANGE|INCONTACT，UPDATE 帶 INRANGE，UP 單獨出現。
                const bool down=flags&0x10000U,update=flags&0x20000U,up=flags&0x40000U;
                const bool valid=type==2 && id<entry.device.max_contacts && std::find(seen.begin(),seen.end(),id)==seen.end() &&
                    (int(down)+int(update)+int(up))==1 && (!down || (flags&6U)==6U) && (!update || (flags&2U)) && (!up || !(flags&6U)) && !(flags&~0x7f6fffU);
                if(!valid){fail(87);arg.return_to(state,0);return std::nullopt;}
                seen.push_back(id);
                input::PointerSample sample;sample.device=injected_touch_device;sample.contact=id;
                sample.x=std::bit_cast<std::int32_t>(load(points));sample.y=std::bit_cast<std::int32_t>(load(points+4));
                if(sample.x<0 || sample.y<0 || sample.x>1439 || sample.y>899){fail(87);arg.return_to(state,0);return std::nullopt;}
                sample.in_range=(flags&2U)!=0;sample.in_contact=(flags&4U)!=0;
                const auto mask=load(info+4);sample.mask=mask;
                if(mask&4U)sample.pressure=load(info+36);if(mask&2U)sample.rotation=load(info+32);
                if(mask&1U)for(unsigned j=0;j<4;++j)sample.contact_rectangle[j]=std::bit_cast<std::int32_t>(load(info+8+j*4));
                samples.push_back(sample);
            }
            for(const auto& sample:samples)deliver_pointer_sample(sample);
            result=1;
        }else throw HostCallFault("未知的使用者工作階段入口");
    }catch(const MemoryFault&){fail(998);}catch(const std::bad_alloc&){fail(8);}
    arg.return_to(state,result);return std::nullopt;
}
}
