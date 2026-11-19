// ConcRT 結構化任務集合（structured_task_group、parallel_for／parallel_invoke 的底層）、取消信標與 Context 的靜態查詢；由 windows_nt_native.cpp 引入。
// _Schedule 把 chore 記入集合並交給既有執行緒池（每個 chore 一個工作項目）；工作執行緒與等待端都要先「認領」chore 才執行，
// _RunAndWait 先在呼叫端執行傳入的 chore，再依後進先出執行還沒被工作執行緒認領的 chore（與 ConcRT 等待端回收本地工作相同，避免執行緒池滿載時互等），
// 最後等待正在其他執行緒執行的 chore。chore 的工作函式是 _Chore::m_pFunction（vtable 之後），呼叫時傳入 chore 本身；
// _UnrealizedChore::_M_pTaskCollection 在 m_pFunction 之後，排程時寫入、集合完成後清除。
// 取消：_Cancel 之後尚未開始的 chore 不再執行，執行中的 chore 以 IsCurrentTaskCollectionCanceling／信標得知；巢狀集合繼承父集合的取消。
// 例外：chore 擲出的 C++ 例外不會轉到等待端重新擲出（Windows 會），會成為工作執行緒上的未處理例外。
if(service.family=="internal-concrt-scheduler" && name=="ChoreThunk"){
    return nested_continuation(state,memory,"ConcRT chore",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall frame(s,m,service);const auto chore=frame(0);const unsigned width=x86_?4U:8U;const auto size=static_cast<std::uint8_t>(width*8);
        const auto collection=m.load(GuestAddress{chore+width*2},size);auto found=concrt_collections_.find(collection);
        const auto pending=found==concrt_collections_.end()?std::vector<std::uint64_t>::iterator{}:std::find(found->second.pending.begin(),found->second.pending.end(),chore);
        if(found==concrt_collections_.end() || pending==found->second.pending.end()){frame.return_to(s,0);co_return std::optional<std::uint32_t>{};} // 等待端已認領或集合已結束
        found->second.pending.erase(pending);++found->second.running;const bool skip=concrt_canceling(collection);const auto thread=threads_.current;
        if(!skip){concrt_current_[thread].push_back(collection);(void)co_await service_callback::call(m.load(GuestAddress{chore+width},size),{chore});
            auto& stack=concrt_current_[thread];if(!stack.empty())stack.pop_back();if(stack.empty())concrt_current_.erase(thread);}
        found=concrt_collections_.find(collection);
        if(found!=concrt_collections_.end() && !--found->second.running && found->second.event)(void)co_await service_callback::call(resolve("kernel32.dll","SetEvent").value,{found->second.event});
        frame.return_to(s,0);co_return std::optional<std::uint32_t>{};
    });
}
if(service.family=="concrt140.dll" && (name.find("_StructuredTaskCollection@details@Concurrency@@")!=name.npos || name.find("_UnrealizedChore@details@Concurrency@@")!=name.npos)){
    // 建構／解構子的名稱以 ??0_／??1_ 開頭（類別名稱前沒有 @），以類別限定名稱比對。x86 的成員是 thiscall（this 在 ECX），只有 _RunAndWait 是 __stdcall（this 是第一個堆疊參數）。
    const bool stdcall=name.starts_with("?_RunAndWait@");
    const auto self=x86_ && !stdcall?state.reg(Gpr::Rcx)&UINT32_MAX:call(0);
    const auto argument=[&](unsigned index){return call(index+(x86_ && !stdcall?0U:1U));};
    const bool constructor=name.starts_with("??0"),destructor=name.starts_with("??1");
    if(name.starts_with("?_CheckTaskCollection@")){
        // task_handle 在集合等待前被銷毀：移出待執行清單，之後不會再以已釋放的 chore 執行。
        const auto collection=memory.load(GuestAddress{self+unit*2},bits);
        if(const auto found=concrt_collections_.find(collection);found!=concrt_collections_.end())std::erase(found->second.pending,self);
        return finish(0);
    }
    if(constructor){
        // 帶取消權杖的建構子；_TaskCollectionBase 的欄位（父集合、擁有者 Context、權杖）由標頭中的內嵌建構子負責，這裡只登記集合。
        memory.check(GuestAddress{self},unit*4,Access::Write);ConcrtCollection collection;collection.token=argument(0);collection.registered=true;
        if(const auto current=concrt_current_.find(threads_.current);current!=concrt_current_.end() && !current->second.empty())collection.parent=current->second.back();
        concrt_collections_[self]=std::move(collection);return finish(self);
    }
    const auto ensure=[&]()->ConcrtCollection&{
        auto& collection=concrt_collections_[self];
        if(!collection.registered){collection.registered=true;if(const auto current=concrt_current_.find(threads_.current);current!=concrt_current_.end() && !current->second.empty() && current->second.back()!=self)collection.parent=current->second.back();}
        return collection;
    };
    if(name.starts_with("?_Schedule@")){
        concrt_attached_.insert(threads_.current);const auto chore=argument(0);if(!chore)return finish(0);
        memory.check(GuestAddress{chore},unit*4,Access::Write);auto& collection=ensure();
        memory.store(GuestAddress{chore+unit*2},bits,self);collection.scheduled.push_back(chore);
        if(concrt_canceling(self))return finish(0); // 已取消的集合不再開始新的 chore
        collection.pending.push_back(chore);
        (void)queue_pool_callback(state,memory,{resolve("internal-concrt-scheduler","ChoreThunk").value,{chore}}); // 排不進執行緒池時仍留在待執行清單，由等待端執行
        return finish(0);
    }
    if(name.starts_with("?_Cancel@")){
        ensure().canceled=true;
        // 取消信標：這個集合與其子集合登記的信標設為已觸發。
        for(const auto& [beacon,owner]:concrt_beacons_)if(concrt_descends(owner,self))memory.store(GuestAddress{beacon},32,1);
        return finish(0);
    }
    if(name.starts_with("?_IsCanceling@"))return finish(concrt_canceling(self)?1:0);
    // _RunAndWait 與解構子：在呼叫端執行傳入與未認領的 chore，再等待執行中的 chore；解構子在沒有等待過時先取消再等待（Windows 之後擲出 missing_wait）。
    const auto first=stdcall?call(1):0;
    return nested_continuation(state,memory,"ConcRT 結構化等待",[this,service,self,first,destructor](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall frame(s,m,service);const unsigned width=x86_?4U:8U;const auto size=static_cast<std::uint8_t>(width*8);const auto thread=threads_.current;
        concrt_attached_.insert(thread);
        {auto& collection=concrt_collections_[self];if(!collection.registered){collection.registered=true;if(const auto current=concrt_current_.find(thread);current!=concrt_current_.end() && !current->second.empty() && current->second.back()!=self)collection.parent=current->second.back();}
         if(destructor && (!collection.pending.empty() || collection.running))collection.canceled=true;}
        const auto run=[&](std::uint64_t chore)->service_callback::Task<std::uint32_t>{
            concrt_current_[thread].push_back(self);(void)co_await service_callback::call(m.load(GuestAddress{chore+width},size),{chore});
            auto& stack=concrt_current_[thread];if(!stack.empty())stack.pop_back();if(stack.empty())concrt_current_.erase(thread);co_return 0;
        };
        if(first){m.store(GuestAddress{first+width*2},size,self);concrt_collections_[self].scheduled.push_back(first);if(!concrt_canceling(self))(void)co_await run(first);}
        for(;;){
            auto found=concrt_collections_.find(self);if(found==concrt_collections_.end())break;auto& collection=found->second;
            if(!collection.pending.empty() && !concrt_canceling(self)){const auto chore=collection.pending.back();collection.pending.pop_back();++collection.running;(void)co_await run(chore);
                if(const auto again=concrt_collections_.find(self);again!=concrt_collections_.end())--again->second.running;continue;}
            collection.pending.clear();if(!collection.running)break;
            if(!collection.event){const auto event=co_await service_callback::call_value(resolve("kernel32.dll","CreateEventW").value,{0,1,0,0});
                if(!event){(void)co_await service_callback::call(resolve("kernel32.dll","Sleep").value,{1});continue;}concrt_collections_[self].event=x86_?event&UINT32_MAX:event;continue;}
            const auto event=collection.event;(void)co_await service_callback::call(resolve("kernel32.dll","ResetEvent").value,{event});
            if(const auto again=concrt_collections_.find(self);again==concrt_collections_.end() || !again->second.running)break;
            (void)co_await service_callback::call(resolve("kernel32.dll","WaitForSingleObject").value,{event,UINT32_MAX});
        }
        std::uint32_t status=1; // _Completed
        if(auto found=concrt_collections_.find(self);found!=concrt_collections_.end()){
            if(concrt_canceling(self))status=2; // _Canceled
            for(const auto chore:found->second.scheduled)if(m.load(GuestAddress{chore+width*2},size)==self)m.store(GuestAddress{chore+width*2},size,0);
            found->second.scheduled.clear();found->second.canceled=false;
            if(destructor){if(found->second.event)(void)co_await service_callback::call(resolve("kernel32.dll","CloseHandle").value,{found->second.event});
                if(const auto again=concrt_collections_.find(self);again!=concrt_collections_.end())concrt_collections_.erase(again);}
        }
        frame.return_to(s,destructor?0:status);co_return std::optional<std::uint32_t>{};
    });
}
if(service.family=="concrt140.dll" && name.find("_Cancellation_beacon@details@Concurrency@@")!=name.npos){
    // _Cancellation_beacon 只有一個欄位：指向 _Beacon_reference（_M_signals）的指標；標頭內嵌的 _Is_signaled 直接讀它。
    // 建構時登記到目前執行中的集合，集合（或祖先）取消時設為 1；_Confirm_cancel 確認目前集合確實在取消，否則清回 0。
    const auto self=x86_?state.reg(Gpr::Rcx)&UINT32_MAX:call(0);
    if(name.starts_with("??0")){
        memory.check(GuestAddress{self},unit,Access::Write);const auto reference=allocate_heap(memory,8);if(!reference)return finish(0);memory.store(GuestAddress{reference},64,0);
        const auto current=concrt_current_.find(threads_.current);const auto owner=current!=concrt_current_.end() && !current->second.empty()?current->second.back():0;
        concrt_beacons_[reference]=owner;if(owner && concrt_canceling(owner))memory.store(GuestAddress{reference},32,1);
        memory.store(GuestAddress{self},bits,reference);return finish(self);
    }
    const auto reference=memory.load(GuestAddress{self},bits);
    if(name.starts_with("??1")){if(concrt_beacons_.erase(reference))(void)free_heap(memory,reference,state);memory.store(GuestAddress{self},bits,0);return finish(0);}
    const auto found=concrt_beacons_.find(reference);const bool canceling=found!=concrt_beacons_.end() && found->second && concrt_canceling(found->second);
    if(!canceling && reference)memory.store(GuestAddress{reference},32,0);
    return finish(canceling?1:0);
}
if(service.family=="concrt140.dll" && (name.starts_with("?Id@Context@") || name.starts_with("?VirtualProcessorId@Context@") || name.starts_with("?ScheduleGroupId@Context@") || name.starts_with("?Oversubscribe@Context@"))){
    // 目前 Context 的識別碼（第一次查詢時依序配發）、虛擬處理器（目前執行的處理器編號）與排程群組（預設群組 0）；執行緒沒有附加排程器、也不在執行 chore 時回 -1。
    // Oversubscribe 隱含附加；本執行緒池不以虛擬處理器數限制同時執行的工作，超額訂閱只記錄巢狀次數。
    const auto thread=threads_.current;
    if(name.starts_with("?Oversubscribe@")){concrt_attached_.insert(thread);auto& depth=concrt_oversubscribed_[thread];if(static_cast<std::uint8_t>(call(0)))++depth;else if(depth)--depth;if(!depth)concrt_oversubscribed_.erase(thread);return finish(0);}
    if(!concrt_attached_.contains(thread) && !concrt_current_.contains(thread))return finish(UINT32_MAX);
    if(name.starts_with("?Id@"))return finish(concrt_context_ids_.try_emplace(thread,static_cast<std::uint32_t>(concrt_context_ids_.size())).first->second);
    if(name.starts_with("?VirtualProcessorId@"))return finish(nested_service(state,memory,"kernel32.dll","GetCurrentProcessorNumber",{})&UINT32_MAX);
    return finish(0);
}
