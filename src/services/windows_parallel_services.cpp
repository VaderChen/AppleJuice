#include "x64arm/windows_services.hpp"
#include <chrono>
#include "x64arm/startup_profile.hpp"
#include "monotonic_clock.hpp"
#include "srw_trace.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace x64arm {
#ifdef X64ARM_HOST_SERVICES_RESIDENT_TIMESTAMP
bool WindowsServices::resident_timestamp_counter(const GuestState&,const GuestMemory& memory,std::uint64_t& result) const noexcept {
    if(!runtime_.host_clock)return false;
    // 與一般 timestamp_counter 相同的單調時間軸及 CPUID 標稱頻率；worker
    // 只讀硬體時間，不呼叫 retired，也不寫全體執行緒共用的 KUSER 時間頁。
    const auto ticks=detail::monotonic_ticks();
    const auto& identity=memory.processor_identity();
    const std::uint64_t mhz=identity.base_mhz?identity.base_mhz:identity.maximum_mhz;
    result=mhz?static_cast<std::uint64_t>(static_cast<unsigned __int128>(ticks)*mhz/10U):ticks;
    return true;
}
#endif
#ifdef X64ARM_HOST_SERVICES_PARALLEL_SERVICE
namespace {
// SRW 慢路徑（客體常式 CAS 失敗、或未安裝客體常式）在 worker 完成可判定的情況：取得、Try 失敗、
// 無等待者的釋放。有宿主等待者（bit1）的釋放、需要排隊的取得仍交協調者。X64ARM_PARALLEL_SRW=0 關閉（同成品對照）。
bool parallel_srw_enabled() noexcept {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_PARALLEL_SRW");return !setting || std::string_view(setting)!="0";}();
    return enabled;
}
bool srw_service(std::string_view symbol) noexcept {
    return symbol=="AcquireSRWLockExclusive" || symbol=="AcquireSRWLockShared" ||
        symbol=="TryAcquireSRWLockExclusive" || symbol=="TryAcquireSRWLockShared" ||
        symbol=="ReleaseSRWLockExclusive" || symbol=="ReleaseSRWLockShared";
}
// 持有者在其他 worker 上執行時，短暫等待通常比進協調者排隊便宜（Windows SRW 亦先自旋）；
// 約 1～2 µs 後仍未釋放就交回協調者，不讓等待者佔住 worker。
constexpr unsigned srw_spin_limit=512;
inline void spin_pause() noexcept {
#if defined(__aarch64__)
    __builtin_arm_yield();
#endif
}
}
bool WindowsServices::parallel_service(GuestAddress address) const noexcept {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_PARALLEL_SERVICES");return !setting || std::string_view(setting)!="0";}();
    if(!enabled || x86_ || !runtime_.host_clock || !trace_services_.empty() || startup_profile::enabled() || runtime_.forced_exit_code())return false;
    const auto symbol=name(address);
    if(parallel_srw_enabled() && srw_service(symbol))return true;
    return symbol=="TlsGetValue" || symbol=="TlsGetValue2" || symbol=="TlsSetValue" ||
        symbol=="FlsGetValue" || symbol=="FlsGetValue2" || symbol=="FlsSetValue" ||
        symbol=="InitOnceBeginInitialize" || symbol=="InitOnceExecuteOnce" ||
        symbol=="WakeConditionVariable" || symbol=="WakeAllConditionVariable" || symbol=="SwitchToThread" ||
        symbol=="_control87" || symbol=="_controlfp" ||
        symbol=="GetLastError" || symbol=="SetLastError" || symbol=="GetCurrentThreadId" ||
        symbol=="QueryPerformanceCounter" || symbol=="QueryPerformanceFrequency" ||
        symbol=="EnterCriticalSection" || symbol=="TryEnterCriticalSection" || symbol=="LeaveCriticalSection";
}

