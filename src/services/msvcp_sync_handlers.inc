// Microsoft STL v14 的 ABI 轉接；鎖、等待與回呼都交由既有 Win32 實作。
if(service.family=="internal-msvcp" && name=="GlobalLocksInit"){
    if(call(1)!=runtime_.msvcp_global_locks)throw HostCallFault("STL 共用鎖初始化參數無效");
    std::vector<GuestCallback> callbacks;
    for(unsigned i=1;i<8;++i)callbacks.push_back({resolve("kernel32.dll","InitializeCriticalSection").value,{call(1)+unit+i*(x86_?24U:40U)}});
    start_callbacks(state,memory,std::move(callbacks),Continuation::Return,1);return std::nullopt;
}
if(name.find("_Lockit@std@@")!=name.npos){
    const bool object=name.find("_Lockit@std@@")!=name.npos,constructor=name.starts_with("??0");
    const auto address=object?(x86_?state.reg(Gpr::Rcx)&UINT32_MAX:call(0)):0;
    std::uint32_t kind=0;
    if(object){
        memory.check(GuestAddress{address},4,constructor?Access::Write:Access::Read);
        if(constructor){kind=name.find("@H@Z")!=name.npos?static_cast<std::uint32_t>(call(x86_?0:1)):0;memory.store(GuestAddress{address},32,kind);}
        else kind=static_cast<std::uint32_t>(memory.load(GuestAddress{address},32));
        if(kind&0x80000000U)throw HostCallFault("STL 鎖種類不可為負數");
        if(kind>=8)return finish(constructor?address:0);
    }
    op.arguments[0]=address;op.attributes=kind;
    // locale 鎖已有 CRT 共用模型，直接轉接，不重新初始化或改變既有狀態。
    if(!kind){op.information=1;return start("crt",constructor?"_lock_locales":"_unlock_locales",{});}
    if(!runtime_.msvcp_global_locks){
        const auto size=unit+8*(x86_?24U:40U);const auto storage=runtime_allocate(memory,{},size);
        memory.write(GuestAddress{storage},std::vector<std::uint8_t>(size));runtime_.msvcp_global_locks=storage;
    }
    return start("kernel32.dll","InitOnceExecuteOnce",{runtime_.msvcp_global_locks,resolve("internal-msvcp","GlobalLocksInit").value,runtime_.msvcp_global_locks,0});
}
if(service.family=="internal-msvcp" && name=="OnceComplete"){memory.store(GuestAddress{call(0)},32,2);return finish(0);}
if(service.family=="internal-msvcp" && name=="ThreadRunner"){
    // _Thrd_create 的 __stdcall 執行緒進入點：binder｛cdecl int(*)(void*), 引數｝讀出後即釋放，函式回傳值即執行緒結束碼。
    const auto binder=call(0);const auto function=memory.load(GuestAddress{binder},bits),argument=memory.load(GuestAddress{binder+unit},bits);
    (void)free_heap(memory,binder,state);
    start_callbacks(state,memory,{{function,{argument}}},Continuation::ReturnCallback);return std::nullopt;
}
if(service.family=="internal-msvcp" && name=="TaskSchedulerCallback"){
    // _Task_scheduler_callback(instance, _Threadpool_chore*, work)：呼叫 chore 的 _M_callback(_M_data)。
    const auto chore=call(1);const auto callback=memory.load(GuestAddress{chore+unit},bits),data=memory.load(GuestAddress{chore+2*unit},bits);
    start_callbacks(state,memory,{{callback,{data}}},Continuation::Return,0);return std::nullopt;
}
if(service.family=="msvcp140.dll" && name.find("@Concurrency@@")!=name.npos){
    // _Threadpool_chore｛_M_work, _M_callback, _M_data｝；_ContextCallback 只有一個指標欄位，1 表示延後擷取。
    const auto self=x86_ && !service.caller_cleanup?state.reg(Gpr::Rcx)&UINT32_MAX:call(0);
    if(name.starts_with("??0task_continuation_context@")){memory.store(GuestAddress{self},bits,1);memory.store(GuestAddress{self+unit},8,0);return finish(self);}
    if(name.starts_with("?GetCurrentThreadId@platform@"))return finish(thread_identifier(threads_.current));
    // 沒有 WinRT 例外與 causality tracer 時，未處理錯誤回報與工作事件記錄在 Windows 桌面版本身就不做事。
    if(name.starts_with("?ReportUnhandledError@") || name.find("@_TaskEventLogger@")!=name.npos)return finish(0);
    if(name.starts_with("?_ReportUnobservedException@")){start_callbacks(state,memory,{{resolve("crt","abort").value,{}}},Continuation::Return,0);return std::nullopt;}
    // 桌面 MTA 的物件內容不需要轉送；不擷取 IContextCallback，_CallInContext 直接在目前執行緒呼叫。
    if(name.starts_with("?_Capture@_ContextCallback@")){memory.store(GuestAddress{self},bits,0);return finish(0);}
    if(name.starts_with("?_Reset@_ContextCallback@")){
        const auto context=memory.load(GuestAddress{self},bits);if(!context || context==1)return finish(0);
        start_callbacks(state,memory,{{memory.load(GuestAddress{memory.load(GuestAddress{context},bits)+2*unit},bits),{context}}},Continuation::Return,0);return std::nullopt;
    }
    if(name.starts_with("?_CallInContext@_ContextCallback@")){
        // std::function<void()> 以值傳遞（x64 指向暫存複本，被呼叫端解構）：impl 位於第 8 個指標槽；_Do_call 為虛擬表 2、_Delete_this 為 4。
        const auto context=memory.load(GuestAddress{self},bits);
        if(context && context!=1)throw HostCallFault("PPL _CallInContext 不支援已擷取的 COM 內容");
        const auto function=call(1),impl=memory.load(GuestAddress{function+7*unit},bits);
        if(!impl)throw HostCallFault("PPL _CallInContext 收到空的 std::function");
        const auto table=memory.load(GuestAddress{impl},bits);
        start_callbacks(state,memory,{{memory.load(GuestAddress{table+2*unit},bits),{impl}},{memory.load(GuestAddress{table+4*unit},bits),{impl,impl!=function?1U:0U}}},Continuation::Return,0);
        return std::nullopt;
    }
    if(name.starts_with("?_Schedule_chore@")){
        memory.check(GuestAddress{call(0)},unit*3,Access::Write);
        return start("kernel32.dll","CreateThreadpoolWork",{resolve("internal-msvcp","TaskSchedulerCallback").value,call(0),0});
    }
    if(name.starts_with("?_Release_chore@")){
        const auto work=memory.load(GuestAddress{call(0)},bits);if(!work)return finish(0);
        memory.store(GuestAddress{call(0)},bits,0);return start("kernel32.dll","CloseThreadpoolWork",{work});
    }
}
if(service.family=="msvcp140.dll" || service.family=="msvcp110.dll" || service.family=="msvcp120.dll"){
    // x64：讓出／睡眠直接尾跳到 kernel32 的 SwitchToThread／Sleep（返回位址與影子空間沿用呼叫端），由該服務自行返回，
    // 不經客體回呼與回呼返回邊界（平行模式每次各多一次全停；電馭判客 Benchmark 每秒數百次）。x86 的 cdecl 與 stdcall
    // 堆疊清理不同，沿用回呼。
    const auto tail=[&](const char* target,std::uint64_t argument)->std::optional<std::uint32_t>{
        state.reg(Gpr::Rcx)=argument;state.rip=GuestAddress{resolve("kernel32.dll",target).value};return std::nullopt;};
    if(name=="_Thrd_yield")return x86_?start("kernel32.dll","SwitchToThread",{}):tail("SwitchToThread",0);
    if(name.starts_with("?_Execute_once@std@@")){
        // STL 的 ABI 包裝只把第四個 Context 設為 NULL。Win64 共用同一呼叫
        // 慣例與 32-byte 影子空間，可直接轉入既有 INIT_ONCE 客體常式及完整
        // provider；不為已完成的 once 額外建立／返回一層宿主 continuation。
        // Win32 的 cdecl／stdcall 清理量不同，仍沿用原回呼流程。
        static const bool tail_once=[] {const auto* value=std::getenv("X64ARM_AOT_MSVCP_ONCE_TAIL");
            return !value || std::string_view(value)!="0";}();
        if(!x86_ && tail_once){
            const auto target=resolve("kernel32.dll","InitOnceExecuteOnce");
            state.reg(Gpr::R9)=0;state.rip=target;return std::nullopt;
        }
        return start("kernel32.dll","InitOnceExecuteOnce",{call(0),call(1),call(2),0});
    }
    // 時鐘與身分沿用 QueryPerformanceCounter（10 MHz）、GetSystemTimePreciseAsFileTime 與 GetCurrentThreadId 的來源。
    if(name=="_Query_perf_frequency"){call.return_to(state,10000000,true);return std::nullopt;}
    if(name=="_Query_perf_counter"){call.return_to(state,runtime_.clock_ticks,true);return std::nullopt;}
    if(name=="_Xtime_get_ticks"){call.return_to(state,runtime_.system_filetime()-UINT64_C(116444736000000000),true);return std::nullopt;}
    if(name=="_Thrd_id")return finish(thread_identifier(threads_.current));
    if(name=="_Thrd_hardware_concurrency")return finish(runtime_.processor_count);
    const auto milliseconds_until=[&](std::uint64_t target)->std::uint32_t{return msvcp_wait_milliseconds(memory,target);};
    if(name=="_Thrd_sleep"){
        if(!x86_)return tail("Sleep",milliseconds_until(call(0)));
        start_callbacks(state,memory,{{resolve("kernel32.dll","Sleep").value,{milliseconds_until(call(0))}}},Continuation::Return,0);return std::nullopt;
    }
    // xtime｛__int64 秒, long 奈秒｝：_Xtime_diff_to_millis 為現在到 xt 的毫秒，_Xtime_diff_to_millis2 為 xt1−xt2，負值皆為 0。
    if(name=="_Xtime_diff_to_millis")return finish(milliseconds_until(call(0)));
    if(name=="_Xtime_diff_to_millis2"){
        const auto ticks=[&](std::uint64_t at){return std::bit_cast<std::int64_t>(memory.load(GuestAddress{at},64))*10000000+static_cast<std::int32_t>(memory.load(GuestAddress{at+8},32))/100;};
        const auto difference=ticks(call(0))-ticks(call(1));return finish(difference<=0?0:static_cast<std::uint32_t>(std::min<std::int64_t>((difference+9999)/10000,INT32_MAX)));
    }
    if(name=="xtime_get"){
        // 只支援 TIME_UTC（1）：填入 1970 起的秒與奈秒並回傳 base，其他 base 回 0。
        if(static_cast<std::uint32_t>(call(1))!=1)return finish(0);
        const auto now=runtime_.system_filetime()-UINT64_C(116444736000000000);memory.check(GuestAddress{call(0)},16,Access::Write);
        memory.store(GuestAddress{call(0)},64,now/10000000);memory.store(GuestAddress{call(0)+8},32,(now%10000000)*100);return finish(1);
    }
    if(name=="_Thrd_current"){
        // _Thrd_t｛目前執行緒的 pseudo handle, id｝：Win32 8 位元組走 EDX:EAX，Win64 16 位元組以隱藏指標回傳。
        const auto id=thread_identifier(threads_.current);
        if(x86_){call.return_to(state,UINT64_C(0xfffffffe)|(std::uint64_t{id}<<32),true);return std::nullopt;}
        memory.check(GuestAddress{call(0)},16,Access::Write);memory.store(GuestAddress{call(0)},64,UINT64_MAX-1);memory.store(GuestAddress{call(0)+8},64,id);return finish(call(0));
    }
    if(name=="_Thrd_equal" || name=="_Thrd_lt"){
        // 以 id 比較：Win32 以值傳遞（低 32 位元 handle、高 32 位元 id），Win64 以指標傳遞暫存複本。
        const auto id=[&](unsigned index)->std::uint32_t{return x86_?static_cast<std::uint32_t>(call(index)>>32):static_cast<std::uint32_t>(memory.load(GuestAddress{call(index)+8},32));};
        return finish(name=="_Thrd_equal"?id(0)==id(1):id(0)<id(1));
    }
    if(name=="_Call_once" || name=="_Call_onceEx"){
        // C11 風格的 _Once_flag（初值 0）：0 未執行、1 執行中、2 完成。完成由內部進入點 OnceComplete 標記；
        // 其他執行緒看到執行中時讓出 1 ms 後重新進入本函式，直到完成。
        const bool extended=name=="_Call_onceEx";const auto flag=call(0);memory.check(GuestAddress{flag},4,Access::Write);
        const auto value=memory.load(GuestAddress{flag},32);
        if(value==2)return finish(0);
        std::vector<GuestCallback> steps;
        if(value==1){
            steps.push_back({resolve("kernel32.dll","Sleep").value,{1}});
            steps.push_back({resolve(service.family,name).value,extended?std::vector<std::uint64_t>{flag,call(1),call(2)}:std::vector<std::uint64_t>{flag,call(1)}});
        }else {
            memory.store(GuestAddress{flag},32,1);
            steps.push_back({call(1),extended?std::vector<std::uint64_t>{call(2)}:std::vector<std::uint64_t>{}});
            steps.push_back({resolve("internal-msvcp","OnceComplete").value,{flag}});
        }
        start_callbacks(state,memory,std::move(steps),Continuation::Return,0);return std::nullopt;
    }
    if(name=="_Thrd_create"){
        // cdecl 的 _Thrd_start_t 經 __stdcall 進入點 ThreadRunner 呼叫；binder 由進入點釋放，建立結果與 _Thrd_start 相同寫回 _Thrd_t。
        memory.check(GuestAddress{call(0)},unit*2,Access::Write);
        const auto binder=allocate_heap(memory,unit*2);if(!binder)return finish(1);
        memory.store(GuestAddress{binder},bits,call(1));memory.store(GuestAddress{binder+unit},bits,call(2));
        return start("crt","_beginthreadex",{0,0,resolve("internal-msvcp","ThreadRunner").value,binder,0,call(0)+unit});
    }
    if(name=="_Thrd_start"){
        // _Thrd_t｛HANDLE, unsigned id｝；以 _beginthreadex 建立並直接寫入 id 欄位。
        memory.check(GuestAddress{call(0)},unit*2,Access::Write);
        return start("crt","_beginthreadex",{0,0,call(1),call(2),0,call(0)+unit});
    }
    if(name=="_Thrd_detach" || name=="_Thrd_join"){
        // _Thrd_t｛HANDLE, id｝在 x64 以指標傳遞暫存複本，x86 以值傳遞（兩個 DWORD）。
        const auto handle=x86_?call(0)&UINT32_MAX:memory.load(GuestAddress{call(0)},64);
        if(name=="_Thrd_detach")return start("kernel32.dll","CloseHandle",{handle});
        std::vector<GuestCallback> steps{{resolve("kernel32.dll","WaitForSingleObjectEx").value,{handle,UINT32_MAX,0}}};
        if(call(1))steps.push_back({resolve("kernel32.dll","GetExitCodeThread").value,{handle,call(1)}});
        steps.push_back({resolve("kernel32.dll","CloseHandle").value,{handle}});
        start_callbacks(state,memory,std::move(steps),Continuation::Return,0);return std::nullopt;
    }
    if(name=="_Cnd_register_at_thread_exit"){
        if(msvcp_thread_exit_notifications_.size()>=65536)throw HostCallFault("at_thread_exit 登記超過上限");
        msvcp_thread_exit_notifications_.push_back({threads_.current,call(0),call(1),call(2)});return finish(0);
    }
    if(name=="_Cnd_unregister_at_thread_exit"){
        std::erase_if(msvcp_thread_exit_notifications_,[&](const auto& entry){return entry[2]==call(0);});return finish(0);
    }
    if(name=="_Cnd_do_broadcast_at_thread_exit"){
        // 與 STL 相同：設定結果旗標、解除 mutex 後廣播；每筆登記只觸發一次。
        std::vector<GuestCallback> steps;
        std::erase_if(msvcp_thread_exit_notifications_,[&](const auto& entry){
            if(entry[0]!=threads_.current)return false;
            if(entry[3])memory.store(GuestAddress{entry[3]},32,1);
            const auto owner=entry[2]+(x86_?40U:72U);
            const auto count=memory.load(GuestAddress{owner+4},32);
            if(count>1)memory.store(GuestAddress{owner+4},32,count-1);
            else {
                memory.store(GuestAddress{owner},32,UINT32_MAX);memory.store(GuestAddress{owner+4},32,0);
                steps.push_back({resolve("kernel32.dll","ReleaseSRWLockExclusive").value,{entry[2]+unit*2}});
            }
            steps.push_back({resolve("kernel32.dll","WakeAllConditionVariable").value,{entry[1]+unit}});
            return true;
        });
        if(steps.empty())return finish(0);
        start_callbacks(state,memory,std::move(steps),Continuation::Return,0);return std::nullopt;
    }
    if(name=="_Mtx_init" || name=="_Cnd_init"){
        // 配置版與 *_init_in_situ 使用相同物件佈局；SRW／條件變數字組為 0 即初始狀態。
        const bool creating_mutex=name=="_Mtx_init";const auto size=creating_mutex?(x86_?48U:80U):(x86_?40U:72U);
        memory.check(GuestAddress{call(0)},unit,Access::Write);
        const auto object=allocate_heap(memory,size);if(!object)return finish(1);
        memory.write(GuestAddress{object},std::vector<std::uint8_t>(size));
        if(creating_mutex){memory.store(GuestAddress{object},32,call(1));memory.store(GuestAddress{object+(x86_?40U:72U)},32,UINT32_MAX);}
        memory.store(GuestAddress{call(0)},bits,object);return finish(0);
    }
    // 銷毀仍被持有的 mutex：release 版 msvcp140 的 _Mtx_destroy_in_situ 只有除錯版才斷言（_THREAD_ASSERT），
    // 實際照常返回（_Mtx_destroy 也照常釋放）；電馭判客結束時會這樣做。不中止程式，保留 SRW 鎖狀態讓持有者之後解鎖不出錯。
    const auto busy_destroy=[&]{static std::atomic<bool> noted{false};if(!noted.exchange(true))std::fprintf(stderr,"msvcp: 銷毀持有中的 mutex（release 版 MSVC 不檢查，照常繼續）\n");};
    if(name=="_Mtx_destroy" || name=="_Cnd_destroy"){
        if(const auto object=call(0)){
            if(name=="_Mtx_destroy"){
                if(memory.load(GuestAddress{object+(x86_?44U:76U)},32))busy_destroy();
                else runtime_.slim_locks.erase(object+unit*2);
            }
            (void)free_heap(memory,object,state);
        }
        return finish(0);
    }
    const bool mutex=name.starts_with("_Mtx_");
    const auto address=call(0),owner=address+(x86_?40U:72U),lock=address+unit*2;
    memory.check(GuestAddress{address},mutex?(x86_?48U:80U):(x86_?40U:72U),Access::Write);
    if(name.ends_with("init_in_situ")){
        if(mutex){
            memory.store(GuestAddress{address},32,call(1));
            memory.store(GuestAddress{address+unit},bits,0);
            memory.store(GuestAddress{owner},32,UINT32_MAX);
            memory.store(GuestAddress{owner+4},32,0);
        }else memory.store(GuestAddress{address},bits,0);
        return start("kernel32.dll",mutex?"InitializeSRWLock":"InitializeConditionVariable",
                     {mutex?lock:address+unit});
    }
    if(name.ends_with("destroy_in_situ")){
        if(mutex){
            if(memory.load(GuestAddress{owner+4},32))busy_destroy();
            else runtime_.slim_locks.erase(lock);
        }
        // 條件變數沒有獨立配置；STL 的原位解構器本身不修改儲存區。
        return finish(0);
    }
    if(mutex){
        const auto count=memory.load(GuestAddress{owner+4},32);
        const bool owned=memory.load(GuestAddress{owner},32)==thread_identifier(threads_.current);
        if(name=="_Mtx_current_owns")return finish(count && owned?1:0);
        if(name=="_Mtx_getconcrtcs")return finish(address+unit);
        if(name=="_Mtx_clear_owner"){memory.store(GuestAddress{owner},32,UINT32_MAX);memory.store(GuestAddress{owner+4},32,count-1);return finish(0);}
        if(name=="_Mtx_reset_owner"){memory.store(GuestAddress{owner},32,thread_identifier(threads_.current));memory.store(GuestAddress{owner+4},32,count+1);return finish(0);}
        if(name=="_Mtx_trylock"){
            if(owned){
                // 遞迴鎖或 plain 型別累加持有數；其餘非遞迴型別回 _Busy。
                const auto type=memory.load(GuestAddress{address},32);
                if(!(type&0x100U) && (type&~0x100U)!=1)return finish(3);
                if(count==INT32_MAX)return finish(3);
                memory.store(GuestAddress{owner+4},32,count+1);return finish(0);
            }
            return start("kernel32.dll","TryAcquireSRWLockExclusive",{lock});
        }
        if(name=="_Mtx_lock"){
            if(owned){
                const auto type=memory.load(GuestAddress{address},32);
                if((type&~0x100U)!=1 && !(type&0x100U))return finish(3);
                if(count==INT32_MAX)return finish(3);
                memory.store(GuestAddress{owner+4},32,count+1);return finish(0);
            }
            return start("kernel32.dll","AcquireSRWLockExclusive",{lock});
        }
        if(!owned || !count)throw HostCallFault("執行緒未持有 MSVC mutex");
        memory.store(GuestAddress{owner+4},32,count-1);
        if(count>1)return finish(0);
        memory.store(GuestAddress{owner},32,UINT32_MAX);
        return start("kernel32.dll","ReleaseSRWLockExclusive",{lock});
    }
    if(name=="_Cnd_wait" || name=="_Cnd_timedwait"){
        const auto mtx=call(1),mtx_owner=mtx+(x86_?40U:72U);
        memory.check(GuestAddress{mtx},x86_?48U:80U,Access::Write);
        if(memory.load(GuestAddress{mtx_owner},32)!=thread_identifier(threads_.current) || memory.load(GuestAddress{mtx_owner+4},32)!=1)
            throw HostCallFault("MSVC 條件變數必須搭配只取得一次的 mutex");
        memory.store(GuestAddress{mtx_owner},32,UINT32_MAX);
        memory.store(GuestAddress{mtx_owner+4},32,0);
        return start("kernel32.dll","SleepConditionVariableSRW",{address+unit,mtx+unit*2,name=="_Cnd_timedwait"?milliseconds_until(call(2)):UINT32_MAX,0});
    }
    return start("kernel32.dll",name=="_Cnd_signal"?"WakeConditionVariable":"WakeAllConditionVariable",{address+unit});
}
