#include "x64arm/windows_services.hpp"
#include "x64arm/startup_profile.hpp"
#include "monotonic_clock.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace x64arm {
#ifdef X64ARM_HOST_SERVICES_PARALLEL_SERVICE
bool WindowsServices::parallel_service(GuestAddress address) const noexcept {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_PARALLEL_SERVICES");return !setting || std::string_view(setting)!="0";}();
    if(!enabled || x86_ || !runtime_.host_clock || !trace_services_.empty() || startup_profile::enabled() || runtime_.forced_exit_code())return false;
    const auto symbol=name(address);
    return symbol=="TlsGetValue" || symbol=="TlsGetValue2" || symbol=="TlsSetValue" ||
        symbol=="GetLastError" || symbol=="SetLastError" || symbol=="GetCurrentThreadId" ||
        symbol=="QueryPerformanceCounter" || symbol=="QueryPerformanceFrequency" ||
        symbol=="EnterCriticalSection" || symbol=="TryEnterCriticalSection" || symbol=="LeaveCriticalSection";
}

bool WindowsServices::invoke_parallel_service(GuestState& state,GuestMemory& memory) {
    if(!parallel_service(state.rip))return false;
    const auto symbol=name(state.rip);
    // worker 身分由交給引擎的 context 決定；客體更動 GS 不得冒用另一執行緒。
    auto found=threads_.items.find(threads_.current);
    for(auto it=threads_.items.begin();it!=threads_.items.end();++it)if(&it->second.context==&state){found=it;break;}
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
        // 不等待其他 worker：一次取鎖失敗就返回協調者，避免批次屏障與客體鎖互相等待。
        if(!guard.compare_exchange_strong(expected,1,std::memory_order_acquire))return false;
        struct Release {std::atomic_ref<std::uint32_t> lock;~Release(){lock.store(0,std::memory_order_release);}} release{guard};
        if(symbol=="LeaveCriticalSection"){
            if(section.owner!=tid || !section.recursion)return false;
            if(!--section.recursion)section.owner=0;
        }else if(!section.owner || section.owner==tid){
            if(section.recursion==UINT32_MAX)return false;
            section.owner=tid;++section.recursion;result=1;
        }else if(symbol!="TryEnterCriticalSection")return false;
        put(lock_count,section.recursion-1U,4);put(recursion,section.recursion,4);put(owner,thread_identifier(section.owner),8);
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    state.rip=GuestAddress{target};state.reg(Gpr::Rsp)+=8;state.reg(Gpr::Rax)=result;
    return true;
}
#endif
}
