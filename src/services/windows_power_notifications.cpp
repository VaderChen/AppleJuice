#include "x64arm/windows_services.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> power_notification_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","RegisterSuspendResumeNotification",2},{"user32.dll","UnregisterSuspendResumeNotification",1},
        {"powrprof.dll","PowerRegisterSuspendResumeNotification",3},{"powrprof.dll","PowerUnregisterSuspendResumeNotification",1},
        {"user32.dll","RegisterPowerSettingNotification",3},{"user32.dll","UnregisterPowerSettingNotification",1},
        {"powrprof.dll","PowerSettingRegisterNotification",4},{"powrprof.dll","PowerSettingUnregisterNotification",1}
    };return table;
}
void WindowsServices::set_power_notification_backend(std::shared_ptr<NativePowerNotifications> backend){
    if(!power_notifications_.empty())throw std::logic_error("電源通知註冊後不可替換事件來源");
    power_notification_backend_=std::move(backend);
}
bool WindowsServices::power_notification_pending(std::uint64_t owner)const{
    return std::any_of(registered_waits_.begin(),registered_waits_.end(),[&](const auto& entry){return entry.second.power_owner==owner;}) ||
        std::any_of(gui_.sent_messages.begin(),gui_.sent_messages.end(),[&](const auto& entry){return entry.second.power_owner==owner;});
}
void WindowsServices::poll_power_notification_events(GuestMemory& memory){
    if(power_notifications_.empty() || !power_notification_backend_)return;
    const auto saved_error=last_error_;
    const auto events=power_notification_backend_->poll();
    for(auto& [owner,registration]:power_notifications_){
        if(registration.closing)continue;
        for(const auto& event:events){
            const bool setting=event.type==0x8013;
            if(registration.settings==setting && (!setting || registration.setting==event.setting))registration.pending.push_back(event);
        }
        if(registration.pending.empty() || power_notification_pending(owner))continue;
        const auto& event=registration.pending.front();
        if(registration.window){
            const auto window=gui_.windows.find(registration.window);
            if(window==gui_.windows.end()){registration.pending.clear();continue;}
            if(gui_.sent_messages.size()>=8192)continue;
            GuiSentMessage request;request.receiver=window->second.thread;request.wide=window->second.unicode;
            request.power_owner=owner;request.message={registration.window,event.type,registration.settings?registration.payload:0,0x218};
            if(registration.settings){
                if(event.data.size()>64)throw HostCallFault("電源設定通知資料超出支援結構大小");
                memory.write(GuestAddress{registration.payload},event.setting);
                memory.store(GuestAddress{registration.payload+16},32,event.data.size());
                memory.write(GuestAddress{registration.payload+20},event.data);
            }
            gui_.sent_messages.emplace(gui_.next_object++,std::move(request));
        }else{
            if(registered_waits_.size()>=64)continue;
            const auto worker=acquire_wait_worker(threads_.items.at(threads_.current).context,memory,std::max(UINT64_C(65536),runtime_.image.stack_size));
            if(!worker)continue;
            if(registration.settings){
                if(event.data.size()>64)throw HostCallFault("電源設定通知資料超出支援結構大小");
                memory.write(GuestAddress{registration.payload},event.setting);
                memory.store(GuestAddress{registration.payload+16},32,event.data.size());
                memory.write(GuestAddress{registration.payload+20},event.data);
            }
            RegisteredWait work;work.worker=worker;work.power_owner=owner;work.armed=false;work.ready=true;work.closing=true;
            work.callback_batch={{registration.callback,{registration.context,event.type,registration.settings?registration.payload:0}}};
            const auto id=next_registered_wait_++;registered_waits_.emplace(id,std::move(work));wait_workers_[worker]=id;
            auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};
        }
        registration.pending.erase(registration.pending.begin());
    }
    last_error_=saved_error;
}
std::optional<std::uint32_t> WindowsServices::invoke_power_notification_event(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const bool user=service.family=="user32.dll",settings=name.find("Setting")!=name.npos;
    const bool unregister=name.starts_with("Unregister") || name.starts_with("PowerUnregister") || name=="PowerSettingUnregisterNotification";
    const auto finish=[&](std::uint64_t value,std::uint32_t error=0){
        if(user && error)last_error_=error;
        arg.return_to(state,user?value:error);return std::optional<std::uint32_t>{};
    };
    if(unregister){
        const auto owner=arg(0);auto& caller=threads_.items.at(threads_.current);
        const auto found=power_notifications_.find(owner);
        if(found==power_notifications_.end() || found->second.settings!=settings || found->second.user32!=user)return finish(0,6);
        const bool self=std::any_of(registered_waits_.begin(),registered_waits_.end(),[&](const auto& entry){return entry.second.power_owner==owner && entry.second.active && entry.second.worker==threads_.current;}) ||
            std::any_of(gui_.sent_messages.begin(),gui_.sent_messages.end(),[&](const auto& entry){return entry.second.power_owner==owner && entry.second.active && entry.second.receiver==threads_.current;});
        if(self)return finish(0,1131); // 等待自己的回呼結束會造成死結。
        found->second.closing=true;found->second.pending.clear();
        std::vector<std::uint64_t> cancel;
        for(const auto& [id,work]:registered_waits_)if(work.power_owner==owner && !work.active)cancel.push_back(id);
        for(const auto id:cancel)release_registered_wait(id);
        std::erase_if(gui_.sent_messages,[&](const auto& entry){return entry.second.power_owner==owner && !entry.second.active;});
        if(power_notification_pending(owner)){
            caller.status=ThreadStatus::Waiting;caller.wait={};caller.wait.kind=ThreadWaitKind::PowerNotificationCompletion;caller.wait.handles={owner};
            schedule(state,memory);return std::nullopt;
        }
        if(found->second.payload)(void)free_heap(memory,found->second.payload,state);
        power_notifications_.erase(found);if(power_notifications_.empty())power_notification_backend_.reset();
        if(caller.wait.kind==ThreadWaitKind::PowerNotificationCompletion)caller.wait={};
        return finish(1);
    }
    std::uint64_t payload=0;
    try{
        const auto recipient=arg(user?0:(settings?2:1));
        const auto flags=static_cast<std::uint32_t>(arg(user?(settings?2:1):(settings?1:0)));
        const auto output=user?0:arg(settings?3:2);
        if(!user){if(!output)return finish(0,87);memory.check(GuestAddress{output},x86_?4:8,Access::Write);}
        if(flags==1 && settings)return finish(0,50); // 尚未具備服務控制事件派送，不註冊空殼。
        if((user && settings && flags!=0) || (user && !settings && flags!=0 && flags!=2) || (!user && flags!=2))return finish(0,87);
        if(!recipient)return finish(0,87);
        PowerNotification registration;registration.settings=settings;registration.user32=user;
        if(flags==0){
            const auto window=gui_.windows.find(recipient);
            if(window==gui_.windows.end())return finish(0,1400);
            if(!threads_.items.contains(window->second.thread) || threads_.items.at(window->second.thread).status==ThreadStatus::Exited)return finish(0,1400);
            registration.window=recipient;
        }else{
            memory.check(GuestAddress{recipient},x86_?8:16,Access::Read);
            registration.callback=memory.load(GuestAddress{recipient},x86_?32:64);
            registration.context=memory.load(GuestAddress{recipient+(x86_?4:8)},x86_?32:64);
            if(!registration.callback)return finish(0,87);
            memory.check(GuestAddress{registration.callback},1,Access::Execute);
        }
        if(settings){
            const auto guid=arg(user?1:0);if(!guid)return finish(0,87);
            memory.read(GuestAddress{guid},registration.setting);
        }
        if(power_notifications_.size()>=4096)return finish(0,8);
        if(!runtime_.network.external_io)return finish(0,50);
        if(!power_notification_backend_)power_notification_backend_=native_power_notifications();
        if(!power_notification_backend_)return finish(0,50);
        // 清除註冊前的事件；既有註冊仍由共用輪詢收到它們。
        if(power_notifications_.empty())(void)power_notification_backend_->poll();
        else poll_power_notification_events(memory);
        if(settings){
            const auto initial=power_notification_backend_->current_setting(registration.setting);
            if(!initial || initial->size()>64)return finish(0,50);
            registration.pending.push_back({0x8013,registration.setting,*initial});
            payload=allocate_heap(memory,84);if(!payload)return finish(0,8);registration.payload=payload;
        }
        const auto owner=runtime_.allocate_wait_handle();
        power_notifications_.emplace(owner,std::move(registration));
        if(output)memory.store(GuestAddress{output},x86_?32:64,owner);
        return finish(user?owner:0);
    }catch(const MemoryFault&){if(payload)(void)free_heap(memory,payload,state);return finish(0,998);}
    catch(const std::bad_alloc&){if(payload)(void)free_heap(memory,payload,state);return finish(0,8);}
}
// 預設沿用本機事件來源；顯式提供快照來源時共用同一 API 入口與協調輪詢。
void WindowsServices::poll_power_notifications(GuestMemory& memory){
    poll_power_notification_events(memory);poll_power_setting_notifications(memory);
}
std::optional<std::uint32_t> WindowsServices::invoke_power_notification(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const bool window_setting=service.family=="user32.dll" && (service.symbol=="RegisterPowerSettingNotification" || service.symbol=="UnregisterPowerSettingNotification");
    const WindowsCall arg(state,memory,service);
    if(window_setting && (service.symbol=="UnregisterPowerSettingNotification"?gui_.power_registrations.contains(arg(0)):bool(power_monitor_)))
        return invoke_power_setting_notification(state,memory,service);
    return invoke_power_notification_event(state,memory,service);
}

}
