// Media Foundation 工作佇列（由 windows_media_foundation.cpp 的匯出鏈引入）。
// 佇列識別碼與工作索引都是 DWORD：輸出只寫 4 位元組。
// - MFAllocateWorkQueue／Ex／MFAllocateSerialWorkQueue：使用者佇列，識別碼帶高位元；標準與視窗型佇列一次只執行一個工作（依提交順序），
//   多執行緒型與 MFASYNC_CALLBACK_QUEUE_* 標準佇列可並行。MFLockWorkQueue／MFUnlockWorkQueue 計算參照，歸零後佇列失效。
// - MFLockSharedWorkQueue：依 MMCSS 類別名稱共用一個多執行緒佇列；空字串為標準佇列。MFLockPlatform／MFUnlockPlatform 計算平台參照。
// - MFScheduleWorkItem／Ex、MFPutWaitingWorkItem、MFAddPeriodicCallback：沿用 kernel32 的計時器佇列與 RegisterWaitForSingleObject，
//   到期或事件觸發時由 internal-mf!TimedFire 把非同步結果放入工作佇列；週期回呼每 MFGetTimerPeriodicity（10 ms）在計時器執行緒呼叫。
//   MFCancelWorkItem 撤銷尚未觸發的排程或等待工作並釋放結果。
// - MMCSS：MFBeginRegisterWorkQueueWithMMCSS／Ex、MFBeginUnregisterWorkQueueWithMMCSS 與 MFRegisterPlatformWithMMCSS 依 avrt 相同的工作設定
//   （SystemProfile\Tasks 的優先順序與排程類別、共用工作索引）建立 MMCSS 工作；之後派送到該佇列的工作在工作執行緒上套用（MmcssEnter），
//   Invoke 結束後還原（MmcssLeave），工作執行緒由多個佇列共用。登錄結果經非同步結果送回，End 函式取回狀態與工作索引。
//   MFLockSharedWorkQueue 帶類別名稱時同樣是 MMCSS 佇列。標準佇列（識別碼 0～8）沿用 MFRegisterPlatformWithMMCSS 的設定。
if(name=="MFAllocateWorkQueue" || name=="MFAllocateWorkQueueEx" || name=="MFAllocateSerialWorkQueue" || name=="MFLockWorkQueue" || name=="MFUnlockWorkQueue" ||
   name=="MFLockSharedWorkQueue" || name=="MFLockPlatform" || name=="MFUnlockPlatform" || name=="MFGetTimerPeriodicity" || name=="MFScheduleWorkItem" ||
   name=="MFScheduleWorkItemEx" || name=="MFCancelWorkItem" || name=="MFPutWaitingWorkItem" || name=="MFAddPeriodicCallback" || name=="MFRemovePeriodicCallback" ||
   name=="TimedFire" || name=="PeriodicFire" || name=="QueueDone" || name=="MmcssEnter" || name=="MmcssLeave" ||
   name=="MFBeginRegisterWorkQueueWithMMCSS" || name=="MFBeginRegisterWorkQueueWithMMCSSEx" || name=="MFEndRegisterWorkQueueWithMMCSS" ||
   name=="MFBeginUnregisterWorkQueueWithMMCSS" || name=="MFEndUnregisterWorkQueueWithMMCSS" || name=="MFGetWorkQueueMMCSSClass" ||
   name=="MFGetWorkQueueMMCSSTaskId" || name=="MFGetWorkQueueMMCSSPriority" || name=="MFRegisterPlatformWithMMCSS" || name=="MFUnregisterPlatformFromMMCSS" ||
   name=="MFJoinWorkQueue" || name=="MFUnjoinWorkQueue"){
    constexpr std::uint32_t invalid_queue=0xc00d36ffU,not_found=0xc00d36d5U;
    const auto allocate_queue=[&](std::uint32_t type,std::uint32_t target)->std::uint32_t{
        require(bridge.queues.size()<4096,0x8007000eU);const auto id=bridge.next_queue++;require((id&0xffff0000U)!=0,0x8007000eU);
        MediaFoundationState::WorkQueue queue;queue.type=type;queue.target=target;bridge.queues.emplace(id,queue);return id;};
    const auto standard=[](std::uint32_t queue){return queue && queue<=8;};
    // 計時器佇列與等待登錄都需要客體位址的輸出 handle。
    const auto host_timer=[&](std::uint64_t key,std::uint32_t due,std::uint32_t period,bool periodic)->std::uint64_t{
        GuestScratch scratch(*this,memory,state);const auto slot=scratch.allocate(8);
        const auto routine=resolve("internal-mf",periodic?"PeriodicFire":"TimedFire").value;
        if(!(nested_service(state,memory,"kernel32.dll","CreateTimerQueueTimer",{slot,0,routine,key,due,period,periodic?0U:8U})&UINT32_MAX))throw Failure{0x8007000eU};
        return memory.load(GuestAddress{slot},bits);};
    // MMCSS：依類別名稱建立工作設定（失敗時回傳 HRESULT_FROM_WIN32）；relative 是相對優先順序（-2～2）。
    const auto register_mmcss=[&](MediaFoundationState::WorkQueue& target,std::u16string_view text,std::uint32_t requested,std::int32_t relative)->std::uint32_t{
        require(relative>=-2 && relative<=2);MultimediaThreadTask task;std::uint32_t index=0;
        if(const auto error=multimedia_task(text,requested,task,index))return 0x80070000U|error;
        task.relative=relative;target.mmcss=std::u16string(text);target.task=index;target.mmcss_task=task;return 0;};
    const auto queue_of=[&](std::uint32_t id)->MediaFoundationState::WorkQueue&{
        if(standard(id) || !id)return bridge.platform;const auto found=bridge.queues.find(id);require(found!=bridge.queues.end(),invalid_queue);return found->second;};
    // 非同步完成：結果送到指定佇列，由 poll_media_foundation 派送（status／identifier 由 End 函式讀取）。
    const auto complete_async=[&](std::uint32_t queue,std::uint64_t callback,std::uint64_t context,std::uint32_t status,std::uint32_t index){
        require(callback!=0,pointer_error);(void)foreign_method(callback,4);
        const auto pointer=make(mf::Async);auto completion=object(pointer,mf::Async);hold(callback);hold(context);completion->callback=callback;completion->state=context;
        completion->status=status;completion->identifier=index;bridge.work_items.push_back({pointer,foreign_method(callback,4),callback,queue});};
    const auto retire=[&](const MediaFoundationState::Timed& timed){
        if(timed.waiting)(void)nested_service(state,memory,"kernel32.dll","UnregisterWait",{timed.handle});
        else (void)nested_service(state,memory,"kernel32.dll","DeleteTimerQueueTimer",{0,timed.handle,0});};
    if(name=="MFAllocateWorkQueue"){put(arg(0),0);put(arg(0),allocate_queue(0,0));}
    else if(name=="MFAllocateWorkQueueEx"){put(arg(1),0);require(d(0)<=2);put(arg(1),allocate_queue(d(0),0));}
    else if(name=="MFAllocateSerialWorkQueue"){
        put(arg(1),0);require(standard(d(0)) || bridge.queues.contains(d(0)),invalid_queue);put(arg(1),allocate_queue(0,d(0)));
    }else if(name=="MFLockWorkQueue" || name=="MFUnlockWorkQueue"){
        if(!standard(d(0))){
            const auto found=bridge.queues.find(d(0));require(found!=bridge.queues.end(),invalid_queue);
            if(name=="MFLockWorkQueue"){require(found->second.locks<UINT32_MAX,0x8007000eU);++found->second.locks;}
            else if(!--found->second.locks)bridge.queues.erase(found);
        }
    }else if(name=="MFLockSharedWorkQueue"){
        // (wszClass, BasePriority, pdwTaskId, pID)
        require(arg(0)!=0 && arg(3)!=0,pointer_error);put(arg(3),0);const auto text=read_utf16(memory,GuestAddress{arg(0)},64);
        std::uint32_t id=1;
        if(!text.empty()){
            // 類別名稱即 MMCSS 工作；同一類別共用一個多執行緒佇列，pdwTaskId 輸入非零時必須是該類別的工作索引。
            auto folded=text;for(auto& c:folded)c=unicode_case_unit(c,false);
            const auto requested=arg(2)?static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32)):0U;
            if(const auto found=bridge.shared_queues.find(folded);found!=bridge.shared_queues.end() && bridge.queues.contains(found->second)){
                id=found->second;auto& queue=bridge.queues.at(id);require(!requested || requested==queue.task,0x8007060fU);require(queue.locks<UINT32_MAX,0x8007000eU);++queue.locks;
            }else {
                MediaFoundationState::WorkQueue probe;const auto status=register_mmcss(probe,text,requested,static_cast<std::int32_t>(d(1))<-2?-2:static_cast<std::int32_t>(d(1))>2?2:static_cast<std::int32_t>(d(1)));
                require(!status,status);id=allocate_queue(2,0);auto& queue=bridge.queues.at(id);queue.mmcss=probe.mmcss;queue.task=probe.task;queue.mmcss_task=probe.mmcss_task;bridge.shared_queues[folded]=id;
            }
            if(arg(2))memory.store(GuestAddress{arg(2)},32,bridge.queues.at(id).task);
        }else if(arg(2))memory.store(GuestAddress{arg(2)},32,0);
        put(arg(3),id);
    }else if(name=="MFLockPlatform"){require(bridge.platform_locks<UINT32_MAX,0x8007000eU);++bridge.platform_locks;}
    else if(name=="MFUnlockPlatform"){if(bridge.platform_locks)--bridge.platform_locks;}
    else if(name=="MFGetTimerPeriodicity"){require(arg(0)!=0,pointer_error);put(arg(0),10);}
    else if(name=="MFScheduleWorkItem" || name=="MFScheduleWorkItemEx" || name=="MFPutWaitingWorkItem"){
        // MFScheduleWorkItem(pCallback, pState, Timeout, pKey)／MFScheduleWorkItemEx(pResult, Timeout, pKey)／MFPutWaitingWorkItem(hEvent, Priority, pResult, pKey)；
        // 逾時以毫秒計，應為負值（-Timeout 毫秒後執行），0 立即執行。
        const bool waiting=name=="MFPutWaitingWorkItem",plain=name=="MFScheduleWorkItem";
        const auto key_pointer=arg(plain?3:waiting?3:2);if(key_pointer)put(key_pointer,0,64);
        std::uint64_t result_pointer=0;
        if(plain){
            require(arg(0)!=0,pointer_error);(void)foreign_method(arg(0),4);
            result_pointer=make(mf::Async);auto completion=object(result_pointer,mf::Async);hold(arg(0));hold(arg(1));completion->callback=arg(0);completion->state=arg(1);
        }else {const auto pointer=arg(waiting?2:0);auto completion=object(pointer,mf::Async);require(completion->callback!=0);hold(pointer);result_pointer=pointer;}
        const auto key=bridge.next_key++;MediaFoundationState::Timed timed;timed.result=result_pointer;timed.waiting=waiting;
        try{
            if(waiting){
                require(arg(0)!=0 && runtime_.wait_objects.contains(arg(0)&~UINT64_C(1)),0x80070006U);
                GuestScratch scratch(*this,memory,state);const auto slot=scratch.allocate(8);
                if(!(nested_service(state,memory,"kernel32.dll","RegisterWaitForSingleObject",{slot,arg(0),resolve("internal-mf","TimedFire").value,key,UINT32_MAX,8})&UINT32_MAX))throw Failure{0x80070006U};
                timed.handle=memory.load(GuestAddress{slot},bits);
            }else {
                const auto timeout=static_cast<std::int64_t>(arg(plain?2:1));const auto milliseconds=timeout<0?-timeout:timeout;
                require(milliseconds<=UINT32_MAX-1);timed.handle=host_timer(key,static_cast<std::uint32_t>(milliseconds),0,false);
            }
        }catch(...){release(result_pointer);throw;}
        bridge.timed.emplace(key,timed);if(key_pointer)put(key_pointer,key,64);
    }else if(name=="MFCancelWorkItem"){
        const auto found=bridge.timed.find(arg(0));require(found!=bridge.timed.end() && !found->second.periodic,not_found);
        const auto timed=found->second;bridge.timed.erase(found);retire(timed);release(timed.result);
    }else if(name=="MFAddPeriodicCallback"){
        // (Callback, pContext, pdwKey)：回呼在計時器執行緒以 (pContext) 呼叫。
        require(arg(0)!=0,pointer_error);memory.check(GuestAddress{arg(0)},1,Access::Execute);if(arg(2))put(arg(2),0);
        const auto key=bridge.next_key++;require(key<=UINT32_MAX,0x8007000eU);MediaFoundationState::Timed timed;timed.periodic=true;timed.callback=arg(0);timed.context=arg(1);
        timed.handle=host_timer(key,10,10,true);hold(arg(1));bridge.timed.emplace(key,timed);if(arg(2))put(arg(2),static_cast<std::uint32_t>(key));
    }else if(name=="MFRemovePeriodicCallback"){
        const auto found=bridge.timed.find(d(0));require(found!=bridge.timed.end() && found->second.periodic,not_found);
        const auto timed=found->second;bridge.timed.erase(found);retire(timed);release(timed.context);
    }else if(name=="TimedFire"){
        // 計時器或等待觸發：結果放入標準佇列，由 poll_media_foundation 派送；已撤銷的工作不做事。
        if(const auto found=bridge.timed.find(arg(0));found!=bridge.timed.end() && !found->second.periodic){
            const auto timed=found->second;bridge.timed.erase(found);retire(timed);
            const auto callback=object(timed.result,mf::Async)->callback;bridge.work_items.push_back({timed.result,foreign_method(callback,4),callback,0});
        }
    }else if(name=="PeriodicFire"){
        if(const auto found=bridge.timed.find(arg(0));found!=bridge.timed.end() && found->second.periodic)callbacks.push_back({found->second.callback,{found->second.context}});
    }else if(name=="MFBeginRegisterWorkQueueWithMMCSS" || name=="MFBeginRegisterWorkQueueWithMMCSSEx"){
        // (dwWorkQueueId, wszClass, dwTaskId, [lPriority,] pDoneCallback, pDoneState)：只接受使用者佇列；標準佇列由 MFRegisterPlatformWithMMCSS 處理。
        const bool extended=name.ends_with("Ex");require(arg(1)!=0 && arg(extended?4:3)!=0,pointer_error);
        require(!standard(d(0)) && d(0) && bridge.queues.contains(d(0)),invalid_queue);
        const auto text=read_utf16(memory,GuestAddress{arg(1)},64);const auto relative=extended?static_cast<std::int32_t>(d(3)):0;require(relative>=-2 && relative<=2);
        auto& queue=bridge.queues.at(d(0));MediaFoundationState::WorkQueue probe;const auto status=register_mmcss(probe,text,d(2),relative);
        if(!status){queue.mmcss=probe.mmcss;queue.task=probe.task;queue.mmcss_task=probe.mmcss_task;}
        complete_async(d(0),arg(extended?4:3),arg(extended?5:4),status,status?0:probe.task);
    }else if(name=="MFBeginUnregisterWorkQueueWithMMCSS"){
        require(arg(1)!=0,pointer_error);require(!standard(d(0)) && d(0) && bridge.queues.contains(d(0)),invalid_queue);
        auto& queue=bridge.queues.at(d(0));queue.mmcss.clear();queue.task=0;queue.mmcss_task={};complete_async(d(0),arg(1),arg(2),0,0);
    }else if(name=="MFEndRegisterWorkQueueWithMMCSS" || name=="MFEndUnregisterWorkQueueWithMMCSS"){
        auto completion=object(arg(0),mf::Async);
        if(name=="MFEndRegisterWorkQueueWithMMCSS"){require(arg(1)!=0,pointer_error);put(arg(1),completion->status?0U:completion->identifier);}
        require(!completion->status,completion->status);
    }else if(name=="MFGetWorkQueueMMCSSClass"){
        // (dwWorkQueueId, pwszClass, pcchClass)：pcchClass 輸入緩衝字元數、輸出所需字元數（含結尾 NUL）；緩衝不足或為 NULL 回報 MF_E_BUFFERTOOSMALL。
        require(arg(2)!=0,pointer_error);const auto& queue=queue_of(d(0));const auto needed=static_cast<std::uint32_t>(queue.mmcss.size()+1);
        const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));put(arg(2),needed);
        require(arg(1)!=0 && capacity>=needed,0xc00d36b1U);auto bytes=utf16_bytes(queue.mmcss);memory.write(GuestAddress{arg(1)},bytes);
    }else if(name=="MFGetWorkQueueMMCSSTaskId"){require(arg(1)!=0,pointer_error);put(arg(1),queue_of(d(0)).task);}
    else if(name=="MFGetWorkQueueMMCSSPriority"){require(arg(1)!=0,pointer_error);put(arg(1),static_cast<std::uint32_t>(queue_of(d(0)).mmcss_task.relative));}
    else if(name=="MFRegisterPlatformWithMMCSS"){
        // (wszClass, pdwTaskId, lPriority)：pdwTaskId 輸入既有工作索引或 0，輸出實際工作索引。
        require(arg(0)!=0 && arg(1)!=0,pointer_error);const auto text=read_utf16(memory,GuestAddress{arg(0)},64);
        MediaFoundationState::WorkQueue probe;const auto status=register_mmcss(probe,text,static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32)),static_cast<std::int32_t>(d(2)));
        require(!status,status);bridge.platform.mmcss=probe.mmcss;bridge.platform.task=probe.task;bridge.platform.mmcss_task=probe.mmcss_task;put(arg(1),probe.task);
    }else if(name=="MFUnregisterPlatformFromMMCSS"){bridge.platform.mmcss.clear();bridge.platform.task=0;bridge.platform.mmcss_task={};}
    else if(name=="MmcssEnter"){
        // 工作執行緒已有 MMCSS 關聯（客體自己呼叫 AvSetMmThreadCharacteristics）時不覆蓋。
        const auto thread=threads_.current;auto& current=threads_.items.at(thread).multimedia;
        const auto queue=d(0);const auto* owner=standard(queue) || !queue?&bridge.platform:bridge.queues.contains(queue)?&bridge.queues.at(queue):nullptr;
        if(owner && owner->mmcss_task.handle && !current.handle){current=owner->mmcss_task;bridge.mmcss_threads[thread]=current.handle;threads_.yield=true;}
    }else if(name=="MmcssLeave"){
        const auto thread=threads_.current;
        if(const auto found=bridge.mmcss_threads.find(thread);found!=bridge.mmcss_threads.end()){
            auto& current=threads_.items.at(thread).multimedia;if(current.handle==found->second){current={};threads_.yield=true;}bridge.mmcss_threads.erase(found);
        }
    }else if(name=="MFJoinWorkQueue"){
        // (dwQueue, hFile, phCookie)：檔案須以 FILE_FLAG_OVERLAPPED 開啟；已繫結其他完成埠時回報 HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER)。
        require(arg(2)!=0,pointer_error);put(arg(2),0,bits);require(standard(d(0)) || bridge.queues.contains(d(0)),invalid_queue);require(bridge.joins.size()<65536,0x8007000eU);
        if(!bridge.io_port){bridge.io_port=runtime_.allocate_wait_handle();runtime_.completion_ports.emplace(bridge.io_port,CompletionPort{});}
        // 同一檔案只能加入一次（完成埠繫結唯一）；解除後重新加入時改寫同一繫結的完成鍵。
        require(std::none_of(bridge.joins.begin(),bridge.joins.end(),[&](const auto& join){return join.second.file==arg(1);}),0x80070057U);
        const auto key=bridge.next_join++;
        if(const auto error=bind_file_completion_port(arg(1),bridge.io_port,key,bridge.bound_files.contains(arg(1))))throw Failure{0x80070000U|(error&0xffffU)};
        bridge.bound_files.insert(arg(1));bridge.joins.emplace(key,MediaFoundationState::Join{d(0),arg(1)});put(arg(2),key,bits);
    }else if(name=="MFUnjoinWorkQueue"){
        // (dwQueue, hCookie)：之後該檔案的完成封包不再派送（完成埠繫結無法撤銷）。
        const auto found=bridge.joins.find(arg(1));require(found!=bridge.joins.end() && found->second.queue==d(0));bridge.joins.erase(found);
    }else if(name=="QueueDone"){
        if(const auto found=bridge.queues.find(d(0));found!=bridge.queues.end())found->second.busy=false;
    }
}
