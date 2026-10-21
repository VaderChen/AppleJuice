#include "x64arm/windows_services.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> registered_wait_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","QueueUserWorkItem",3,0,0,0x4},{"kernel32.dll","RegisterWaitForSingleObject",6,0,0,0x30},{"kernel32.dll","UnregisterWait",1},{"kernel32.dll","UnregisterWaitEx",2},
        {"kernel32.dll","CreateTimerQueue",0},{"kernel32.dll","CreateTimerQueueTimer",7,0,0,0x70},
        {"kernel32.dll","ChangeTimerQueueTimer",4,0,0,0xc},{"kernel32.dll","DeleteTimerQueueTimer",3},
        {"kernel32.dll","DeleteTimerQueueEx",2},{"kernel32.dll","DeleteTimerQueue",1},
        {"internal-threadpool","WaitDispatch",1}
        ,{"kernel32.dll","CreateThreadpool",1},{"kernel32.dll","CloseThreadpool",1},
        {"kernel32.dll","SetThreadpoolThreadMaximum",2,0,0,0x2},{"kernel32.dll","SetThreadpoolThreadMinimum",2,0,0,0x2}
        ,{"kernel32.dll","CreateThreadpoolWork",3},{"kernel32.dll","SubmitThreadpoolWork",1},
        {"kernel32.dll","WaitForThreadpoolWorkCallbacks",2,0,0,0x2},{"kernel32.dll","CloseThreadpoolWork",1},
        {"kernel32.dll","CreateThreadpoolTimer",3},{"kernel32.dll","SetThreadpoolTimer",4,0,0,0xc},
        {"kernel32.dll","IsThreadpoolTimerSet",1},{"kernel32.dll","WaitForThreadpoolTimerCallbacks",2,0,0,0x2},{"kernel32.dll","CloseThreadpoolTimer",1},
        {"kernel32.dll","CreateThreadpoolWait",3},{"kernel32.dll","SetThreadpoolWait",3},
        {"kernel32.dll","WaitForThreadpoolWaitCallbacks",2,0,0,0x2},{"kernel32.dll","CloseThreadpoolWait",1},
        {"kernel32.dll","TrySubmitThreadpoolCallback",3},{"kernel32.dll","CallbackMayRunLong",1},
        {"kernel32.dll","DisassociateCurrentThreadFromCallback",1},{"kernel32.dll","SetEventWhenCallbackReturns",2},
        {"kernel32.dll","ReleaseSemaphoreWhenCallbackReturns",3,0,0,0x4},{"kernel32.dll","ReleaseMutexWhenCallbackReturns",2},
        {"kernel32.dll","LeaveCriticalSectionWhenCallbackReturns",2},{"kernel32.dll","FreeLibraryWhenCallbackReturns",2}
    };return table;
}
void WindowsServices::release_registered_wait(std::uint64_t handle){
    const auto found=registered_waits_.find(handle);if(found==registered_waits_.end())return;
    const auto registration=found->second;auto& worker=threads_.items.at(registration.worker);
    if(registration.bluetooth_delivery)finish_bluetooth_delivery(registration.bluetooth_delivery);
    if(worker.status!=ThreadStatus::Exited){worker.status=ThreadStatus::Waiting;worker.wait={};worker.wait.kind=ThreadWaitKind::RegisteredWait;}
    worker.impersonation_token=0;collect_tokens();
    wait_workers_[registration.worker]=0;registered_waits_.erase(found);
    if(registration.pool_instance)release_threadpool_instance(registration.pool_instance);
    if(const auto event=runtime_.wait_objects.find(registration.completion_event);event!=runtime_.wait_objects.end())event->second.set_signal(true);
    if(registration.timer_owner){
        const auto timer=queue_timers_.find(registration.timer_owner);
        if(timer!=queue_timers_.end() && timer->second.outstanding)--timer->second.outstanding;
        collect_timer_queues();
    }
}
void WindowsServices::poll_registered_waits(GuestMemory& memory){
    (void)memory;
    for(auto& [handle,wait]:registered_waits_){
        (void)handle;if(!wait.armed || wait.active || wait.ready || wait.closing)continue;
        auto& worker=threads_.items.at(wait.worker);bool signaled=false;
        const auto object=runtime_.wait_objects.find(wait.target);
        // 共用事件以 acquire 原子讀取探查。未發訊且尚未逾時不需進入命名空間鎖；
        // 真正消耗自動重設事件或決定逾時前，仍在鎖內重新檢查，避免多個 waiter 重複領取。
        if(object!=runtime_.wait_objects.end() && !runtime_.wait_state(object->second).signaled() && wait.deadline>runtime_.clock_ticks)continue;
        SharedEventLock lock(object!=runtime_.wait_objects.end() && object->second.shared_event?object->second.shared_event->lock_descriptor():-1);
        if(object!=runtime_.wait_objects.end())signaled=runtime_.wait_state(object->second).signaled();
        else if(const auto id=thread_id(wait.target))signaled=threads_.items.at(id).status==ThreadStatus::Exited;
        else if(const auto result=process_signaled(wait.target))signaled=*result;
        if(!signaled && wait.deadline>runtime_.clock_ticks)continue;
        wait.timed_out=!signaled;wait.ready=true;wait.armed=false;
        if(signaled && object!=runtime_.wait_objects.end())runtime_.wait_state(object->second).consume_signal();
        worker.status=ThreadStatus::Runnable;
    }
}
std::optional<std::uint32_t> WindowsServices::resume_registered_wait(GuestState& state,GuestMemory& memory){
    const auto handle=wait_workers_.at(threads_.current);auto& wait=registered_waits_.at(handle);
    if(wait.pool_instance && !wait.pool_cleanup){
        if(cleanup_threadpool_callback(state,memory,wait.pool_instance))return std::nullopt;
        wait.pool_cleanup=true;
    }
    wait.active=false;
    for(const auto pointer:wait.callback_storage)free_heap(memory,pointer,state);
    wait.callback_storage.clear();
    if(wait.device_owner)finish_device_notification(state,memory,wait.device_owner);
    if(wait.closing)release_registered_wait(handle);
    else {
        wait.armed=!(wait.flags&8U);wait.deadline=wait.armed && wait.milliseconds!=UINT32_MAX?runtime_.clock_ticks+std::uint64_t{wait.milliseconds}*10000:UINT64_MAX;
        auto& worker=threads_.items.at(threads_.current);worker.status=ThreadStatus::Waiting;worker.wait={};worker.wait.kind=ThreadWaitKind::RegisteredWait;worker.wait.deadline=wait.deadline;
    }
    schedule(state,memory);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_registered_wait(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    if(name.find("Threadpool")!=std::string_view::npos || name.ends_with("WhenCallbackReturns") || name=="CallbackMayRunLong" || name=="DisassociateCurrentThreadFromCallback")return invoke_threadpool(state,memory,service);
    if(name.find("TimerQueue")!=std::string_view::npos)return invoke_timer_queue(state,memory,service);
    const auto finish=[&](std::uint64_t value,std::uint32_t error=0){if(error)last_error_=error;arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(name=="WaitDispatch"){
        const auto handle=wait_workers_.at(threads_.current);auto& wait=registered_waits_.at(handle);
        if(!wait.ready || wait.active)throw HostCallFault("註冊等待 callback 狀態不一致");wait.active=true;wait.ready=false;
        if(wait.perflib_operation)return advance_perflib(state,memory,wait.perflib_operation);
        if(!wait.callback && wait.callback_batch.empty())return resume_registered_wait(state,memory);
        if(!wait.callback_batch.empty()){start_callbacks(state,memory,wait.callback_batch,wait.etw_operation?Continuation::EtwCallback:Continuation::RegisteredWait,wait.etw_operation);return std::nullopt;}
        auto arguments=std::vector<std::uint64_t>{wait.parameter};if(!wait.work_item)arguments.push_back(wait.timed_out?1U:0U);
        start_callbacks(state,memory,{{wait.callback,std::move(arguments)}},Continuation::RegisteredWait);return std::nullopt;
    }
    if(name=="QueueUserWorkItem"){
        if(!arg(0))return finish(0,87);
        if(arg(2)&~UINT64_C(0x90))return finish(0,50); // 長工作／持續執行緒提示；不提供 impersonation 或 APC I/O worker。
        memory.check(GuestAddress{arg(0)},1,Access::Execute);if(registered_waits_.size()>=64)return finish(0,8);
        const auto stack=std::max(UINT64_C(65536),runtime_.image.stack_size);
        const auto worker=acquire_wait_worker(state,memory,stack);if(!worker)return finish(0,last_error_);
        const auto handle=next_registered_wait_++;RegisteredWait wait;wait.callback=arg(0);wait.parameter=arg(1);wait.worker=worker;
        wait.armed=false;wait.ready=true;wait.closing=true;wait.work_item=true;registered_waits_.emplace(handle,wait);wait_workers_[worker]=handle;
        auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};return finish(1);
    }
    if(name=="RegisterWaitForSingleObject"){
        if(!arg(0)||!arg(2))return finish(0,87);
        if(arg(5)&~UINT64_C(0x9c))return finish(0,50); // 不宣告 impersonation／APC I/O worker 能力。
        const auto object=runtime_.wait_objects.find(arg(1));
        if(object==runtime_.wait_objects.end() && !thread_id(arg(1)) && !process_signaled(arg(1)).has_value())return finish(0,6);
        if(object!=runtime_.wait_objects.end() && (object->second.kind==WaitObject::Kind::Mutex || !(object->second.access&0x100000)))return finish(0,87);
        if(thread_id(arg(1)) && !(thread_access(arg(1))&0x100000))return finish(0,5);
        memory.check(GuestAddress{arg(0)},x86_?4:8,Access::Write);memory.check(GuestAddress{arg(2)},1,Access::Execute);
        if(registered_waits_.size()>=64)return finish(0,8);
        const auto worker=acquire_wait_worker(state,memory,65536);if(!worker)return finish(0,last_error_);
        const auto handle=next_registered_wait_++;RegisteredWait wait;wait.target=arg(1);wait.callback=arg(2);wait.parameter=arg(3);wait.worker=worker;
        wait.milliseconds=static_cast<std::uint32_t>(arg(4));wait.flags=static_cast<std::uint32_t>(arg(5));
        wait.deadline=wait.milliseconds==UINT32_MAX?UINT64_MAX:runtime_.clock_ticks+std::uint64_t{wait.milliseconds}*10000;
        registered_waits_.emplace(handle,wait);wait_workers_[worker]=handle;
        auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::RegisteredWait;thread.wait.deadline=wait.deadline;
        memory.store(GuestAddress{arg(0)},x86_?32:64,handle);return finish(1);
    }
    auto& calling=threads_.items.at(threads_.current);
    if(calling.wait.kind==ThreadWaitKind::RegisteredWaitCompletion && !registered_waits_.contains(arg(0))){calling.wait={};return finish(1);}
    const auto found=registered_waits_.find(arg(0));if(found==registered_waits_.end())return finish(0,6);
    auto& wait=found->second;const auto completion=name=="UnregisterWaitEx"?arg(1):0;const bool blocking=completion==(x86_?UINT32_MAX:UINT64_MAX);
    if(completion && !blocking){const auto event=runtime_.wait_objects.find(completion);if(event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event)return finish(0,6);}
    if(wait.closing)return finish(0,6);wait.closing=true;wait.armed=false;wait.completion_event=blocking?0:completion;
    if(!wait.active){release_registered_wait(arg(0));return finish(1);}
    if(blocking && wait.worker!=threads_.current){calling.status=ThreadStatus::Waiting;calling.wait={};calling.wait.kind=ThreadWaitKind::RegisteredWaitCompletion;calling.wait.handles={arg(0)};schedule(state,memory);return std::nullopt;}
    return finish(0,997);
}
}
