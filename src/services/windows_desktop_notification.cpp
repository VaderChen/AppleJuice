#include "x64arm/windows_services.hpp"
#include "x64arm/desktop_notification_abi.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
namespace x64arm {
std::span<const WindowsServiceDescriptor> desktop_notification_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"shell32.dll","Shell_NotifyIconA",2},{"shell32.dll","Shell_NotifyIconW",2},
        {"shell32.dll","SHQueryUserNotificationState",1},
        {"user32.dll","FlashWindow",2},{"user32.dll","FlashWindowEx",1}
    };return table;
}
void WindowsServices::poll_desktop_notifications(){
    gui_.attention.tick(runtime_.clock_ticks,native_gui_ && !native_gui_->input_active()?0:gui_.active);
    if(!native_gui_)return;
    if(gui_.desktop_notification_state)if(const auto current=native_gui_->notification_state()){
        const auto old=*gui_.desktop_notification_state;
        if((old==1)!=(*current==1) || (old==4)!=(*current==4))for(const auto& [window,model]:gui_.windows)
            if(!model.parent)enqueue_gui_message({window,0,0,0x1a});
        gui_.desktop_notification_state=current;
    }
    auto owners=gui_.native_attention_windows;
    for(const auto& [window,value]:gui_.attention.values()){(void)value;owners.insert(window);}
    for(const auto window:owners){
        const auto found=gui_.attention.values().find(window);
        const bool caption=found!=gui_.attention.values().end() && (found->second.flags&1) && found->second.phase;
        const bool tray=found!=gui_.attention.values().end() && (found->second.flags&2) && found->second.phase;
        (void)native_gui_->attention(window,caption,tray);
    }
    gui_.native_attention_windows.clear();
    for(const auto& [window,value]:gui_.attention.values()){(void)value;gui_.native_attention_windows.insert(window);}
}
std::optional<std::uint32_t> WindowsServices::invoke_desktop_notification(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return done(name=="SHQueryUserNotificationState"?0x80070000U|error:0);};
    try{
        if(name=="SHQueryUserNotificationState"){
            if(!arg(0))return done(0x80070057U);
            memory.check(GuestAddress{arg(0)},4,Access::Write);
            std::uint32_t value=5;
            if(native_gui_){const auto current=native_gui_->notification_state();if(!current)return done(0x80004005U);value=*current;}
            else{
                const auto active=gui_.windows.find(gui_.active);
                if(thread_desktop_object(threads_.current)!=gui_.input_desktop)value=1;
                else if(active!=gui_.windows.end() && active->second.visible && active->second.width>=1440 && active->second.height>=900 && !(active->second.style&0xc00000U))value=2;
            }
            memory.store(GuestAddress{arg(0)},32,value);if(!gui_.desktop_notification_state)gui_.desktop_notification_state=value;poll_desktop_notifications();return done(0);
        }
        if(name=="FlashWindowEx" || name=="FlashWindow"){
            const bool extended=name=="FlashWindowEx";std::uint64_t window=arg(0);std::uint32_t flags=0,count=0,timeout=0;
            if(extended){
                if(!window)return fail(87);const auto cb=memory.load(GuestAddress{window},32);if(cb!=(x86_?20:32))return fail(87);
                memory.check(GuestAddress{window},cb,Access::Read);const auto base=window;window=memory.load(GuestAddress{base+(x86_?4:8)},x86_?32:64);
                flags=static_cast<std::uint32_t>(memory.load(GuestAddress{base+(x86_?8:16)},32));count=static_cast<std::uint32_t>(memory.load(GuestAddress{base+(x86_?12:20)},32));timeout=static_cast<std::uint32_t>(memory.load(GuestAddress{base+(x86_?16:24)},32));
            }
            const auto found=gui_.windows.find(window);if(found==gui_.windows.end())return fail(1400);
            const bool prior=gui_.attention.caption(window,gui_.active==window);
            auto old=gui_.attention;
            const auto result=extended?gui_.attention.set(window,flags,count,timeout,500,runtime_.clock_ticks):gui_.attention.toggle(window,arg(1)!=0,(found->second.style&0x20000000U)?2:1);
            if(result!=desktop::Status::Ok)return fail(result==desktop::Status::Limit?8:87);
            const auto next=gui_.attention.values().find(window);
            try{if(native_gui_ && !native_gui_->attention(window,next!=gui_.attention.values().end() && (next->second.flags&1) && next->second.phase,next!=gui_.attention.values().end() && (next->second.flags&2) && next->second.phase)){
                gui_.attention=std::move(old);return fail(50);
            }}catch(...){gui_.attention=std::move(old);throw;}
            poll_desktop_notifications();return done(prior);
        }
        const auto action=static_cast<std::uint32_t>(arg(0));const auto pointer=arg(1);if(!pointer || action>4)return fail(87);
        const bool wide=name.ends_with('W');const desktop::Layout layout(x86_,wide);
        const auto cb=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer},32));if(!layout.accepts(cb))return fail(87);
        memory.check(GuestAddress{pointer},cb,Access::Read);
        const auto load=[&](std::uint32_t offset,unsigned bits=32){return memory.load(GuestAddress{pointer+offset},static_cast<std::uint8_t>(bits));};
        const auto text=[&](std::uint32_t offset,std::uint32_t count){
            std::u16string value;
            if(wide){for(std::uint32_t i=0;i<count;++i){const auto unit=static_cast<char16_t>(load(offset+2*i,16));if(!unit)break;value+=unit;}}
            else{std::string bytes;for(std::uint32_t i=0;i<count;++i){const auto unit=static_cast<char>(load(offset+i,8));if(!unit)break;bytes+=unit;}value=utf8_to_utf16_lossy(bytes);}
            return value;
        };
        desktop::Change change;auto& input=change.value;
        input.owner=load(layout.owner,x86_?32:64);input.id=static_cast<std::uint32_t>(load(layout.id));input.flags=static_cast<std::uint32_t>(load(layout.flags));
        if(input.flags&32){if(cb<layout.v3)return fail(87);WindowsGuid guid;memory.read(GuestAddress{pointer+layout.guid},guid);input.guid=guid;}
        if(action==0 && !gui_.windows.contains(input.owner))return fail(1400);
        if(action==4){if(cb<layout.v2)return fail(87);input.version=static_cast<std::uint32_t>(load(layout.version));}
        if(action==0 || action==1){
            if(input.flags&1)input.callback=static_cast<std::uint32_t>(load(layout.callback));
            const auto icon=[&](std::uint64_t handle){if(!handle)return GuiIcon{};const auto found=gui_.icons.find(handle);if(found==gui_.icons.end() || !found->second.is_icon)throw std::invalid_argument("無效的 HICON");return found->second;};
            if(input.flags&2)input.image=icon(load(layout.icon,x86_?32:64));
            if(input.flags&4)input.tip=text(layout.tip,cb==layout.v1?64:128);
            if(input.flags&8){if(cb<layout.v2)return fail(87);input.state=static_cast<std::uint32_t>(load(layout.state));change.state_mask=static_cast<std::uint32_t>(load(layout.state+4));}
            if(input.flags&16){
                if(cb<layout.v2)return fail(87);input.info=text(layout.info,256);input.title=text(layout.title,64);input.timeout=static_cast<std::uint32_t>(load(layout.version));input.info_flags=static_cast<std::uint32_t>(load(layout.info_flags));
                if((input.info_flags&15)==4){const auto balloon=cb==layout.size?load(layout.balloon,x86_?32:64):0;input.balloon=icon(balloon?balloon:load(layout.icon,x86_?32:64));}
            }
        }
        auto old=gui_.status_icons;const auto result=gui_.status_icons.apply(action,change);
        if(result.status!=desktop::Status::Ok)return fail(result.status==desktop::Status::Limit?8:result.status==desktop::Status::Missing?1168:result.status==desktop::Status::Exists?183:87);
        try{if(native_gui_ && !(action==3?native_gui_->status_focus(result.token):native_gui_->status_icons(gui_.status_icons.values()))){gui_.status_icons=std::move(old);return fail(50);}}
        catch(...){gui_.status_icons=std::move(old);throw;}
        return done(1);
    }catch(const MemoryFault&){return fail(998);}catch(const std::invalid_argument&){return fail(87);}catch(const std::bad_alloc&){return fail(8);}
}
}
