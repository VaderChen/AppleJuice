#include "x64arm/windows_services.hpp"
#include "x64arm/windows_power_notifications.hpp"
#include <algorithm>

namespace x64arm {
void WindowsServices::set_power_notification_monitor(std::shared_ptr<NativePowerMonitor> monitor){
    if(!gui_.power_registrations.empty())throw HostCallFault("電源通知來源須在訂閱前設定");
    power_monitor_=std::move(monitor);
}
bool WindowsServices::finish_power_notification(const GuiMessage& message){
    if(message.message!=0x218 || message.wparam!=0x8013)return false;
    const auto found=gui_.power_deliveries.find(message.lparam);
    if(found==gui_.power_deliveries.end() || found->second.window!=message.window)return false;
    gui_.power_notification_buffers.push_back(message.lparam);gui_.power_deliveries.erase(found);return true;
}
void WindowsServices::cancel_power_notifications(std::uint64_t window,std::uint64_t thread,std::uint64_t registration){
    std::erase_if(gui_.power_registrations,[&](const auto& entry){
        const auto target=gui_.windows.find(entry.second.window);
        return registration?entry.first==registration:window?entry.second.window==window:thread && target!=gui_.windows.end() && target->second.thread==thread;
    });
    for(auto it=gui_.sent_messages.begin();it!=gui_.sent_messages.end();){
        const auto& request=it->second;
        const bool match=request.power_notification && (registration?request.power_notification==registration:window?request.message.window==window:thread && request.receiver==thread);
        // 已進入 WNDPROC 的資料仍有效到回呼結束；執行緒終止則不再有回呼使用者。
        if(match && (!request.active || thread)){finish_power_notification(request.message);it=gui_.sent_messages.erase(it);}else ++it;
    }
}
void WindowsServices::poll_power_setting_notifications(GuestMemory& memory){
    if(gui_.power_registrations.empty() || !power_monitor_ || !runtime_.network.external_io)return;
    const auto snapshot=power_monitor_->sample();
    for(auto it=gui_.power_registrations.begin();it!=gui_.power_registrations.end();){
        const auto id=it->first;auto& registration=it->second;const auto window=gui_.windows.find(registration.window);
        if(window==gui_.windows.end() || window->second.destroying){++it;cancel_power_notifications(0,0,id);continue;}
        const auto setting=static_cast<std::size_t>(std::find(power::notification_settings.begin(),power::notification_settings.end(),registration.guid)-power::notification_settings.begin());
        const auto value=power::notification_value(snapshot,setting);++it;
        if(!value || registration.last==value)continue;
        if(gui_.sent_messages.size()>=8192 || gui_.power_deliveries.size()>=8192)return;
        // 固定 24-byte 結構與 x86／x64 無關，資料由送出者保留到 callback 結束。
        if(gui_.power_notification_buffers.empty())gui_.power_notification_buffers.push_back(runtime_allocate(memory,{},24));
        const auto buffer=gui_.power_notification_buffers.back();memory.write(GuestAddress{buffer},registration.guid);
        memory.store(GuestAddress{buffer+16},32,4);memory.store(GuestAddress{buffer+20},32,*value);
        GuiSentMessage request;request.message={registration.window,0x8013,buffer,0x218};request.receiver=window->second.thread;
        request.wide=window->second.unicode;request.power_notification=id;
        gui_.power_deliveries.emplace(buffer,GuiState::PowerDelivery{id,registration.window});
        try{gui_.sent_messages.emplace(gui_.next_object++,std::move(request));}
        catch(...){gui_.power_deliveries.erase(buffer);throw;}
        gui_.power_notification_buffers.pop_back();registration.last=value;
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_power_setting_notification(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);std::uint64_t result=0;
    const auto fail=[&](std::uint32_t error){last_error_=error;};
    try{
        if(service.symbol=="UnregisterPowerSettingNotification"){
            if(!gui_.power_registrations.contains(arg(0)))fail(6);
            else {cancel_power_notifications(0,0,arg(0));result=1;}
        }else {
            const auto flags=static_cast<std::uint32_t>(arg(2));
            if(flags>1 || !arg(1))fail(87);
            else if(flags==1)fail(6); // 現有服務查詢 handle 並不是 SERVICE_STATUS_HANDLE。
            else if(const auto target=gui_.windows.find(arg(0));target==gui_.windows.end() || target->second.destroying)fail(1400);
            else {
                power::Guid guid{};memory.read(GuestAddress{arg(1)},guid);
                if(std::find(power::notification_settings.begin(),power::notification_settings.end(),guid)==power::notification_settings.end())fail(50);
                else if(!runtime_.network.external_io)fail(50);
                else if(gui_.power_registrations.size()>=4096)fail(8);
                else {
                    if(!power_monitor_)power_monitor_=make_native_power_monitor();
                    if(!power_monitor_)fail(50);
                    else {const auto id=gui_.next_object++;gui_.power_registrations.emplace(id,GuiState::PowerRegistration{arg(0),guid,{}});result=id;}
                }
            }
        }
    }catch(const MemoryFault&){fail(998);}catch(const std::bad_alloc&){fail(8);}
    arg.return_to(state,result);return std::nullopt;
}
}
