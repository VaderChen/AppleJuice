// ConcRT 排程入口：ScheduleTask 把 TaskProc 與參數交給既有的 QueueUserWorkItem 執行緒池，內部轉接服務在工作執行緒上呼叫 TaskProc(data)；
// 第一次排程或查詢內部 _CurrentScheduler 時，呼叫端隱含附加預設排程器（每執行緒記錄）。公開的 GetNumberOfVirtualProcessors／Id 在沒有附加時回 -1。
// 取消查詢看目前執行緒正在執行 chore 的結構化集合（concrt_task_collection.inc）；追蹤函式在沒有啟用 ConcRT 追蹤時不輸出事件。
if(service.family=="internal-concrt-scheduler" && name=="TaskThunk"){
    return nested_continuation(state,memory,"ConcRT 工作",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall frame(s,m,service);const auto block=frame(0);const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto proc=m.load(GuestAddress{block},bits),data=m.load(GuestAddress{block+unit},bits);(void)free_heap(m,block,s);
        if(proc)(void)co_await service_callback::call(proc,{data});
        frame.return_to(s,0);co_return std::optional<std::uint32_t>{};
    });
}
if(service.family=="concrt140.dll" && (name.find("@CurrentScheduler@Concurrency@@")!=name.npos || name.find("@_CurrentScheduler@details@Concurrency@@")!=name.npos ||
   name.starts_with("?IsCurrentTaskCollectionCanceling@") || name.starts_with("?is_current_task_group_canceling@") || name.starts_with("?_Trace_"))){
    if(name.starts_with("?_Trace_"))return finish(0);
    if(name.starts_with("?IsCurrentTaskCollectionCanceling@") || name.starts_with("?is_current_task_group_canceling@")){
        const auto current=concrt_current_.find(threads_.current);return finish(current!=concrt_current_.end() && !current->second.empty() && concrt_canceling(current->second.back())?1:0);
    }
    const bool internal=name.find("@_CurrentScheduler@details@")!=name.npos;const auto thread=threads_.current;
    if(name.find("ScheduleTask@")!=name.npos){
        concrt_attached_.insert(thread);if(!call(0))return finish(0);
        const auto block=allocate_heap(memory,unit*2);if(!block)return finish(0);
        memory.store(GuestAddress{block},bits,call(0));memory.store(GuestAddress{block+unit},bits,call(1));
        if(!nested_service(state,memory,"kernel32.dll","QueueUserWorkItem",{resolve("internal-concrt-scheduler","TaskThunk").value,block,0}))(void)free_heap(memory,block,state);
        return finish(0); // finish 會還原呼叫前的 LastError。
    }
    if(internal)concrt_attached_.insert(thread);
    if(!concrt_attached_.contains(thread))return finish(UINT32_MAX);
    // 明確附加的排程器（CurrentScheduler::Create／Scheduler::Attach，concrt_objects.inc）優先，否則是預設排程器（識別碼 1、預設政策）。
    const auto stack=concrt_scheduler_stack_.find(thread);
    const auto attached=stack!=concrt_scheduler_stack_.end() && !stack->second.empty()?concrt_schedulers_.find(stack->second.back()):concrt_schedulers_.end();
    if(name.find("NumberOfVirtualProcessors@")!=name.npos){
        const auto limit=attached!=concrt_schedulers_.end()?attached->second.policy[1]:concrt_default_policy_?(*concrt_default_policy_)[1]:UINT32_MAX;
        return finish(limit!=UINT32_MAX?limit:nested_service(state,memory,"kernel32.dll","GetActiveProcessorCount",{0xffff})&UINT32_MAX);
    }
    return finish(attached!=concrt_schedulers_.end()?attached->second.id:1U);
}
