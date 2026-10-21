#include "x64arm/windows_services.hpp"
#include "x64arm/security_descriptor.hpp"
#include "windows_etw_support.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> etw_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
        {"advapi32.dll","StartTraceW",3},{"advapi32.dll","StartTraceA",3},
        {"advapi32.dll","StopTraceW",3,1},{"advapi32.dll","StopTraceA",3,1},
        {"advapi32.dll","QueryTraceW",3,1},{"advapi32.dll","QueryTraceA",3,1},
        {"advapi32.dll","FlushTraceW",3,1},{"advapi32.dll","FlushTraceA",3,1},
        {"advapi32.dll","ControlTraceW",4,1},{"advapi32.dll","ControlTraceA",4,1},
        {"advapi32.dll","OpenTraceW",1},{"advapi32.dll","OpenTraceA",1},
        {"advapi32.dll","ProcessTrace",4},{"advapi32.dll","CloseTrace",1,1},
        {"advapi32.dll","SetTraceCallback",2},{"advapi32.dll","RemoveTraceCallback",1},
        {"sechost.dll","SetTraceCallback",2},{"sechost.dll","RemoveTraceCallback",1},
        {"advapi32.dll","EnableTrace",5,0x10},{"advapi32.dll","EnableTraceEx",9,0x64},
        {"advapi32.dll","EnableTraceEx2",8,0x31},{"advapi32.dll","EventActivityIdControl",2},
        {"advapi32.dll","EventWriteString",4,5}
    };return entries;
}
std::optional<std::uint32_t> WindowsServices::invoke_etw(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name.starts_with("StartTrace") || name.starts_with("ControlTrace") || name.starts_with("StopTrace") || name.starts_with("QueryTrace") || name.starts_with("FlushTrace") || name.starts_with("EnableTrace"))return invoke_etw_controller(state,memory,service);
    if(name.starts_with("OpenTrace") || name=="CloseTrace" || name=="ProcessTrace" || name=="SetTraceCallback" || name=="RemoveTraceCallback")return invoke_etw_consumer(state,memory,service);
    return invoke_etw_provider(state,memory,service);
}
bool WindowsServices::etw_session_access(const EtwSessionInfo& session){
    initialize_tokens();const auto& token=runtime_.token_objects.at(effective_token_id());
    if(session.private_logger && token_user_sid_string(effective_token_id())==session.owner)return true;
    for(auto name:{"SYSTEM","LOCAL SERVICE","NETWORK SERVICE"})if(const auto account=security::find_account(name);account && token.user.sid==security::account_sid(*account))return true;
    for(const auto& group:token.groups){if(!(group.attributes&4U) || (group.attributes&16U))continue;
        if(const auto account=security::find_account("Administrators");account && group.sid==security::account_sid(*account))return true;
        // Performance Log Users，S-1-5-32-559；沿用同一 token 的啟用／deny-only 屬性。
        const std::vector<std::uint8_t> perf{1,2,0,0,0,0,0,5,32,0,0,0,0x2f,2,0,0};if(group.sid==perf)return true;
    }return false;
}
std::uint64_t WindowsServices::etw_timestamp(std::uint32_t clock,const GuestState& state)const{
    (void)state;if(clock==1)return runtime_.clock_ticks;
    const auto value=clock==2?static_cast<unsigned __int128>(runtime_.filetime_base)+runtime_.clock_ticks:static_cast<unsigned __int128>(runtime_.clock_ticks)*240;
    if(value>UINT64_MAX)throw etw::guest::Fault{534};return static_cast<std::uint64_t>(value);
}
std::uint32_t WindowsServices::flush_etw_file(std::uint64_t handle){
    const auto found=etw_sessions_.find(handle);if(found==etw_sessions_.end() || !found->second.file)return 0;
    try{const auto bytes=runtime_.traces.file_bytes(handle);std::size_t at=0;std::uint32_t error=0;
        while(at<bytes.size()){const auto result=found->second.file->write(std::span(bytes).subspan(at),at);if(result.error || !result.bytes){error=result.error?result.error:29U;break;}at+=static_cast<std::size_t>(result.bytes);}
        if(!error)error=found->second.file->truncate(bytes.size());if(!error)error=found->second.file->flush();
        if(error && found->second.file_errors!=UINT32_MAX)++found->second.file_errors;return error;
    }catch(const etw::Error& error){return error.failure==etw::Failure::TooLarge?1502U:13U;}
}
std::uint64_t WindowsServices::etw_next_deadline()const{std::uint64_t next=UINT64_MAX;for(const auto& [id,session]:etw_sessions_){const auto core=runtime_.traces.session(id);if(core && core->active && !core->pending.empty())next=std::min(next,session.deadline);}return next;}
void WindowsServices::poll_etw(GuestMemory& memory){
    for(auto& [id,session]:etw_sessions_){const auto core=runtime_.traces.session(id);if(!core || !core->active || session.deadline>runtime_.clock_ticks)continue;
        (void)runtime_.traces.flush(id);(void)flush_etw_file(id);
        const auto seconds=session.flush_timer?session.flush_timer:session.mode&0x100U?1U:0U;
        session.deadline=seconds && runtime_.clock_ticks<=UINT64_MAX-std::uint64_t{seconds}*10000000?runtime_.clock_ticks+std::uint64_t{seconds}*10000000:UINT64_MAX;
    }
    if(runtime_.traces.notification_count()){
        auto current=threads_.items.at(threads_.current).context;(void)dispatch_etw_notifications(current,memory);
    }
}
bool WindowsServices::dispatch_etw_notifications(GuestState& state,GuestMemory& memory,bool late,std::uint64_t only_registration){
    while(late || registered_waits_.size()<64){
        auto notification=runtime_.traces.take_notification(only_registration);if(!notification)return false;
        const auto found=runtime_.event_providers.find(notification->registration);
        if(found==runtime_.event_providers.end() || !found->second.callback){(void)runtime_.traces.complete_notification(notification->id);continue;}
        const auto registration=found->second;const auto filter=std::find_if(notification->enable.filters.begin(),notification->enable.filters.end(),[](const auto& item){return item.type==0 || item.type==0x80000000U;});
        const auto extent=notification->classic?64U:48U+(filter==notification->enable.filters.end()?0U:filter->data.size());
        const auto allocation=allocate_heap(memory,extent);if(!allocation){(void)runtime_.traces.complete_notification(notification->id);throw etw::guest::Fault{8};}
        EtwCallbackOperation operation{*notification,allocation,0,threads_.current,late};std::vector<GuestCallback> calls;
        try{
            memory.write(GuestAddress{allocation},etw::Bytes(extent));
            if(notification->classic){memory.store(GuestAddress{allocation},32,48);memory.store(GuestAddress{allocation+8},64,notification->logger);
                memory.write(GuestAddress{allocation+24},registration.id);memory.store(GuestAddress{allocation+44},32,0x20000);
                calls.push_back({registration.callback,{notification->code==0?5U:4U,registration.context,0,allocation}});
            }else{memory.write(GuestAddress{allocation},notification->source);std::uint64_t descriptor=0;
                if(filter!=notification->enable.filters.end()){descriptor=allocation+16;memory.store(GuestAddress{descriptor},64,allocation+48);
                    memory.store(GuestAddress{descriptor+8},32,filter->data.size());memory.store(GuestAddress{descriptor+12},32,filter->type);memory.write(GuestAddress{allocation+48},filter->data);}
                calls.push_back({registration.callback,{allocation,notification->code,notification->aggregate.level,notification->aggregate.any,notification->aggregate.all,descriptor,registration.context},false,false,0x18});
            }
            if(late){etw_callbacks_.emplace(notification->id,operation);start_callbacks(state,memory,std::move(calls),Continuation::EtwCallback,notification->id);return true;}
            const auto worker=acquire_wait_worker(state,memory,std::max(UINT64_C(65536),runtime_.image.stack_size));if(!worker)throw etw::guest::Fault{last_error_?last_error_:8};
            operation.worker=worker;operation.owner=worker;const auto wait_handle=next_registered_wait_++;RegisteredWait wait;wait.worker=worker;wait.callback_batch=std::move(calls);
            wait.armed=false;wait.ready=true;wait.closing=true;wait.work_item=true;wait.etw_operation=notification->id;
            etw_callbacks_.emplace(notification->id,std::move(operation));registered_waits_.emplace(wait_handle,std::move(wait));wait_workers_[worker]=wait_handle;
            auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};
        }catch(...){etw_callbacks_.erase(notification->id);(void)free_heap(memory,allocation,state);(void)runtime_.traces.complete_notification(notification->id);throw;}
    }return false;
}
void WindowsServices::discard_etw_callback(GuestState& state,GuestMemory& memory,std::uint64_t id){const auto found=etw_callbacks_.find(id);if(found==etw_callbacks_.end())return;
    const auto allocation=found->second.allocation;etw_callbacks_.erase(found);(void)runtime_.traces.complete_notification(id);(void)free_heap(memory,allocation,state);
}
std::optional<std::uint32_t> WindowsServices::resume_etw_callback(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t result){
    const auto found=etw_callbacks_.find(id);if(found==etw_callbacks_.end())throw HostCallFault("ETW callback 沒有 operation");
    const bool worker=found->second.worker!=0,classic=found->second.notification.classic;discard_etw_callback(state,memory,id);
    if(worker)return resume_registered_wait(state,memory);complete_call(state,memory,classic?static_cast<std::uint32_t>(result):0);return {};
}
void WindowsServices::cleanup_etw_thread(GuestState& state,GuestMemory& memory,std::uint64_t thread){
    if(etw_processes_.contains(thread))discard_etw_process(state,memory,thread);etw_enable_waits_.erase(thread);
    std::vector<std::uint64_t> callbacks;for(const auto& [id,operation]:etw_callbacks_)if(operation.worker==thread)callbacks.push_back(id);
    for(auto id:callbacks)discard_etw_callback(state,memory,id);
}
}