bool WindowsServices::invoke_parallel_service(GuestState& state,GuestMemory& memory) {
    bool retry=false;return invoke_parallel_service_impl(state,memory,retry);
}
#ifdef X64ARM_HOST_SERVICES_RESIDENT_PARALLEL
ResidentServiceProgress WindowsServices::resident_parallel_service(GuestState& state,GuestMemory& memory) {
    bool retry=false;
    if(invoke_parallel_service_impl(state,memory,retry))return ResidentServiceProgress::Completed;
    return retry?ResidentServiceProgress::Retry:ResidentServiceProgress::Coordinator;
}
#ifdef X64ARM_HOST_SERVICES_RESIDENT_ACTIVE_CONTEXTS
ResidentServiceProgress WindowsServices::resident_parallel_service(GuestState& state,GuestMemory& memory,
                                                                   std::span<GuestState* const> active) {
    bool retry=false;
    if(invoke_parallel_service_impl(state,memory,retry,active))return ResidentServiceProgress::Completed;
    return retry?ResidentServiceProgress::Retry:ResidentServiceProgress::Coordinator;
}
#endif
#endif
bool WindowsServices::invoke_parallel_service_impl(GuestState& state,GuestMemory& memory,bool& retry,
                                                  std::span<GuestState* const> active) {
    if(!parallel_service(state.rip))return false;
    const auto symbol=name(state.rip);
    struct CountScope {
        WindowsServices& self;std::uint64_t rip;std::chrono::steady_clock::time_point start;
        ~CountScope(){if(!self.service_counts_enabled_)return;
            const auto ns=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());
            std::lock_guard lock(*self.worker_service_counts_mutex_);auto& c=self.worker_service_counts_[rip];++c.calls;c.ns+=ns;}
    } count_scope{*this,state.rip.value,service_counts_enabled_?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{}};
    // worker 身分由交給引擎的 context 決定；客體更動 GS 不得冒用另一執行緒。
    auto key=threads_.current;
    {
        std::unique_lock<std::mutex> lock;const auto& index=resident_thread_index(lock);
        if(const auto context=index.contexts.find(&state);context!=index.contexts.end())key=context->second;
    }
    auto found=threads_.items.find(key);
    if(found==threads_.items.end() || found->second.teb!=state.gs_base)return false;
    auto& thread=found->second;const auto tid=found->first;const bool current=tid==threads_.current;
    // 所有存取先驗證。native_window 排除 COW、可執行寫入、外部 backing 及跨映射；
    // 失敗留給協調者處理，不在 worker 內改變映射或吞掉客體例外。
    const auto pointer=[&](std::uint64_t address,unsigned size,unsigned access)->void* {
        if((address&(size-1U)) || address>UINT64_MAX-size)return nullptr;
        // 一般 WindowsCall 在寫入後讀取返回位址；輸出若與返回槽重疊，交回完整路徑。
        const auto sp=state.reg(Gpr::Rsp);
        if((access&2U) && (sp>UINT64_MAX-8 || (address<sp+8 && address+size>sp)))return nullptr;
        // 對齊的 4／8 byte 存取不跨客體頁；沿用相同原生權限，避免每次 API
        // 都經 native_pointer 重寫全體 worker 共用的 JIT 頁面快取與 ready 旗標。
        const auto window=memory.native_window(GuestAddress{address},access);
        return window.data && window.after>=size?window.data:nullptr;
    };
    const auto load=[](const void* source,unsigned size){std::uint64_t value=0;std::memcpy(&value,source,size);return value;};
    const auto put=[](void* destination,std::uint64_t value,unsigned size){std::memcpy(destination,&value,size);};
    auto* stack=pointer(state.reg(Gpr::Rsp),8,1);if(!stack || state.reg(Gpr::Rsp)>UINT64_MAX-8)return false;
    const auto target=load(stack,8),first=state.reg(Gpr::Rcx),second=state.reg(Gpr::Rdx);
    std::uint64_t result=0;
    // 與一般服務一樣，服務邊界保持客體可見的記憶體順序；不依賴宿主是否已授權 TSO。
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if(symbol=="GetLastError" || symbol=="SetLastError"){
        auto* error=pointer(thread.teb+0x68,4,symbol=="GetLastError"?1U:2U);if(!error)return false;
        if(symbol=="GetLastError")result=current?last_error_:load(error,4);
        else {put(error,static_cast<std::uint32_t>(first),4);if(current)last_error_=static_cast<std::uint32_t>(first);}
    }else if(symbol=="TlsGetValue" || symbol=="TlsGetValue2" || symbol=="TlsSetValue"){
        const auto index=static_cast<std::uint32_t>(first);const bool reading=symbol!="TlsSetValue";
        if(index>=runtime_.tls_used.size() || (!reading && !runtime_.tls_used[index]) || !runtime_.image.base)return false;
        const auto offset=index<64?0x1480+index*8U:0x2000+(index-64)*8U;
        auto* slot=pointer(thread.teb+offset,8,reading?1U:2U);if(!slot)return false;
        auto* error=symbol=="TlsGetValue"?pointer(thread.teb+0x68,4,2):nullptr;
        if(symbol=="TlsGetValue" && !error)return false;
        if(reading){result=load(slot,8);if(error){put(error,0,4);if(current)last_error_=0;}}
        else {put(slot,second,8);(current?runtime_.tls_values:thread.tls_values)[index]=second;result=1;}
    }else if(symbol=="FlsGetValue" || symbol=="FlsGetValue2" || symbol=="FlsSetValue"){
        const auto index=static_cast<std::uint32_t>(first);const bool reading=symbol!="FlsSetValue";
        if(index>=runtime_.fls_used.size() || !runtime_.fls_used[index])return false;
        auto* error=symbol=="FlsGetValue"?pointer(thread.teb+0x68,4,2):nullptr;
        if(symbol=="FlsGetValue" && !error)return false;
        auto* mirror=!reading && fls_mirror_?pointer(thread.teb+0x1840+std::uint64_t{index}*8,8,2):nullptr;
        if(!reading && fls_mirror_ && !mirror)return false;
        auto& values=current?runtime_.fls_values:thread.fls_values;
        if(reading){result=values[index];if(error){put(error,0,4);if(current)last_error_=0;}}
        else {values[index]=second;if(mirror)put(mirror,second,8);result=1;}
    }else if(symbol=="InitOnceBeginInitialize" || symbol=="InitOnceExecuteOnce"){
        const bool begin=symbol=="InitOnceBeginInitialize";
        if(begin && (static_cast<std::uint32_t>(second)&~1U))return false;
        const auto sp=state.reg(Gpr::Rsp);
        if(sp%16!=8 || sp>UINT64_MAX-40)return false;
        const auto home=memory.native_window(GuestAddress{sp+8},2);
        if(!home.data || home.after<32)return false;
        auto* object=pointer(first,8,3);if(!object)return false;
        const auto initialization=runtime_.once_initializations.find(first);
        if(initialization==runtime_.once_initializations.end())return false;
        const auto& once=initialization->second;
        // once 表只在全體交出狀態後修改；完成查詢不需要停止其他 worker。
        // 仍核對客體值，已歸零／重建的同址物件不得沿用前一個 context。
        if(!once.complete || once.owner || load(object,8)!=(once.context|UINT64_C(2)))return false;
        auto* pending=begin?pointer(state.reg(Gpr::R8),4,2):nullptr;
        const auto output=state.reg(Gpr::R9);
        auto* context=output?pointer(output,8,2):nullptr;
        if((begin && !pending) || (output && !context))return false;
        // 完成所有資格檢查後才按原 provider 順序寫入，保留輸出別名語義。
        if(pending)put(pending,0,4);
        if(context)put(context,once.context,8);
        // 此 context 已由 worker 執行；只清除原流程會清除的自身等待紀錄。
        auto& wait=thread.wait;
        if(!begin || (wait.kind==ThreadWaitKind::Once && !wait.handles.empty() && wait.handles[0]==first))wait={};
        result=1;
    }else if(symbol=="WakeConditionVariable" || symbol=="WakeAllConditionVariable"){
        if(!pointer(first,8,2))return false;
        // 等待集合在 epoch 內固定。沒有尚待喚醒者時，通知已完整完成；有等待者
        // 才交協調者更新等待狀態、重新取得鎖及排程，避免無效通知停止全體 worker。
        for(const auto& [id,other]:threads_.items){
            (void)id;
            if(other.status==ThreadStatus::Waiting && other.wait.kind==ThreadWaitKind::Condition &&
               other.wait.handles[0]==first && !other.wait.signaled)return false;
        }
    }else if(symbol=="SwitchToThread"){
        if(active.empty() || state.processor_id>=64)return false;
        for(const auto& [id,other]:threads_.items){
            if(id==tid || other.status!=ThreadStatus::Runnable || other.suspended ||
               !(other.affinity&(UINT64_C(1)<<state.processor_id)))continue;
            // 主 context 位於呼叫端，其餘位於執行緒表。已取得別的 worker 者是
            // running，不能當成目前處理器尚可接手的 ready 執行緒。
            const bool running=id==threads_.current ||
                std::find(active.begin(),active.end(),&other.context)!=active.end();
            if(!running)return false;
        }
        result=0; // 沒有可接手者；依 Windows 契約不做排程切換。
    }else if(symbol=="_control87" || symbol=="_controlfp"){
        // x64 合法控制遮罩只修改呼叫者 MXCSR，沒有 errno、回呼或輸出指標。
        // 非法遮罩沿既有完整例外路徑；有效呼叫共用原數學 provider。
        if(static_cast<std::uint32_t>(second)&0x70000U)return false;
        const auto sp=state.reg(Gpr::Rsp);
        if(sp%16!=8 || sp>UINT64_MAX-40)return false;
        const auto home=memory.native_window(GuestAddress{sp+8},2);
        if(!home.data || home.after<32)return false;
        if(invoke_math(state,memory,{"crt",symbol,2}))throw std::logic_error("浮點控制服務意外終止客體");
        std::atomic_thread_fence(std::memory_order_seq_cst);return true;
    }else if(srw_service(symbol)){
        // 字組編碼與協調者路徑／客體常式相同：bit0 獨占、bit1 宿主有等待者、bit2 起為共用持有數。
        // 只以原子 CAS 改字組，不碰執行緒表；與協調者的 CAS 迴圈及客體 lock cmpxchg 互相線性化。
        auto* word=pointer(first,8,3);if(!word)return false;
        std::atomic_ref<std::uint64_t> lock(*static_cast<std::uint64_t*>(word));
        const bool shared=symbol.ends_with("Shared");
        auto value=lock.load(std::memory_order_acquire);
        if(symbol.starts_with("Release")){
            for(;;){
                // 有等待者時須由原服務喚醒；未持有則交回原路徑回報錯誤。
                if((value&2) || (shared?!(value>>2):!(value&1))){detail::record_srw(detail::SrwEvent::WorkerDefer);return false;}
                if(lock.compare_exchange_weak(value,shared?value-4:value&~UINT64_C(1),std::memory_order_acq_rel,std::memory_order_acquire))break;
            }
            detail::record_srw(detail::SrwEvent::WorkerRelease);
        }else{
            // 取得條件與原服務相同（bit1 存在時仍可插隊取得）；Try 不等待，競爭即回 FALSE。
            const bool attempt=symbol.starts_with("Try");const bool spin=!attempt && active.size()>1;
            for(unsigned round=0;;){
                if(!(value&1) && (shared || !(value>>2))){
                    if(shared && (value>>2)==UINT64_C(0x3fffffffffffffff)){detail::record_srw(detail::SrwEvent::WorkerDefer);return false;}
                    if(lock.compare_exchange_weak(value,shared?value+4:value|1,std::memory_order_acq_rel,std::memory_order_acquire)){
                        result=1;detail::record_srw(round?detail::SrwEvent::WorkerSpinAcquire:detail::SrwEvent::WorkerAcquire);break;
                    }
                    continue;
                }
                if(attempt){detail::record_srw(detail::SrwEvent::WorkerTryFail);break;}
                if(!spin || ++round>srw_spin_limit){detail::record_srw(detail::SrwEvent::WorkerDefer);return false;}
                spin_pause();value=lock.load(std::memory_order_acquire);
            }
        }
    }else if(symbol=="GetCurrentThreadId")result=thread_identifier(tid);
