// ntdll 同步轉接家族：RTL_RESOURCE 讀寫鎖（RtlInitializeResource、RtlAcquireResourceShared／Exclusive、RtlReleaseResource、
// RtlDeleteResource、RtlDumpResource）、PEB 鎖（RtlAcquirePebLock／RtlReleasePebLock，鎖存於 PEB.FastPebLock）、同步屏障
// （RtlInitBarrier／RtlBarrier／RtlDeleteBarrier，與 kernel32 SynchronizationBarrier 同一實作）與 RtlWaitOnAddress（與 WaitOnAddress 共用）。
// RTL_RESOURCE 依 Windows 欄位版面：臨界區段保護計數，共享與獨占等待者各在自己的號誌上等待；NumberOfActive 正值為共享數、負值為獨占重入深度。
// 會阻塞的步驟（進入臨界區段、等待號誌、屏障與位址等待）經協程呼叫既有服務。
#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <cstdio>
#include <string>

namespace x64arm {
std::span<const WindowsServiceDescriptor> ntdll_sync_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ntdll.dll","RtlInitializeResource",1},{"ntdll.dll","RtlDeleteResource",1},{"ntdll.dll","RtlAcquireResourceShared",2,0,0,0x2},
        {"ntdll.dll","RtlAcquireResourceExclusive",2,0,0,0x2},{"ntdll.dll","RtlReleaseResource",1},{"ntdll.dll","RtlDumpResource",1},
        {"ntdll.dll","RtlAcquirePebLock",0},{"ntdll.dll","RtlReleasePebLock",0},
        {"ntdll.dll","RtlInitBarrier",3,0,0,0x6},{"ntdll.dll","RtlBarrier",2,0,0,0x2},{"ntdll.dll","RtlDeleteBarrier",1},
        {"ntdll.dll","RtlWaitOnAddress",4}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_ntdll_sync(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    {
        // 不會阻塞的入口直接完成：屏障建立／刪除與 RTL_RESOURCE 的初始化、刪除、傾印。
        const WindowsCall call(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        const auto finish=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
        const auto nested=[&](std::string_view dll,std::string_view symbol,std::initializer_list<std::uint64_t> arguments){return nested_service(state,memory,dll,symbol,arguments);};
        if(name=="RtlInitBarrier")return finish(nested("kernel32.dll","InitializeSynchronizationBarrier",{call(0),static_cast<std::uint32_t>(call(1)),static_cast<std::uint32_t>(call(2))})?0U:0xc000000dU);
        if(name=="RtlDeleteBarrier"){(void)nested("kernel32.dll","DeleteSynchronizationBarrier",{call(0)});return finish(0);}
        // RtlBarrier 與 EnterSynchronizationBarrier 相同（BOOL→BOOLEAN）；PEB 鎖就是 PEB.FastPebLock（x64 0x38、x86 0x1c）上的臨界區段，
        // 尚未建立時以程序 heap 上的臨界區段補上。三者都以轉接回呼交給既有實作，等待與換手沿用其排程。
        const auto forward=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments){
            start_callbacks(state,memory,{{resolve(dll,symbol).value,std::move(arguments)}},Continuation::ReturnCallback);return std::optional<std::uint32_t>{};};
        if(name=="RtlBarrier")return forward("kernel32.dll","EnterSynchronizationBarrier",{call(0),static_cast<std::uint32_t>(call(1))});
        if(name=="RtlAcquirePebLock" || name=="RtlReleasePebLock"){
            const auto slot=peb_base_.value+(x86_?0x1cU:0x38U);auto lock=memory.load(GuestAddress{slot},bits);
            if(!lock){lock=allocate_heap(memory,x86_?24:40);if(!lock)throw std::bad_alloc{};(void)nested("ntdll.dll","RtlInitializeCriticalSection",{lock});memory.store(GuestAddress{slot},bits,lock);}
            return forward("ntdll.dll",name=="RtlAcquirePebLock"?"RtlEnterCriticalSection":"RtlLeaveCriticalSection",{lock});
        }
        const auto resource=call(0);
        const unsigned cs=x86_?24U:40U,shared_semaphore=cs,waiting_shared=cs+unit,exclusive_semaphore=waiting_shared+(x86_?4U:8U);
        const unsigned waiting_exclusive=exclusive_semaphore+unit,active=waiting_exclusive+4,owner=active+4,debug=owner+unit+(x86_?4U:8U);
        if(name=="RtlInitializeResource"){
            if(!resource)return finish(0);
            memory.fill(GuestAddress{resource},0,debug+unit);(void)nested("ntdll.dll","RtlInitializeCriticalSection",{resource});
            // 共享與獨占等待者各用一個號誌（初值 0），handle 寫入 SharedSemaphore／ExclusiveSemaphore 欄位。
            for(const auto at:{shared_semaphore,exclusive_semaphore}){
                const auto handle=nested("kernel32.dll","CreateSemaphoreW",{0,0,65535,0});if(!handle)throw CpuFault(0xc0000017U,"RtlInitializeResource 無法建立號誌");
                memory.store(GuestAddress{resource+at},bits,handle);
            }
            return finish(0);
        }
        if(name=="RtlDeleteResource"){
            if(!resource)return finish(0);
            (void)nested("ntdll.dll","RtlDeleteCriticalSection",{resource});
            for(const auto at:{shared_semaphore,exclusive_semaphore})if(const auto handle=memory.load(GuestAddress{resource+at},bits))(void)nested("kernel32.dll","CloseHandle",{handle});
            memory.fill(GuestAddress{resource},0,debug+unit);return finish(0);
        }
        if(name=="RtlDumpResource"){
            if(!resource)return finish(0);
            char address[24];std::snprintf(address,sizeof address,"%llx",static_cast<unsigned long long>(resource));
            const auto text="RtlDumpResource(0x"+std::string(address)+"): active count = "+std::to_string(static_cast<std::int32_t>(memory.load(GuestAddress{resource+active},32)))+
                ", waiting shared = "+std::to_string(memory.load(GuestAddress{resource+waiting_shared},32))+", waiting exclusive = "+std::to_string(memory.load(GuestAddress{resource+waiting_exclusive},32))+
                ", owner = "+std::to_string(memory.load(GuestAddress{resource+owner},bits))+"\n";
            GuestScratch scratch(*this,memory,state);const auto at=scratch.allocate(text.size()+1);
            if(at){memory.write(GuestAddress{at},std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()+1));(void)nested("kernel32.dll","OutputDebugStringA",{at});}
            return finish(0);
        }
    }
    return nested_continuation(state,memory,"ntdll 同步",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto name=service.symbol;
        const auto entry=[&](std::string_view dll,std::string_view symbol){return resolve(dll,symbol).value;};
        const auto nested=[&](std::string_view dll,std::string_view symbol,std::initializer_list<std::uint64_t> arguments){return nested_service(s,m,dll,symbol,arguments);};
        if(name=="RtlWaitOnAddress"){
            // (Address, CompareAddress, AddressSize, PLARGE_INTEGER Timeout)：負值為相對 100 ns、正值為絕對系統時間、NULL 為無限等待。
            std::uint32_t milliseconds=UINT32_MAX;
            if(const auto timeout=call(3)){
                const auto value=static_cast<std::int64_t>(m.load(GuestAddress{timeout},64));
                std::uint64_t ticks=0;if(value<0)ticks=static_cast<std::uint64_t>(-value);else {const auto now=runtime_.system_filetime();ticks=static_cast<std::uint64_t>(value)>now?static_cast<std::uint64_t>(value)-now:0;}
                milliseconds=static_cast<std::uint32_t>(std::min<std::uint64_t>(0xfffffffeU,(ticks+9999)/10000));
            }
            // 最後錯誤直接讀寫 TEB.LastErrorValue（x86 0x34、x64 0x68）：NTSTATUS 介面不改變呼叫端的最後錯誤。
            const auto last_error=(x86_?s.fs_base:s.gs_base)+(x86_?0x34U:0x68U);const auto saved=m.load(GuestAddress{last_error},32);
            const auto woke=co_await service_callback::call(entry("kernel32.dll","WaitOnAddress"),{call(0),call(1),call(2),milliseconds});
            const auto teb_error=(x86_?s.fs_base:s.gs_base)+(x86_?0x34U:0x68U);const auto error=static_cast<std::uint32_t>(m.load(GuestAddress{teb_error},32));
            m.store(GuestAddress{teb_error},32,saved);last_error_=static_cast<std::uint32_t>(saved);
            co_return finish(woke?0U:error==1460?0x102U:0xc000000dU);
        }
        // ---- RTL_RESOURCE ----
        const auto resource=call(0);if(!resource)co_return finish(0);
        const unsigned cs=x86_?24U:40U,shared_semaphore=cs,waiting_shared=cs+unit,exclusive_semaphore=waiting_shared+(x86_?4U:8U);
        const unsigned waiting_exclusive=exclusive_semaphore+unit,active=waiting_exclusive+4,owner=active+4;
        const auto u32=[&](unsigned at){return static_cast<std::uint32_t>(m.load(GuestAddress{resource+at},32));};
        const auto set32=[&](unsigned at,std::uint64_t v){m.store(GuestAddress{resource+at},32,v);};
        const auto ptr=[&](unsigned at){return m.load(GuestAddress{resource+at},bits);};
        const auto thread=nested("kernel32.dll","GetCurrentThreadId",{});
        const auto enter=[&]()->service_callback::Task<std::uint32_t>{co_return static_cast<std::uint32_t>(co_await service_callback::call(entry("ntdll.dll","RtlEnterCriticalSection"),{resource}));};
        const auto leave=[&]()->service_callback::Task<std::uint32_t>{co_return static_cast<std::uint32_t>(co_await service_callback::call(entry("ntdll.dll","RtlLeaveCriticalSection"),{resource}));};
        if(name=="RtlReleaseResource"){
            (void)co_await enter();
            auto count=static_cast<std::int32_t>(u32(active));
            if(count>0){
                set32(active,static_cast<std::uint32_t>(--count));
                if(!count && u32(waiting_exclusive)){set32(waiting_exclusive,u32(waiting_exclusive)-1);(void)nested("kernel32.dll","ReleaseSemaphore",{ptr(exclusive_semaphore),1,0});}
            }else if(count<0){
                set32(active,static_cast<std::uint32_t>(++count));
                if(!count){
                    m.store(GuestAddress{resource+owner},bits,0);
                    if(u32(waiting_exclusive)){set32(waiting_exclusive,u32(waiting_exclusive)-1);(void)nested("kernel32.dll","ReleaseSemaphore",{ptr(exclusive_semaphore),1,0});}
                    else if(const auto waiting=u32(waiting_shared)){set32(waiting_shared,0);(void)nested("kernel32.dll","ReleaseSemaphore",{ptr(shared_semaphore),waiting,0});}
                }
            }
            (void)co_await leave();co_return finish(0);
        }
        const bool exclusive=name=="RtlAcquireResourceExclusive",wait=static_cast<std::uint8_t>(call(1))!=0;
        while(true){
            (void)co_await enter();
            const auto count=static_cast<std::int32_t>(u32(active));
            // 目前執行緒已獨占持有時，共享與獨占取得都視為重入。
            if(count<0 && ptr(owner)==thread){set32(active,static_cast<std::uint32_t>(count-1));(void)co_await leave();co_return finish(1);}
            if(exclusive ? count==0 : (count>=0 && !u32(waiting_exclusive))){
                if(exclusive){set32(active,static_cast<std::uint32_t>(-1));m.store(GuestAddress{resource+owner},bits,thread);}else set32(active,static_cast<std::uint32_t>(count+1));
                (void)co_await leave();co_return finish(1);
            }
            if(!wait){(void)co_await leave();co_return finish(0);}
            const auto at=exclusive?waiting_exclusive:waiting_shared;set32(at,u32(at)+1);
            (void)co_await leave();
            (void)co_await service_callback::call(entry("ntdll.dll","NtWaitForSingleObject"),{ptr(exclusive?exclusive_semaphore:shared_semaphore),0,0});
        }
    });
}
}
