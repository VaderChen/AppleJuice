// 包含於 windows_nt_native.cpp：NT 同步物件、計時、APC 與例外續行的系統服務（ntdll 的 Nt／Zw 匯出與直接系統呼叫共用）。
// 建立、開啟、訊號與等待都轉交既有 Win32 物件實作，只在這一層轉換 OBJECT_ATTRIBUTES、存取遮罩與 NTSTATUS；
// 物件狀態查詢直接讀同一份等待物件，不另存一套狀態。
namespace {
constexpr std::string_view nt_object_symbols[]{
    "NtCreateEvent","NtOpenEvent","NtSetEvent","NtResetEvent","NtClearEvent","NtQueryEvent","NtSetEventBoostPriority",
    "NtCreateMutant","NtOpenMutant","NtReleaseMutant","NtCreateSemaphore","NtOpenSemaphore","NtReleaseSemaphore",
    "NtCreateTimer","NtOpenTimer","NtSetTimer","NtCancelTimer","NtQueryTimer","NtSetTimerResolution","NtQueryTimerResolution",
    "NtDelayExecution","NtYieldExecution","NtQueryPerformanceCounter","NtQuerySystemTime","NtSetSystemTime","NtGetTickCount",
    "NtGetCurrentProcessorNumber","NtFlushProcessWriteBuffers","NtTestAlert","NtQueueApcThread","NtQueueApcThreadEx",
    "NtAlertThreadByThreadId","NtWaitForAlertByThreadId"};
}
bool nt_object_service(std::string_view symbol){return std::find(std::begin(nt_object_symbols),std::end(nt_object_symbols),symbol)!=std::end(nt_object_symbols);}
std::optional<std::uint32_t> WindowsServices::invoke_nt_object(GuestState& entry_state,GuestMemory& entry_memory,const WindowsServiceDescriptor& service){
    return nested_continuation(entry_state,entry_memory,"NtObject",
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(state,memory,service);const auto name=service.symbol;const auto saved_error=last_error_;
            const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
            constexpr std::uint32_t invalid_parameter=0xc000000dU,invalid_handle=0xc0000008U,type_mismatch=0xc0000024U,access_violation=0xc0000005U;
            const auto finish=[&](std::uint32_t status){threads_.items.at(threads_.current).last_native_status=status;last_error_=saved_error;call.return_to(state,status);return std::optional<std::uint32_t>{};};
            const auto api=[&](std::string_view symbol){return resolve("kernel32.dll",symbol).value;};
            const auto word=[&](unsigned index){return static_cast<std::uint32_t>(call(index));};
            // Win32 失敗碼轉 NTSTATUS：無效 handle 依 handle 是否存在區分「無效」與「物件型別不符」。
            const auto failure=[&](std::uint64_t handle){const auto error=last_error_;return error==6 && handle && !object_type(handle).empty()?type_mismatch:nt_status(error);};
            // 相對（負值）或絕對（正值，系統時間）的 100 ns 期限轉成毫秒；NULL 或最小負值為無限。
            const auto milliseconds=[&](std::uint64_t pointer){return nt_timeout_milliseconds(memory,pointer,runtime_.system_filetime());};
            const auto wait_object=[&](std::uint64_t handle,WaitObject::Kind kind,std::uint32_t& status)->WaitObject*{
                const auto found=runtime_.wait_objects.find(handle);
                if(found==runtime_.wait_objects.end()){status=object_type(handle).empty()?invalid_handle:type_mismatch;return nullptr;}
                if(found->second.kind!=kind){status=type_mismatch;return nullptr;}
                return &found->second;
            };
            GuestScratch scratch(*this,memory,state);
            try{
                const bool creating=name=="NtCreateEvent" || name=="NtCreateMutant" || name=="NtCreateSemaphore" || name=="NtCreateTimer";
                const bool opening=name=="NtOpenEvent" || name=="NtOpenMutant" || name=="NtOpenSemaphore" || name=="NtOpenTimer";
                if(creating || opening){
                    // (Handle*, DesiredAccess, ObjectAttributes*, …)：名稱與安全屬性放在暫存區交給 Win32 建立／開啟函式。
                    if(!call(0))co_return finish(access_violation);
                    memory.check(GuestAddress{call(0)},unit,Access::Write);
                    Win32ObjectRequest request;
                    if(const auto status=win32_object_request(memory,scratch,runtime_,call(2),x86_,creating,request,[this](std::u16string& text,bool create){return namespace_object_name(text,create);},name=="NtCreateEvent" || name=="NtOpenEvent"))co_return finish(status);
                    const auto security=request.security,text=request.name;
                    const auto access=word(1);std::uint64_t handle=0;
                    if(name=="NtCreateEvent"){
                        if(word(3)>1)co_return finish(invalid_parameter); // 0 NotificationEvent（手動重設）、1 SynchronizationEvent
                        handle=co_await service_callback::call_value(resolve("internal-nt-event","CreateEventExW").value,{security,text,(word(3)==0?1U:0U)|((call(4)&0xffU)?2U:0U),access,request.flags});
                    }else if(name=="NtCreateMutant")handle=co_await service_callback::call_value(api("CreateMutexExW"),{security,text,(call(3)&0xffU)?1U:0U,access});
                    else if(name=="NtCreateSemaphore"){
                        const auto initial=static_cast<std::int32_t>(word(3)),maximum=static_cast<std::int32_t>(word(4));
                        if(maximum<=0 || initial<0 || initial>maximum)co_return finish(invalid_parameter);
                        handle=co_await service_callback::call_value(api("CreateSemaphoreExW"),{security,word(3),word(4),text,0,access});
                    }else if(name=="NtCreateTimer"){
                        if(word(3)>1)co_return finish(invalid_parameter);
                        handle=co_await service_callback::call_value(api("CreateWaitableTimerExW"),{security,text,word(3)==0?1U:0U,access});
                    }else if(name=="NtOpenEvent")handle=co_await service_callback::call_value(resolve("internal-nt-event","OpenEventW").value,{access,request.inherit(),text,request.flags});
                    else handle=co_await service_callback::call_value(api(name=="NtOpenEvent"?"OpenEventW":name=="NtOpenMutant"?"OpenMutexW":name=="NtOpenSemaphore"?"OpenSemaphoreW":"OpenWaitableTimerW"),{access,request.inherit(),text});
                    if(x86_)handle&=UINT32_MAX;
                    if(!handle)co_return finish(last_error_==6?type_mismatch:nt_status(last_error_));
                    std::uint32_t status=0;
                    if(creating && last_error_==183){
                        // 同名物件已存在：OBJ_OPENIF 時成功並回報 STATUS_OBJECT_NAME_EXISTS，否則為名稱衝突。
                        if(!(request.flags&0x80U)){(void)co_await service_callback::call(api("CloseHandle"),{handle});co_return finish(0xc0000035U);}
                        status=0x40000000U;
                    }
                    memory.store(GuestAddress{call(0)},bits,handle);co_return finish(status);
                }
                if(name=="NtSetEvent" || name=="NtResetEvent" || name=="NtClearEvent" || name=="NtSetEventBoostPriority"){
                    std::uint32_t status=0;const auto* event=wait_object(call(0),WaitObject::Kind::Event,status);if(!event)co_return finish(status);
                    const bool previous=event->signaled();
                    if(name=="NtSetEventBoostPriority" && event->manual_reset)co_return finish(type_mismatch); // 只適用同步（自動重設）事件
                    const bool set=name=="NtSetEvent" || name=="NtSetEventBoostPriority";
                    if(!co_await service_callback::call(api(set?"SetEvent":"ResetEvent"),{call(0)}))co_return finish(failure(call(0)));
                    if((name=="NtSetEvent" || name=="NtResetEvent") && call(1))memory.store(GuestAddress{call(1)},32,previous?1U:0U);
                    co_return finish(0);
                }
                if(name=="NtQueryEvent"){
                    // EventBasicInformation：EventType（0 手動重設、1 自動重設）與 EventState。
                    std::uint32_t status=0;const auto* event=wait_object(call(0),WaitObject::Kind::Event,status);if(!event)co_return finish(status);
                    if(word(1)!=0)co_return finish(0xc0000003U);if(word(3)!=8)co_return finish(0xc0000004U);
                    if(!(event->access&1U))co_return finish(0xc0000022U);
                    memory.store(GuestAddress{call(2)},32,event->manual_reset?0U:1U);memory.store(GuestAddress{call(2)+4},32,event->signaled()?1U:0U);
                    if(call(4))memory.store(GuestAddress{call(4)},32,8);co_return finish(0);
                }
                if(name=="NtReleaseMutant"){
                    std::uint32_t status=0;const auto* handle=wait_object(call(0),WaitObject::Kind::Mutex,status);if(!handle)co_return finish(status);
                    // PreviousCount＝釋放前的 CurrentCount（1－遞迴次數），與 NtQueryMutant 同一來源。
                    const auto& mutex=runtime_.mutex_objects.at(handle->mutex);std::uint32_t recursion=mutex.recursion;
                    if(mutex.shared){SharedEventLock lock(mutex.shared->lock_descriptor());recursion=mutex.shared->state_locked().recursion;}
                    if(!co_await service_callback::call(api("ReleaseMutex"),{call(0)}))co_return finish(failure(call(0)));
                    if(call(1))memory.store(GuestAddress{call(1)},32,static_cast<std::uint32_t>(1-static_cast<std::int32_t>(recursion)));
                    co_return finish(0);
                }
                if(name=="NtReleaseSemaphore"){
                    std::uint32_t status=0;if(!wait_object(call(0),WaitObject::Kind::Semaphore,status))co_return finish(status);
                    if(static_cast<std::int32_t>(word(1))<=0)co_return finish(invalid_parameter);
                    if(!co_await service_callback::call(api("ReleaseSemaphore"),{call(0),word(1),call(2)}))co_return finish(failure(call(0)));
                    co_return finish(0);
                }
                if(name=="NtSetTimer" || name=="NtCancelTimer" || name=="NtQueryTimer"){
                    std::uint32_t status=0;const auto* handle=wait_object(call(0),WaitObject::Kind::Timer,status);if(!handle)co_return finish(status);
                    // 計時器狀態與 handle 分開保存（同一計時器的多個 handle 共用）。
                    const auto* timer=&runtime_.wait_state(*handle);const bool previous=timer->signaled();
                    if(name=="NtQueryTimer"){
                        // TimerBasicInformation：RemainingTime（100 ns；未啟動或已到期為負的經過時間）與 TimerState。
                        if(word(1)!=0)co_return finish(0xc0000003U);if(word(3)!=16)co_return finish(0xc0000004U);
                        const auto now=runtime_.clock_ticks;
                        const auto remaining=timer->active?static_cast<std::int64_t>(timer->due)-static_cast<std::int64_t>(now):0;
                        memory.store(GuestAddress{call(2)},64,static_cast<std::uint64_t>(remaining));memory.store(GuestAddress{call(2)+8},64,previous?1U:0U);
                        if(call(4))memory.store(GuestAddress{call(4)},32,16);co_return finish(0);
                    }
                    if(name=="NtCancelTimer"){
                        if(!co_await service_callback::call(api("CancelWaitableTimer"),{call(0)}))co_return finish(failure(call(0)));
                        if(call(1))memory.store(GuestAddress{call(1)},8,previous?1U:0U);co_return finish(0);
                    }
                    // (Handle, DueTime*, ApcRoutine, ApcContext, Resume, Period, PreviousState*)：APC 常式與 Win32 完成常式同為 (context, low, high)。
                    if(!call(1))co_return finish(access_violation);
                    if(static_cast<std::int32_t>(word(5))<0)co_return finish(invalid_parameter);
                    if(!co_await service_callback::call(api("SetWaitableTimer"),{call(0),call(1),word(5),call(2),call(3),call(4)&0xffU}))co_return finish(failure(call(0)));
                    if(call(6))memory.store(GuestAddress{call(6)},8,previous?1U:0U);
                    // 沙箱沒有可由計時器喚醒的系統睡眠狀態：要求 Resume 時與不支援喚醒的 Windows 相同回報已忽略。
                    co_return finish((call(4)&0xffU)?0x40000025U:0U);
                }
                if(name=="NtSetTimerResolution" || name=="NtQueryTimerResolution"){
                    // 計時器解析度以 100 ns 計：最粗 15.625 ms、最細 0.5 ms；每個程序各自要求。
                    constexpr std::uint32_t coarsest=156250,finest=5000;
                    if(name=="NtQueryTimerResolution"){
                        if(!call(0) || !call(1) || !call(2))co_return finish(access_violation);
                        memory.store(GuestAddress{call(0)},32,coarsest);memory.store(GuestAddress{call(1)},32,finest);
                        memory.store(GuestAddress{call(2)},32,runtime_.timer_resolution_set?runtime_.timer_resolution:coarsest);co_return finish(0);
                    }
                    if(!call(2))co_return finish(access_violation);
                    if(call(1)&0xffU){
                        runtime_.timer_resolution=std::clamp(word(0),finest,coarsest);runtime_.timer_resolution_set=true;
                        memory.store(GuestAddress{call(2)},32,runtime_.timer_resolution);co_return finish(0);
                    }
                    const bool was_set=runtime_.timer_resolution_set;runtime_.timer_resolution_set=false;runtime_.timer_resolution=coarsest;
                    memory.store(GuestAddress{call(2)},32,coarsest);co_return finish(was_set?0U:0xc0000245U); // STATUS_TIMER_RESOLUTION_NOT_SET
                }
                if(name=="NtDelayExecution"){
                    if(!call(1))co_return finish(access_violation);
                    const auto result=co_await service_callback::call(api("SleepEx"),{milliseconds(call(1)),call(0)&0xffU});
                    co_return finish(result==0xc0U?0xc0U:0U); // WAIT_IO_COMPLETION＝STATUS_USER_APC
                }
                if(name=="NtYieldExecution")co_return finish(co_await service_callback::call(api("SwitchToThread"),{})?0U:0x40000024U); // STATUS_NO_YIELD_PERFORMED
                if(name=="NtQueryPerformanceCounter"){
                    if(!call(0))co_return finish(access_violation);
                    memory.check(GuestAddress{call(0)},8,Access::Write);if(call(1))memory.check(GuestAddress{call(1)},8,Access::Write);
                    (void)co_await service_callback::call(api("QueryPerformanceCounter"),{call(0)});
                    if(call(1))(void)co_await service_callback::call(api("QueryPerformanceFrequency"),{call(1)});
                    co_return finish(0);
                }
                if(name=="NtQuerySystemTime"){
                    if(!call(0))co_return finish(access_violation);
                    (void)co_await service_callback::call(api("GetSystemTimeAsFileTime"),{call(0)});co_return finish(0);
                }
                if(name=="NtSetSystemTime"){
                    // 需要 SeSystemtimePrivilege；沙箱的使用者權杖沒有這項特權（與一般使用者在 Windows 上相同）。
                    if(!call(0))co_return finish(access_violation);
                    co_return finish(0xc0000061U);
                }
                if(name=="NtGetTickCount"){const auto ticks=co_await service_callback::call(api("GetTickCount"),{});last_error_=saved_error;call.return_to(state,ticks);co_return std::nullopt;}
                if(name=="NtGetCurrentProcessorNumber"){const auto number=co_await service_callback::call(api("GetCurrentProcessorNumber"),{});last_error_=saved_error;call.return_to(state,number);co_return std::nullopt;}
                if(name=="NtFlushProcessWriteBuffers"){(void)co_await service_callback::call(api("FlushProcessWriteBuffers"),{});co_return finish(0);}
                if(name=="NtTestAlert"){
                    // 送達排入的使用者 APC（與可警示的零時間等待相同）；本程序沒有其他來源的執行緒警示。
                    (void)co_await service_callback::call(api("SleepEx"),{0,1});co_return finish(0);
                }
                if(name=="NtQueueApcThread" || name=="NtQueueApcThreadEx"){
                    // (Thread, [Reserve／旗標], ApcRoutine, Arg1, Arg2, Arg3)：NT APC 常式收到三個參數。
                    const unsigned shift=name.ends_with("Ex")?1U:0U;
                    const auto id=thread_id(call(0));if(!id)co_return finish(object_type(call(0)).empty()?invalid_handle:type_mismatch);
                    if(!(thread_access(call(0))&0x10U))co_return finish(0xc0000022U);
                    if(!call(1+shift))co_return finish(invalid_parameter);
                    auto& target=threads_.items.at(id);
                    if(target.status==ThreadStatus::Exited || target.terminating)co_return finish(0xc000004bU); // STATUS_THREAD_IS_TERMINATING
                    if(target.apcs.size()>=1024)co_return finish(0xc0000017U);
                    target.apcs.push_back({call(1+shift),{call(2+shift),call(3+shift),call(4+shift)}});threads_.yield=true;co_return finish(0);
                }
                if(name=="NtAlertThreadByThreadId" || name=="NtWaitForAlertByThreadId"){
                    // 每個執行緒一個自動重設的警示事件：依執行緒識別碼喚醒、等待端以期限等待（Windows 的 WaitOnAddress／SRW 等待基礎）。
                    const bool alert=name=="NtAlertThreadByThreadId";std::uint64_t target=threads_.current;
                    if(alert){
                        target=UINT64_MAX;const auto wanted=static_cast<std::uint32_t>(call(0));
                        for(const auto& [index,item]:threads_.items)if(item.status!=ThreadStatus::Exited && thread_identifier(index)==wanted){target=index;break;}
                        if(target==UINT64_MAX)co_return finish(0xc000000bU); // STATUS_INVALID_CID
                    }
                    auto event=thread_alert_events_.find(target);
                    if(event==thread_alert_events_.end()){
                        const auto handle=co_await service_callback::call_value(api("CreateEventW"),{0,0,0,0});
                        if(!handle)co_return finish(nt_status(last_error_));
                        event=thread_alert_events_.emplace(target,x86_?handle&UINT32_MAX:handle).first;
                    }
                    if(alert){(void)co_await service_callback::call(api("SetEvent"),{event->second});co_return finish(0);}
                    const auto handle=event->second;
                    const auto result=co_await service_callback::call(api("WaitForSingleObject"),{handle,milliseconds(call(1))});
                    co_return finish(result==0?0x101U:result==0x102U?0x102U:nt_status(last_error_)); // STATUS_ALERTED／STATUS_TIMEOUT
                }
            }catch(const NativeError& error){co_return finish(error.status);}
            catch(const MemoryFault&){co_return finish(access_violation);}
            throw HostCallFault("未知的 NT 物件服務");
        });
}
// NtContinue／NtRaiseException 與 RtlRestoreContext／RtlRaiseException／RtlRaiseStatus：以指定的 CONTEXT 續行或派送例外。
// 續行會跨越的主機回呼框架與其上的例外派送狀態一併丟棄（與不含 SEH 目標的 longjmp 相同）。
GuestState restore_jump_buffer(const GuestMemory& memory,std::uint64_t address,const GuestState& state,std::uint32_t value);
bool nt_continuation_service(std::string_view symbol){
    return symbol=="NtContinue" || symbol=="NtContinueEx" || symbol=="NtRaiseException" || symbol=="RtlRestoreContext" || symbol=="RtlRaiseException" || symbol=="RtlRaiseStatus";
}
std::uint64_t WindowsServices::native_dispatcher_entry(std::string_view symbol) const{
    const auto module=runtime_.modules.find("ntdll.dll");if(module==runtime_.modules.end())throw HostCallFault("尚未載入使用者派送模組");
    const auto* image=module_image(module->second);if(!image)throw HostCallFault("使用者派送模組缺少映像");
    const auto name=image->export_names.find(std::string(symbol));if(name==image->export_names.end())throw HostCallFault("使用者派送模組缺少入口");
    return image->base+image->export_ordinals.at(name->second);
}
bool WindowsServices::enter_user_apc(GuestState& state,GuestMemory& memory,GuestCallback callback,std::uint64_t completion,bool completion_error){
    auto resumed=state;
    if(completion!=UINT64_MAX){
        complete_call(resumed,memory,completion);
        const auto& calls=threads_.items.at(threads_.current).system_calls;
        bool native_wait=false;
        if(!calls.empty()){
            const auto& frame=calls.back();const auto& table=system_call_table();
            // 巢狀 APC／視窗回呼內的 Win32 等待，不屬於更外層尚未返回的 NT 呼叫。
            for(auto i=runtime_.callbacks.rbegin();i!=runtime_.callbacks.rend();++i){
                if(i->continuation!=Continuation::NestedResult && i->continuation!=Continuation::NativeOperation)break;
                if(frame.call_id<table.targets.size() && i->saved.rip.value==table.targets[frame.call_id] && i->saved.reg(Gpr::Rsp)==frame.stack){native_wait=true;break;}
            }
        }
        if(native_wait){
            // NT 的可警示等待可能經過宿主 coroutine；對外只暴露真正的 syscall 返回狀態。
            const auto& frame=calls.back();const auto& table=system_call_table();
            const auto name=frame.call_id<table.descriptors.size()?table.descriptors[frame.call_id]->symbol:std::string_view{};
            resumed.rip=GuestAddress{frame.resume};resumed.reg(Gpr::Rsp)=frame.resume_stack;
            resumed.reg(Gpr::Rcx)=frame.resume_rcx;resumed.reg(Gpr::Rdx)=frame.resume_rdx;
            resumed.reg(Gpr::R11)=frame.resume_r11;resumed.rflags=frame.flags;
            resumed.reg(Gpr::Rax)=name=="NtTestAlert" || name=="ZwTestAlert"?0U:0xc0U;
        }
        // GetQueuedCompletionStatusEx 在最後一個 APC 完成後才發布 WAIT_IO_COMPLETION。
        // 回呼可以修改 LastError，不能提前寫入後被回呼覆蓋；NT 入口則不改 Win32 錯誤。
        completion_error|=!completion && !native_wait;
    }
    // x64 APC 入口的前 0x4d0 bytes 為 CONTEXT，Home slots 放入三個參數與常式。
    constexpr std::uint64_t size=0x500;
    if(resumed.reg(Gpr::Rsp)<size)throw HostCallFault("使用者 APC 堆疊不足");
    const auto stack=(resumed.reg(Gpr::Rsp)-size)&~UINT64_C(15);
    memory.check(GuestAddress{stack},size,Access::Write);memory.write(GuestAddress{stack},std::array<std::uint8_t,size>{});
    write_context(memory,stack,resumed);
    for(unsigned i=0;i<3;++i)memory.store(GuestAddress{stack+i*8},64,i<callback.arguments.size()?callback.arguments[i]:0);
    // Winsock 等完成常式可能超過原生 APC 的三個參數，經共用配接入口保存完整 ABI。
    const auto routine=callback.arguments.size()>3?resolve("internal-apc-dispatch","Invoke").value:callback.target;
    memory.store(GuestAddress{stack+24},64,routine);
    memory.store(GuestAddress{stack+0x4d0},64,resumed.rip.value);memory.store(GuestAddress{stack+0x4d8},64,0x33);
    memory.store(GuestAddress{stack+0x4e0},64,memory.load(GuestAddress{stack+0x44},32));
    memory.store(GuestAddress{stack+0x4e8},64,resumed.reg(Gpr::Rsp));memory.store(GuestAddress{stack+0x4f0},64,0x2b);
    if(runtime_.callbacks.size()>=128)throw HostCallFault("客體 callback 巢狀深度超過上限");
    runtime_.callbacks.emplace_back(resumed,std::vector<GuestCallback>{std::move(callback)},0,
        completion_error?Continuation::NativeApcIoError:Continuation::NativeApc,stack,exceptions_.stack.size());
    state=resumed;state.reg(Gpr::Rsp)=stack;state.rip=GuestAddress{native_dispatcher_entry("KiUserApcDispatcher")};
    enter_process_instrumentation(state);return true;
}
std::optional<std::uint32_t> WindowsServices::finish_user_apc_entry(GuestState& state,GuestMemory& memory){
    if(x86_)throw HostCallFault("KiUserApcDispatcher 需要 x64 核心堆疊 ABI");
    const auto stack=state.reg(Gpr::Rsp);memory.check(GuestAddress{stack},0x4d0,Access::Read);
    if(runtime_.callbacks.empty() || !native_apc_continuation(runtime_.callbacks.back().continuation) || runtime_.callbacks.back().value!=stack){
        // 由客體直接進入時仍採標準 CONTEXT，返回也必須真正續行。
        if(runtime_.callbacks.size()>=128)throw HostCallFault("客體 callback 巢狀深度超過上限");
        auto resumed=read_context(memory,stack,state);
        runtime_.callbacks.emplace_back(resumed,std::vector<GuestCallback>{{}},0,Continuation::NativeApc,stack,exceptions_.stack.size());
    }
    auto& callback=runtime_.callbacks.back().callbacks.front();const auto routine=memory.load(GuestAddress{stack+24},64);
    if(routine==resolve("internal-apc-dispatch","Invoke").value && callback.arguments.size()>3){
        state.rip=GuestAddress{routine};return std::nullopt;
    }
    callback.target=routine;
    callback.arguments.clear();for(unsigned i=0;i<3;++i)callback.arguments.push_back(memory.load(GuestAddress{stack+i*8},64));
    enter_callback(state,memory);return std::nullopt;
}
void WindowsServices::restore_user_context(GuestState& state,GuestMemory& memory,const GuestState& restored){
        abandon_callbacks_above(state,memory,restored.reg(Gpr::Rsp));
        // 準備例外的客體回呼可直接 NtContinue；離開的核心框架不能留在下一次派送。
        while(!exceptions_.stack.empty() && exceptions_.stack.back().native_stack && restored.reg(Gpr::Rsp)>=exceptions_.stack.back().native_stack+0x598){
            exceptions_.stack.pop_back();sync_msvc_uncaught(memory);
        }
        // NtContinue 不經 syscall return 返回；同步釋放被續行跨過的借用框架。
        auto& calls=threads_.items.at(threads_.current).system_calls;
        while(!calls.empty() && restored.reg(Gpr::Rsp)>=calls.back().stack){
            const auto& frame=calls.back();
            if(x86_)memory.write(GuestAddress{frame.stack},frame.saved_memory);else memory.store(GuestAddress{frame.stack},64,frame.caller);
            calls.pop_back();
        }
        state=restored;
}
// 回呼的客體框架從 ((saved-256)&~15)-8 開始；堆疊指標在此之上的執行點已離開該回呼。NtContinue／RtlRestoreContext 續行，
// 以及客體不經服務自行還原內容（CoreCLR 以組語還原非揮發暫存器後直接跳到 catch 續行點）後再發生的例外，都依此結束被跨過的回呼與派送。
void WindowsServices::abandon_callbacks_above(GuestState& state,GuestMemory& memory,std::uint64_t rsp){
        while(!runtime_.callbacks.empty() && rsp>=((runtime_.callbacks.back().saved.reg(Gpr::Rsp)-256)&~UINT64_C(15))){
            const auto& callback=runtime_.callbacks.back();
            auto depth=std::min(callback.exception_depth,exceptions_.stack.size());
            // Exception／ContinueException 的深度包含正在派送的那筆例外。
            // NtContinue 跳過正常回呼返回時，必須一併結束它；不能留下
            // 已完成的派送，讓每次成功續行都累積成新的巢狀例外。
            if((callback.continuation==Continuation::Exception || callback.continuation==Continuation::ContinueException) &&
               callback.exception_depth && callback.exception_depth<=exceptions_.stack.size()){
                depth=callback.exception_depth-1;
                if(x86_)leave_x86_exception_handler(memory,exceptions_.stack[depth]);
            }
            exceptions_.stack.resize(depth);sync_msvc_uncaught(memory);discard_callbacks(runtime_.callbacks.size()-1,state,memory);
        }
}
std::optional<std::uint32_t> WindowsServices::invoke_nt_continuation(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto name=service.symbol;const auto bits=static_cast<std::uint8_t>(x86_?32:64);const unsigned unit=x86_?4U:8U;
    const auto finish=[&](std::uint32_t status){call.return_to(state,status);return std::optional<std::uint32_t>{};};
    const auto restore=[&](const GuestState& restored){
        if(!exceptions_.stack.empty()){
            const auto& dispatch=exceptions_.stack.back();
            detail::trace_handler_resume_instruction(restored,memory,dispatch.code,dispatch.original.rip.value,process_id(),threads_.current);
        }
        if(restored.reg(Gpr::Rsp)<256 && std::getenv("X64ARM_TRACE_FAULT_MEMORY"))std::fprintf(stderr,"[context_restore_invalid_stack] api=%.*s context=%llx flags=%llx old_rip=%llx old_rsp=%llx new_rip=%llx new_rsp=%llx\n",static_cast<int>(name.size()),name.data(),static_cast<unsigned long long>(call(0)),static_cast<unsigned long long>(memory.load(GuestAddress{call(0)+(x86_?0U:48U)},32)),static_cast<unsigned long long>(state.rip.value),static_cast<unsigned long long>(state.reg(Gpr::Rsp)),static_cast<unsigned long long>(restored.rip.value),static_cast<unsigned long long>(restored.reg(Gpr::Rsp)));
        restore_user_context(state,memory,restored);
    };
    const auto raise=[&](GuestState origin,std::uint64_t record,std::optional<std::uint64_t> address,bool first_chance){
        const auto code=static_cast<std::uint32_t>(memory.load(GuestAddress{record},32)),flags=static_cast<std::uint32_t>(memory.load(GuestAddress{record+4},32));
        const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{record+(x86_?16U:24U)},32));
        if(count>15)return finish(0xc000000dU);
        std::vector<std::uint64_t> parameters;for(unsigned i=0;i<count;++i)parameters.push_back(memory.load(GuestAddress{record+(x86_?20U:32U)+i*unit},bits));
        begin_exception(origin,memory,code,flags&1U,parameters);
        auto& dispatch=exceptions_.stack.back();
        memory.store(GuestAddress{dispatch.record+(x86_?12U:16U)},bits,address?*address:memory.load(GuestAddress{record+(x86_?12U:16U)},bits));
        // 第二次機會：不再搜尋處理常式，直接進入未處理流程（偵錯器、程序結束）。
        if(!first_chance){dispatch.stage=ExceptionStage::Unhandled;dispatch.unhandled_called=true;}
        if(first_chance)enter_user_exception(origin,memory);
        state=origin;return std::optional<std::uint32_t>{};
    };
    try{
        if(name=="NtContinue" || name=="NtContinueEx"){
            if(!call(0))return finish(0xc0000005U);
            memory.check(GuestAddress{call(0)},x86_?716:1232,Access::Read);
            restore(read_context(memory,call(0),state));return std::nullopt;
        }
        if(name=="RtlRestoreContext"){
            if(!call(0))return finish(0xc0000005U);
            memory.check(GuestAddress{call(0)},x86_?716:1232,Access::Read);
            auto restored=read_context(memory,call(0),state);
            if(const auto record=call(1)){
                const auto code=static_cast<std::uint32_t>(memory.load(GuestAddress{record},32));
                const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{record+(x86_?16U:24U)},32));
                const auto first=count?memory.load(GuestAddress{record+(x86_?20U:32U)},bits):0;
                // STATUS_LONGJUMP：ExceptionInformation[0] 是 jmp_buf，非揮發暫存器與目標位址取自其中。
                if(code==0x80000026U && count==1 && first && !x86_)restored=restore_jump_buffer(memory,first,restored,static_cast<std::uint32_t>(restored.reg(Gpr::Rax)));
                // STATUS_UNWIND_CONSOLIDATE：呼叫 ExceptionInformation[0] 的收尾回呼，其回傳值為續行位址。
                else if(code==0x80000029U && count>=1 && first){
                    restore(restored);const auto resumed=state;
                    start_callbacks(state,memory,{{first,{record}}},Continuation::UnwindConsolidate,exceptions_.stack.size());
                    runtime_.callbacks.back().unwind_target=resumed;return std::nullopt;
                }
            }
            restore(restored);return std::nullopt;
        }
        if(name=="NtRaiseException"){
            if(!call(0) || !call(1))return finish(0xc0000005U);
            memory.check(GuestAddress{call(1)},x86_?716:1232,Access::Read);
            auto origin=read_context(memory,call(1),state);
            while(!runtime_.callbacks.empty() && origin.reg(Gpr::Rsp)>=((runtime_.callbacks.back().saved.reg(Gpr::Rsp)-256)&~UINT64_C(15))){
                exceptions_.stack.resize(runtime_.callbacks.back().exception_depth);sync_msvc_uncaught(memory);discard_callbacks(runtime_.callbacks.size()-1,state,memory);
            }
            return raise(origin,call(0),std::nullopt,(call(2)&0xffU)!=0);
        }
        // RtlRaiseException(record)／RtlRaiseStatus(status)：以呼叫者的內容派送；ExceptionAddress 為呼叫者的返回位址。
        auto caller=state;call.return_to(caller,state.reg(Gpr::Rax));
        if(name=="RtlRaiseStatus"){
            begin_exception(caller,memory,static_cast<std::uint32_t>(call(0)),1,{});state=caller;return std::nullopt;
        }
        if(!call(0))return finish(0xc0000005U);
        return raise(caller,call(0),caller.rip.value,true);
    }catch(const MemoryFault&){return finish(0xc0000005U);}
}
