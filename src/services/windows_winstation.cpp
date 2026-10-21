#include "x64arm/windows_services.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> winstation_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","CreateWindowStationA",4},{"user32.dll","CreateWindowStationW",4},{"user32.dll","OpenWindowStationA",3},{"user32.dll","OpenWindowStationW",3},
        {"user32.dll","CloseWindowStation",1},{"user32.dll","GetProcessWindowStation",0},{"user32.dll","SetProcessWindowStation",1},
        {"user32.dll","CreateDesktopA",6},{"user32.dll","CreateDesktopW",6},{"user32.dll","OpenDesktopA",4},{"user32.dll","OpenDesktopW",4},
        {"user32.dll","OpenInputDesktop",3},{"user32.dll","CloseDesktop",1},{"user32.dll","GetThreadDesktop",1},{"user32.dll","SetThreadDesktop",1},{"user32.dll","SwitchDesktop",1},
        {"user32.dll","GetUserObjectInformationA",5},{"user32.dll","GetUserObjectInformationW",5},{"user32.dll","SetUserObjectInformationA",4},{"user32.dll","SetUserObjectInformationW",4},
        {"user32.dll","GetUserObjectSecurity",5},{"user32.dll","SetUserObjectSecurity",3},
        {"user32.dll","AllowSetForegroundWindow",1},{"user32.dll","LockSetForegroundWindow",1},{"user32.dll","EnumDesktopWindows",3}
    };return table;
}
namespace {
std::u16string folded(std::u16string text){for(auto& c:text)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return text;}
std::uint32_t user_access(std::uint32_t access,bool desktop){
    if(access&0x10000000)access|=desktop?0xf01ff:0xf037f;
    if(access&0x80000000)access|=desktop?0x20041:0x20303;
    if(access&0x40000000)access|=desktop?0x200be:0x2001c;
    if(access&0x20000000)access|=desktop?0x20100:0x20060;
    if(access&0x02000000)access|=desktop?0xf01ff:0xf037f;
    return access&~0xf2000000U;
}
}
void WindowsServices::initialize_user_objects(){
    if(gui_.station_handle)return;
    gui_.user_objects.emplace(1,GuiUserObject{u"WinSta0",0,1,true});gui_.user_objects.emplace(2,GuiUserObject{u"Default",1,0,true});
    const auto descriptor=synchronization_descriptor(GuestMemory{},0).bytes;
    gui_.user_objects.at(1).security_descriptor=descriptor;gui_.user_objects.at(2).security_descriptor=descriptor;
    gui_.station_handle=runtime_.allocate_wait_handle();gui_.initial_desktop_handle=runtime_.allocate_wait_handle();
    gui_.user_handles.emplace(gui_.station_handle,GuiUserHandle{1,0xf037f});gui_.user_handles.emplace(gui_.initial_desktop_handle,GuiUserHandle{2,0xf01ff});
}
std::uint64_t WindowsServices::thread_desktop_object(std::uint64_t thread) const {
    const auto found=threads_.items.find(thread);if(found==threads_.items.end() || !found->second.desktop_handle)return 2;
    const auto handle=gui_.user_handles.find(found->second.desktop_handle);return handle==gui_.user_handles.end()?2:handle->second.object;
}
bool WindowsServices::interactive_station() const {return !gui_.station_handle || gui_.user_handles.at(gui_.station_handle).object==1;}
void WindowsServices::present_native_window(std::uint64_t handle,const GuiWindow& window){
    publish_gui_window(handle);
    if(!native_gui_)return;
    if(gui_.update_lock && window_update_locked(handle))return; // LockWindowUpdate：解除時統一重新呈現。
    if(window.desktop_object==gui_.input_desktop)native_gui_->present(handle,window);
    else{auto hidden=window;hidden.visible=false;native_gui_->present(handle,hidden);}
}
void WindowsServices::show_native_window(std::uint64_t handle,bool visible){
    publish_gui_window(handle);
    if(!native_gui_)return;const auto found=gui_.windows.find(handle);
    native_gui_->show(handle,visible && found!=gui_.windows.end() && found->second.desktop_object==gui_.input_desktop);
}
std::optional<std::uint32_t> WindowsServices::invoke_winstation(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    initialize_user_objects();const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const bool wide=symbol.ends_with('W');
    if(symbol=="GetUserObjectSecurity" || symbol=="SetUserObjectSecurity")return invoke_object_security(state,memory,service);
    std::uint64_t result=0;const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    if((symbol.starts_with("CreateDesktop") || symbol=="SwitchDesktop") && (job_ui_restrictions()&0x40U)){fail(5);arg.return_to(state,result);return std::nullopt;}
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto string=[&](std::uint64_t p){return p?(wide?read_utf16(memory,GuestAddress{p}):utf8_to_utf16_lossy(read_ansi(memory,p))):std::u16string{};};
    const auto station=gui_.user_handles.at(gui_.station_handle).object;
    const auto handle=gui_.user_handles.find(service.arguments?arg(0):0);
    const auto open=[&](std::uint64_t object,std::uint32_t access,bool inherit){result=runtime_.allocate_wait_handle();gui_.user_handles.emplace(result,GuiUserHandle{object,access});if(inherit)runtime_.handle_flags[result]=1;};
    try{
    if(symbol=="GetProcessWindowStation")result=gui_.station_handle;
    else if(symbol=="GetThreadDesktop"){
        const auto thread=threads_.items.find(thread_from_identifier(d(0)));if(thread==threads_.items.end() || thread->second.status==ThreadStatus::Exited)fail(87);
        else result=thread->second.desktop_handle?thread->second.desktop_handle:gui_.initial_desktop_handle;
    }else if(symbol.starts_with("CreateWindowStation") || symbol.starts_with("OpenWindowStation") || symbol.starts_with("CreateDesktop") || symbol.starts_with("OpenDesktop") || symbol=="OpenInputDesktop"){
        const bool desktop=symbol.find("Desktop")!=symbol.npos,create=symbol.starts_with("Create"),input=symbol=="OpenInputDesktop";
        const auto flags=input?d(0):desktop?d(create?3:1):create?d(1):0;
        const auto security_address=create?arg(desktop?5:3):0;
        bool inherit=create?false:arg(desktop && !input?2:1)!=0;
        std::uint64_t descriptor_address{};
        if(security_address){const unsigned size=x86_?12:24;memory.check(GuestAddress{security_address},size,Access::Read);if(memory.load(GuestAddress{security_address},32)!=size)throw SecurityDescriptorError{87};inherit=memory.load(GuestAddress{security_address+(x86_?8:16)},32)!=0;descriptor_address=memory.load(GuestAddress{security_address+(x86_?4:8)},x86_?32:64);}
        const auto desired=d(desktop && !input?(create?4:3):2),access=user_access(desired,desktop);
        auto name=input?gui_.user_objects.at(gui_.input_desktop).name:string(arg(0));if(create && !desktop && name.empty())name=u"Service-0x0-3e7$";
        if(flags&~1U || name.empty() || name.size()>255 || name.find(u'\\')!=name.npos || access&~(desktop?0xf01ffU:0xf037fU))fail(87);
        else if(desktop && create && (arg(1) || arg(2)))fail(87);
        else if(desktop && !create && ((access&0xe0000) && (access&0x81)!=0x81))fail(5);
        else if(desktop && create && (!(gui_.user_handles.at(gui_.station_handle).access&8) || !(access&2)))fail(5);
        else if(input && station!=1)fail(5);
        else{
            const auto object=std::find_if(gui_.user_objects.begin(),gui_.user_objects.end(),[&](const auto& item){return (item.second.station!=0)==desktop && (!desktop || item.second.station==station) && folded(item.second.name)==folded(name);});
            const auto mapping=[&](bool interactive){return desktop?std::array<std::uint32_t,4>{0x20041,0x200be,0x20100,0xf01ff}:std::array<std::uint32_t,4>{interactive?0x20303U:0x20103U,interactive?0x2001cU:0x2000cU,0x20060,0xf037f};};
            if(object!=gui_.user_objects.end()){
                if(create && !desktop && (flags&1))fail(183);else open(object->first,object_security_access(object->second.security_descriptor,desired,mapping(object->first==1)),inherit);
            }else if(!create)fail(2);
            else if(gui_.user_objects.size()>=256)fail(8);
            else{
                auto descriptor=desktop && !descriptor_address?gui_.user_objects.at(station).security_descriptor:synchronization_descriptor(memory,security_address).bytes;
                const auto granted=object_security_access(descriptor,desired,mapping(false));
                const auto id=gui_.next_user_object++;gui_.user_objects.emplace(id,GuiUserObject{std::move(name),desktop?station:0,desktop?flags:0,false,std::move(descriptor)});open(id,granted,inherit);
            }
        }
    }else if(symbol=="AllowSetForegroundWindow" || symbol=="LockSetForegroundWindow"){
        if(!interactive_station() || gui_.input_desktop!=thread_desktop_object(threads_.current))fail(5);
        else if(symbol=="LockSetForegroundWindow"){
            if(d(0)!=1 && d(0)!=2)fail(87);else{gui_.foreground_locked=d(0)==1;result=1;}
        }else if(gui_.foreground_locked || !gui_.menu_tracks.empty())fail(5);
        else if(d(0)!=process_id() && d(0)!=UINT32_MAX)fail(50); // 跨宿主程序的前景授權尚無共用 broker。
        else{gui_.foreground_grant=d(0);result=1;}
    }else if(handle==gui_.user_handles.end())fail(6);
    else{
        auto& object=gui_.user_objects.at(handle->second.object);const bool desktop=object.station!=0;
        if(symbol=="CloseDesktop" || symbol=="CloseWindowStation"){
            if(desktop!=(symbol=="CloseDesktop"))fail(6);
            else if(arg(0)==gui_.station_handle || arg(0)==gui_.initial_desktop_handle || (runtime_.handle_flags[arg(0)]&2) || std::any_of(threads_.items.begin(),threads_.items.end(),[&](const auto& t){return t.second.status!=ThreadStatus::Exited && t.second.desktop_handle==arg(0);}))fail(170);
            else{
                const auto id=handle->second.object;gui_.user_handles.erase(handle);runtime_.handle_flags.erase(arg(0));result=1;
                // Desktop／station 本身由所有 handles、輸入桌面與子桌面持有。
                if(!object.initial && id!=gui_.input_desktop && std::none_of(gui_.user_handles.begin(),gui_.user_handles.end(),[&](const auto& h){return h.second.object==id;}) && std::none_of(gui_.user_objects.begin(),gui_.user_objects.end(),[&](const auto& o){return o.second.station==id;}) && std::none_of(gui_.windows.begin(),gui_.windows.end(),[&](const auto& w){return w.second.desktop_object==id;})){gui_.user_objects.erase(id);gui_.station_namespaces.erase(id);}
            }
        }else if(symbol=="SetProcessWindowStation"){
            if(desktop)fail(6);else if(gui_.clipboard_open_thread)fail(170);
            else{
                const auto next=handle->second.object;
                if(next!=station){std::swap(static_cast<GuiStationNamespace&>(gui_),gui_.station_namespaces[station]);std::swap(static_cast<GuiStationNamespace&>(gui_),gui_.station_namespaces[next]);}
                gui_.station_handle=arg(0);result=1;
            }
        }else if(symbol=="SetThreadDesktop"){
            if(!desktop)fail(6);else if(object.station!=station)fail(5);
            else if(thread_desktop_object(threads_.current)!=handle->second.object && (std::any_of(gui_.windows.begin(),gui_.windows.end(),[&](const auto& w){return w.second.thread==threads_.current;}) || std::any_of(gui_.hooks.begin(),gui_.hooks.end(),[&](const auto& h){return h.second.owner==threads_.current;})))fail(170);
            else{threads_.items.at(threads_.current).desktop_handle=arg(0);result=1;}
        }else if(symbol=="SwitchDesktop"){
            if(!desktop)fail(6);else if(!(handle->second.access&0x100) || object.station!=1)fail(5);
            else{gui_.input_desktop=handle->second.object;gui_.focus=gui_.active=gui_.capture=0;for(const auto& [hwnd,w]:gui_.windows){present_gui(hwnd);show_native_window(hwnd,w.visible);}result=1;}
        }else if(symbol=="EnumDesktopWindows"){
            if(!desktop)fail(6);else if(!(handle->second.access&0x40))fail(5);else if(!arg(1))fail(87);
            else{std::vector<GuestCallback> callbacks;for(const auto hwnd:gui_.z_order){const auto w=gui_.windows.find(hwnd);if(w!=gui_.windows.end() && !w->second.parent && w->second.desktop_object==handle->second.object)callbacks.push_back({arg(1),{hwnd,arg(2)}});}if(callbacks.empty())result=1;else{start_callbacks(state,memory,std::move(callbacks),Continuation::WindowEnumeration);return std::nullopt;}}
        }else if(symbol.starts_with("GetUserObjectInformation")){
            std::vector<std::uint8_t> bytes;
            const auto number=[&](std::uint32_t value){for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));};
            bool valid=true;
            if(d(1)==1){number(runtime_.handle_flags[arg(0)]&1);number(0);number(object.flags);}
            else if(d(1)==2 || d(1)==3){const auto text=d(1)==2?object.name:desktop?u"Desktop":u"WindowStation";if(wide)bytes=utf16_bytes(text);else{const auto ansi=utf16_to_utf8(text);bytes.assign(ansi.begin(),ansi.end());bytes.push_back(0);}}
            else if(d(1)==6 && desktop)number(handle->second.object==gui_.input_desktop);
            else{fail(d(1)==4 || d(1)==5?50:87);valid=false;}
            if(valid){if(arg(4))memory.store(GuestAddress{arg(4)},32,bytes.size());if(d(3)<bytes.size())fail(122);else if(!arg(2) && !bytes.empty())fail(87);else{memory.write(GuestAddress{arg(2)},bytes);result=1;}}
        }else if(symbol.starts_with("SetUserObjectInformation")){
            if(d(1)!=1)fail(d(1)==7?50:87);
            else if(d(3)!=12 || !arg(2))fail(87);
            else if(!(handle->second.access&(desktop?0x80:0x10)))fail(5);
            else{const auto flags=memory.load(GuestAddress{arg(2)+8},32);if(memory.load(GuestAddress{arg(2)+4},32) || flags&~1U || (!desktop && flags!=object.flags))fail(87);
                else{runtime_.handle_flags[arg(0)]=(runtime_.handle_flags[arg(0)]&~1U)|(memory.load(GuestAddress{arg(2)},32)?1U:0U);if(desktop)object.flags=static_cast<std::uint32_t>(flags);result=1;}}
        }
    }
    }catch(const SecurityDescriptorError& error){fail(error.code);}
    catch(const MemoryFault&){fail(998);}catch(const std::bad_alloc&){fail(8);}
    complete_call(state,memory,result);return std::nullopt;
}
}