else if(symbol=="QueryPerformanceCounter" || symbol=="QueryPerformanceFrequency"){
        auto* output=pointer(first,8,2);if(!output)return false;
        // host-clock 直接讀硬體計數器，與 machine clock 同一時間軸；不在 worker 更新共享時間頁。
        const auto value=symbol=="QueryPerformanceFrequency"?UINT64_C(10000000):detail::monotonic_ticks();
        put(output,value,8);result=1;
    }else {
        const auto section_it=runtime_.critical_sections.find(first);if(section_it==runtime_.critical_sections.end())return false;
        const auto window=memory.native_window(GuestAddress{first},2);
        if(!window.data || window.after<40)return false;
        auto* lock_count=pointer(first+8,4,2);auto* recursion=pointer(first+12,4,2);auto* owner=pointer(first+16,8,2);
        if(first>UINT64_MAX-40 || !lock_count || !recursion || !owner)return false;
        auto& section=section_it->second;std::atomic_ref guard(section.worker_guard);std::uint32_t expected=0;
        // 僅回報等待，不持鎖進入信箱。舊 JIT 仍交回協調者；常駐 worker 可等待通知。
        if(!guard.compare_exchange_strong(expected,1,std::memory_order_acquire)){retry=true;return false;}
        struct Release {std::atomic_ref<std::uint32_t> lock;~Release(){lock.store(0,std::memory_order_release);}} release{guard};
        if(symbol=="LeaveCriticalSection"){
            if(section.owner!=tid || !section.recursion)return false;
            if(section.recursion==1)for(const auto& [id,other]:threads_.items){
                (void)id;if(other.status!=ThreadStatus::Waiting)continue;
                const auto& wait=other.wait;
                const bool critical=wait.kind==ThreadWaitKind::Critical || wait.kind==ThreadWaitKind::CriticalRetry ||
                    (wait.kind==ThreadWaitKind::CrtLockRetry && wait.critical);
                const bool condition=wait.kind==ThreadWaitKind::Condition && wait.critical &&
                    (wait.signaled || wait.deadline!=UINT64_MAX);
                if((critical && !wait.handles.empty() && wait.handles[0]==first) ||
                   (condition && wait.handles.size()>1 && wait.handles[1]==first))
                    // 虛擬等待者沒有 active worker，服務進度通知無法喚醒它。
                    // 尚未釋放鎖；由原 provider 完成釋放及 wake_threads，避免漏掉排程事件。
                    return false;
            }
            if(!--section.recursion)section.owner=0;
        }else if(!section.owner || section.owner==tid){
            if(section.recursion==UINT32_MAX)return false;
            section.owner=tid;++section.recursion;result=1;
        }else if(symbol!="TryEnterCriticalSection"){
            // 只有持有者正取得 worker 時，等待服務進度才可能完成。
            // 持有者尚未排程時交回共用等待流程，不能讓等待者佔著 worker
            // 睡到下一個排程期限，反過來阻止持有者執行 LeaveCriticalSection。
            const auto holder=threads_.items.find(section.owner);
            retry=holder!=threads_.items.end() && (holder->first==threads_.current ||
                std::find(active.begin(),active.end(),&holder->second.context)!=active.end());
            return false;
        }
        put(lock_count,section.recursion-1U,4);put(recursion,section.recursion,4);put(owner,thread_identifier(section.owner),8);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    state.rip=GuestAddress{target};state.reg(Gpr::Rsp)+=8;state.reg(Gpr::Rax)=result;
    return true;
}
#endif
}
