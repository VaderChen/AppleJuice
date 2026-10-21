#include "x64arm/windows_services.hpp"
#include <algorithm>

namespace x64arm {
std::uint64_t WindowsServices::acquire_wait_worker(const GuestState& state,GuestMemory& memory,std::uint64_t stack){
    const auto idle=std::find_if(wait_workers_.begin(),wait_workers_.end(),[&](const auto& entry){
        const auto& thread=threads_.items.at(entry.first);
        return !entry.second && !threadpool_worker_reserved(entry.first) && thread.status!=ThreadStatus::Exited && thread.stack_size>=stack;
    });
    if(idle!=wait_workers_.end())return idle->first;
    const auto handle=create_guest_thread(state,memory,resolve("internal-threadpool","WaitDispatch").value,0,stack,0);
    if(!handle)return 0;
    const auto worker=thread_id(handle);runtime_.process_thread_handles.erase(handle);return worker;
}
void WindowsServices::collect_timer_queues(){
    const auto signal=[&](std::uint64_t handle){
        if(const auto event=runtime_.wait_objects.find(handle);event!=runtime_.wait_objects.end())event->second.set_signal(true);
    };
    std::erase_if(queue_timers_,[&](const auto& entry){
        if(!entry.second.closing || entry.second.outstanding)return false;
        signal(entry.second.completion_event);return true;
    });
    std::erase_if(timer_queues_,[&](const auto& entry){
        if(!entry.second.closing || std::any_of(queue_timers_.begin(),queue_timers_.end(),[&](const auto& timer){return timer.second.queue==entry.first;}))return false;
        signal(entry.second.completion_event);return true;
    });
}
void WindowsServices::poll_timer_queues(GuestMemory& memory){
    timer_workers_exhausted_=false;
    if(queue_timers_.empty())return;
    const auto saved_error=last_error_;
    // 以每次到期為獨立工作，允許前一次 callback 等待期間繼續派送。
    // 池滿時保留到期時間，後續輪詢再派送，不丟棄已到期的工作。
    for(auto& [handle,timer]:queue_timers_){
        if(timer.closing || timer.expired || timer.deadline>runtime_.clock_ticks)continue;
        if(timer.multimedia && (timer.flags&0x30U)){
            if(const auto event=runtime_.wait_objects.find(timer.callback);event!=runtime_.wait_objects.end()){
                event->second.set_signal(true);
                if(timer.flags&0x20U)multimedia_pulses_.insert(timer.callback);
            }
            if(timer.period)timer.deadline+=(1+(runtime_.clock_ticks-timer.deadline)/(std::uint64_t{timer.period}*10000))*std::uint64_t{timer.period}*10000;
            else {timer.expired=true;timer.closing=true;}
            continue;
        }
        if(registered_waits_.size()>=64){timer_workers_exhausted_=true;break;}
        const auto worker=acquire_wait_worker(threads_.items.at(threads_.current).context,memory,std::max(UINT64_C(65536),runtime_.image.stack_size));
        if(!worker){timer_workers_exhausted_=true;break;}
        RegisteredWait work;work.callback=timer.callback;work.parameter=timer.parameter;work.worker=worker;
        work.timer_owner=handle;work.armed=false;work.ready=true;work.closing=true;work.timed_out=true;
        if(timer.multimedia)work.callback_batch={{timer.callback,{handle,0,timer.parameter,0,0}}};
        const auto registration=next_registered_wait_++;registered_waits_.emplace(registration,work);wait_workers_[worker]=registration;
        auto& thread=threads_.items.at(worker);thread.status=ThreadStatus::Runnable;thread.wait={};
        ++timer.outstanding;
        if(timer.period)timer.deadline+=std::uint64_t{timer.period}*10000;
        else {timer.expired=true;if(timer.multimedia)timer.closing=true;}
    }
    collect_timer_queues();
    last_error_=saved_error;
}
std::optional<std::uint32_t> WindowsServices::invoke_timer_queue(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t result,std::uint32_t error=0){if(error)last_error_=error;arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto u32=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    if(name=="timeSetEvent"){
        const auto flags=u32(4),mode=flags&0x30U;
        if(!u32(0) || u32(0)>65535 || u32(1)>65535 || !arg(2) || (flags&~0x131U) || mode==0x30U || queue_timers_.size()>=4096)return finish(0);
        if(mode){
            const auto event=runtime_.wait_objects.find(arg(2));
            if(event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event || !(event->second.access&2U))return finish(0);
        }else memory.check(GuestAddress{arg(2)},1,Access::Execute);
        QueueTimer timer;timer.multimedia=true;timer.callback=arg(2);timer.parameter=arg(3);timer.flags=flags;
        timer.period=(flags&1U)?u32(0):0;timer.deadline=runtime_.clock_ticks+std::uint64_t{u32(0)}*10000;
        const auto handle=next_timer_queue_handle_++;queue_timers_.emplace(handle,timer);return finish(handle);
    }
    if(name=="timeKillEvent"){
        const auto handle=u32(0);auto& caller=threads_.items.at(threads_.current);
        const bool resumed=caller.wait.kind==ThreadWaitKind::TimerQueueCompletion && caller.wait.handles.at(0)==handle;
        auto found=queue_timers_.find(handle);
        if(found==queue_timers_.end()){if(resumed)caller.wait={};return finish(resumed?0:11);}
        if(!found->second.multimedia)return finish(11);
        const bool blocking=(found->second.flags&0x100U)!=0;
        if(blocking){
            const auto worker=wait_workers_.find(threads_.current);
            if(worker!=wait_workers_.end() && worker->second && registered_waits_.at(worker->second).timer_owner==handle)return finish(11);
        }
        found->second.closing=true;
        // 尚未開始的回呼直接撤銷；正在執行的回呼依同步旗標等待結束。
        std::vector<std::uint64_t> cancel;
        for(const auto& [id,work]:registered_waits_)if(work.timer_owner==handle && !work.active)cancel.push_back(id);
        for(const auto id:cancel)release_registered_wait(id);
        collect_timer_queues();
        if(!blocking || !queue_timers_.contains(handle)){if(resumed)caller.wait={};return finish(0);}
        caller.status=ThreadStatus::Waiting;caller.wait={};caller.wait.kind=ThreadWaitKind::TimerQueueCompletion;
        caller.wait.handles={handle,0};schedule(state,memory);return std::nullopt;
    }
    if(name=="CreateTimerQueue"){
        if(timer_queues_.size()>=4096)return finish(0,8);
        const auto handle=next_timer_queue_handle_++;timer_queues_.emplace(handle,TimerQueue{});return finish(handle);
    }
    const bool create=name=="CreateTimerQueueTimer",change=name=="ChangeTimerQueueTimer",single=name=="DeleteTimerQueueTimer";
    const auto queue=arg(create?1:0);
    const auto found_queue=timer_queues_.find(queue);
    if(create || change){
        if(queue && (found_queue==timer_queues_.end() || found_queue->second.closing))return finish(0,6);
        if(create){
            if(!arg(0) || !arg(2) || ((u32(6)&8U) && u32(5)))return finish(0,87);
            // Vista 之後 I/O-thread 位元被忽略；timer-thread APC、權杖移交及調整池上限尚未提供。
            if(u32(6)&~0x99U)return finish(0,50);
            memory.check(GuestAddress{arg(0)},x86_?4:8,Access::Write);memory.check(GuestAddress{arg(2)},1,Access::Execute);
            if(queue_timers_.size()>=4096)return finish(0,8);
            QueueTimer timer;timer.queue=queue;timer.callback=arg(2);timer.parameter=arg(3);timer.period=u32(5);timer.flags=u32(6);
            timer.deadline=runtime_.clock_ticks+std::uint64_t{u32(4)}*10000;
            const auto handle=next_timer_queue_handle_++;queue_timers_.emplace(handle,timer);memory.store(GuestAddress{arg(0)},x86_?32:64,handle);return finish(1);
        }
        const auto found=queue_timers_.find(arg(1));
        if(found==queue_timers_.end() || found->second.queue!=queue || found->second.closing)return finish(0,6);
        auto& timer=found->second;if((timer.flags&8U) && u32(3))return finish(0,87);
        // Microsoft 契約：已觸發的單次 timer 不會由 Change 重新啟用。
        if(!timer.expired){timer.deadline=runtime_.clock_ticks+std::uint64_t{u32(2)}*10000;timer.period=u32(3);}
        return finish(1);
    }
    const auto handle=single?arg(1):queue;
    auto& caller=threads_.items.at(threads_.current);
    if(caller.wait.kind==ThreadWaitKind::TimerQueueCompletion){
        const bool pending=single?queue_timers_.contains(handle):timer_queues_.contains(handle);
        if(!pending){caller.wait={};return finish(1);}
    }
    if((queue && (found_queue==timer_queues_.end() || found_queue->second.closing)) || (!single && !queue))return finish(0,6);
    const auto found_timer=queue_timers_.find(handle);
    if(single && (found_timer==queue_timers_.end() || found_timer->second.queue!=queue || found_timer->second.closing))return finish(0,6);
    const auto completion=name=="DeleteTimerQueue"?0:arg(single?2:1);
    const bool blocking=completion==(x86_?UINT32_MAX:UINT64_MAX);
    if(completion && !blocking){
        const auto event=runtime_.wait_objects.find(completion);
        if(event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event)return finish(0,6);
        if(!(event->second.access&2U))return finish(0,5);
    }
    if(blocking){
        const auto worker=wait_workers_.find(threads_.current);
        if(worker!=wait_workers_.end() && worker->second){
            const auto owner=registered_waits_.at(worker->second).timer_owner;
            if(owner && (!single || owner==handle))return finish(0,1131); // 同步刪除自己的回呼會死結。
        }
    }
    if(single){found_timer->second.closing=true;found_timer->second.completion_event=blocking?0:completion;}
    else {
        found_queue->second.closing=true;found_queue->second.completion_event=blocking?0:completion;
        for(auto& [id,timer]:queue_timers_){(void)id;if(timer.queue==queue)timer.closing=true;}
    }
    collect_timer_queues();
    const bool pending=single?queue_timers_.contains(handle):timer_queues_.contains(handle);
    if(!pending)return finish(1);
    if(blocking){
        caller.status=ThreadStatus::Waiting;caller.wait={};caller.wait.kind=ThreadWaitKind::TimerQueueCompletion;
        caller.wait.handles={handle,single?0U:1U};schedule(state,memory);return std::nullopt;
    }
    return finish(0,997);
}
}
