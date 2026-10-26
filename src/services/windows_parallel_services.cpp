#include <shared_mutex>
#include "x64arm/windows_services.hpp"
#include <chrono>
#include "x64arm/startup_profile.hpp"
#include "x64arm/entry_runtime_profile.hpp"
#include "monotonic_clock.hpp"
#include "srw_trace.hpp"
#include "guest_heap_layout.hpp"
#include "guest_tls.hpp"
#include "parallel_mapping_lock.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <thread>

namespace x64arm {
#ifdef X64ARM_HOST_SERVICES_RESIDENT_HEAP
ResidentHeapOperation WindowsServices::resident_heap_recipe(GuestAddress address) const noexcept {
    if(!initialized_ || !x86_ || !runtime_.host_clock || !trace_services_.empty() ||
       startup_profile::enabled() || service_counts_enabled_)return ResidentHeapOperation::None;
    const auto symbol=name(address);
    if(symbol=="HeapAlloc")return ResidentHeapOperation::Allocate;
    if(symbol=="HeapFree")return ResidentHeapOperation::Free;
    if(symbol=="HeapSize")return ResidentHeapOperation::Size;
    return ResidentHeapOperation::None;
}
const ResidentHeap* WindowsServices::borrow_resident_heap(GuestMemory& memory) noexcept {
    using namespace detail::guest_heap_layout;
    auto& cache=resident_heap_cache_;
    static_assert(resident_heap_chunk_size==guest_heap_chunk && resident_heap_header_size==guest_heap_header &&
                  resident_heap_small_limit==guest_heap_small_limit && resident_heap_small_class_count==guest_heap_class(guest_heap_small_limit) &&
                  resident_heap_small_unit==guest_heap_class_size(1) &&
                  resident_heap_large_unit==guest_heap_class_size(257)-guest_heap_small_limit);
    constexpr auto max_class=static_cast<std::uint32_t>(guest_heap_class(guest_heap_limit+guest_heap_aligned_header));
    constexpr auto process_heads=max_class+1;
    static_assert(guest_heap_heads+process_heads*sizeof(std::uint64_t)<=GuestMemory::guest_page_size);
    // 只能在協調者已停止全部 worker 時呼叫；沒有正常熱碼時間／配額輪詢。
    if(!initialized_ || !x86_ || !guest_heap_chunked_ || !guest_heap_state_.value ||
       !runtime_.host_clock || !trace_services_.empty() || startup_profile::enabled() || service_counts_enabled_){
        cache.descriptor={};return nullptr;
    }
    const auto identity=memory.identity(),generation=memory.native_borrow_generation();
    const bool mapping_changed=identity!=cache.memory_identity || generation!=cache.memory_generation;
    const bool chunks_changed=mapping_changed || guest_heap_chunk_generation_!=cache.chunk_generation;
    const bool entries_changed=mapping_changed || guest_heap_topology_generation_!=cache.topology_generation;
    if(!chunks_changed && !entries_changed && cache.descriptor.guard)return &cache.descriptor;
    // 失敗不能把部分重建的表當成舊有效借用；固定 descriptor 地址供所有 DispatchIndex 共用。
    cache.descriptor={};
    const auto state=memory.native_private_span(guest_heap_state_,GuestMemory::guest_page_size,3);
    if(state.empty() || (reinterpret_cast<std::uintptr_t>(state.data())&7U))return nullptr;
    try{
        if(!cache.chunks)cache.chunks=std::make_unique<std::array<std::uint8_t*,resident_heap_chunk_count>>();
        if(chunks_changed){
            cache.chunks->fill(nullptr);
            for(std::size_t word=0;word<guest_heap_chunk_bits_.size();++word)
                for(auto bits=guest_heap_chunk_bits_[word];bits;bits&=bits-1U){
                    const auto bit=static_cast<unsigned>(std::countr_zero(bits));
                    const auto slot=word*64+bit;
                    const auto bytes=memory.native_private_span(GuestAddress{static_cast<std::uint64_t>(slot)<<resident_heap_chunk_bits},guest_heap_chunk,3);
                    if(!bytes.empty())(*cache.chunks)[slot]=bytes.data();
                }
        }
        if(entries_changed){
            cache.entries.clear();cache.entries.reserve(guest_private_heaps_.size()+1);
            const auto ready=[&](std::uint64_t heap){const auto lock=runtime_.heap_locks.find(heap);
                return lock==runtime_.heap_locks.end() || !lock->second.owner?resident_heap_entry_ready:0U;};
            cache.entries.push_back({process_heap,reinterpret_cast<std::uint64_t*>(state.data()+guest_heap_heads),process_heads,ready(process_heap)});
            for(auto& [heap,lists]:guest_private_heaps_){
                if(lists.heads.empty() || !runtime_.private_heaps.contains(heap))continue;
                if(lists.heads.size()>UINT32_MAX)return nullptr;
                cache.entries.push_back({heap,lists.heads.data(),static_cast<std::uint32_t>(lists.heads.size()),ready(heap)});
            }
        }
        if(cache.entries.empty() || cache.entries.size()>UINT32_MAX)return nullptr;
        cache.descriptor={reinterpret_cast<std::uint32_t*>(state.data()),&guest_heap_mutations_,cache.chunks->data(),
            cache.entries.data(),process_heap,guest_heap_magic,resident_heap_chunk_count,
            static_cast<std::uint32_t>(cache.entries.size()),
            static_cast<std::uint32_t>(guest_heap_small_limit+guest_heap_aligned_header),max_class,
            ++cache.borrow_generation,guest_heap_state_.value,guest_heap_state_.value+GuestMemory::guest_page_size};
        cache.memory_identity=identity;cache.memory_generation=generation;
        cache.chunk_generation=guest_heap_chunk_generation_;cache.topology_generation=guest_heap_topology_generation_;
        return &cache.descriptor;
    }catch(const std::bad_alloc&){
        // 只是可選原生借用的資源不足；原 provider 仍完整處理 API 的配置與錯誤。
        cache.descriptor={};return nullptr;
    }
}
#endif
#ifdef X64ARM_HOST_SERVICES_CLOCK_RECIPE
ResidentClockRecipe WindowsServices::resident_clock_recipe(GuestAddress address) const noexcept {
    if(!initialized_ || !runtime_.host_clock || !trace_services_.empty() || startup_profile::enabled())
        return ResidentClockRecipe::None;
    const auto symbol=name(address);
    if(symbol=="timeGetTime" || symbol=="GetTickCount")return ResidentClockRecipe::Milliseconds32;
    if(symbol=="GetTickCount64")return ResidentClockRecipe::Milliseconds64;
    // QueryPerformanceCounter（Win64）：與 worker 服務同一條 100 ns 硬體時間軸；X64ARM_AOT_QPC_LOWERING=0 保留原服務。
    static const bool qpc=[] {const auto* value=std::getenv("X64ARM_AOT_QPC_LOWERING");return !value || std::string_view(value)!="0";}();
    if(qpc && !x86_ && symbol=="QueryPerformanceCounter")return ResidentClockRecipe::PerformanceCounter;
    return ResidentClockRecipe::None;
}
#endif
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
// MSVC std::mutex（msvcp140 的 _Mtx_lock／_Mtx_unlock／_Mtx_trylock）：內含同一編碼的 SRW 字組，未競爭的取得／釋放
// 與遞迴可在 worker 完成；原本每次都是協調者服務加上一次客體回呼（兩次全停）。與 SRW 同一開關。
// MSVCP 的 _Thrd_sleep／_Thrd_yield（x64）只把引數換成 kernel32 Sleep／SwitchToThread 的引數後尾跳（見 msvcp_sync_handlers），
// 真正的等待／讓出由尾跳目標的服務處理；原本這一步在協調者執行，平行模式每次多一次全停（電馭判客 Benchmark 後段約 11 萬次）。
// X64ARM_PARALLEL_THRD_TAIL=0 關閉。
bool thrd_tail_service(std::string_view symbol) noexcept {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_PARALLEL_THRD_TAIL");return !setting || std::string_view(setting)!="0";}();
    return enabled && (symbol=="_Thrd_sleep" || symbol=="_Thrd_yield");
}
bool msvcp_mutex_service(std::string_view symbol) noexcept {
    return symbol=="_Mtx_lock" || symbol=="_Mtx_unlock" || symbol=="_Mtx_trylock";
}
bool tick_clock_service(std::string_view symbol) noexcept {
    return symbol=="timeGetTime" || symbol=="GetTickCount" || symbol=="GetTickCount64";
}
bool parallel_tick_clock_enabled() noexcept {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_PARALLEL_TICK_CLOCK");return !value || std::string_view(value)!="0";}();
    return enabled;
}
// 同一組執行緒內服務共用 Win32 stdcall 框架；配置擴充、回呼、等待與映射
// 仍交回協調者。Win64 沿用暫存器 ABI，不因擴充 Win32 改變原有資格。
std::optional<unsigned> context_service_arguments(std::string_view symbol) noexcept {
    if(symbol=="HeapAlloc" || symbol=="HeapFree" || symbol=="HeapSize")return 3;
    if(symbol=="GetLastError" || symbol=="GetCurrentThreadId" || symbol=="SwitchToThread" || tick_clock_service(symbol))return 0;
    if(symbol=="TlsSetValue" || symbol=="FlsSetValue")return 2;
    if(symbol=="SetLastError" || symbol=="TlsGetValue" || symbol=="TlsGetValue2" ||
       symbol=="FlsGetValue" || symbol=="FlsGetValue2" ||
       symbol=="QueryPerformanceCounter" || symbol=="QueryPerformanceFrequency" ||
       symbol=="EnterCriticalSection" || symbol=="TryEnterCriticalSection" || symbol=="LeaveCriticalSection" ||
       symbol=="WakeConditionVariable" || symbol=="WakeAllConditionVariable" ||
       symbol=="WakeByAddressSingle" || symbol=="WakeByAddressAll" || srw_service(symbol))return 1;
    return std::nullopt;
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
}
namespace x64arm::detail {
const WindowsServiceDescriptor* runtime_stub_descriptor(GuestAddress stub_base,GuestAddress address) noexcept;
// GuestMemory 的區域表（std::vector）讀取不加鎖；協調者在其他 worker 繼續執行時本地完成虛擬記憶體服務會插入／刪除
// 區域，worker 上的服務（native_window、load／store）同時查表即可能讀到重新配置中的陣列，把客體資料寫到錯誤位置
// （DQ3 強制平行在 13～16 秒崩潰；停用本地映射服務即不再發生）。worker 服務持共享鎖，本地映射服務持獨占鎖。
ParallelMappingLock& parallel_mapping_mutex() noexcept {static ParallelMappingLock lock;return lock;}
}
namespace x64arm {
// CRT memcpy／memmove／memset 只讀寫呼叫者傳入的客體範圍：較長的複製在單核時於常駐框架內執行，平行時若交回協調者，
// 每次都要停住全體 worker（DQ3 強制平行 60 秒內 193 萬次 memcpy 全停，遊戲停在第 1 幀）。worker 上以原生視窗
// 驗證整段範圍後直接處理；跨映射、COW、可執行頁或覆蓋返回槽時交回協調者。X64ARM_PARALLEL_CRT_MEMORY=0 關閉（x64）。
bool crt_memory_worker_service(std::string_view symbol) noexcept {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_PARALLEL_CRT_MEMORY");return !value || std::string_view(value)!="0";}();
    return enabled && (symbol=="memcpy" || symbol=="memmove" || symbol=="memset");
}
// CRT 唯讀的寬字串搜尋／比較、記憶體比較與浮點分類：只讀參數與客體記憶體，不寫 errno、LastError 或其他程序狀態，也不配置。
// worker 上直接呼叫同一 provider（invoke_runtime）；無效指標或字串超過上限等故障一律交回協調者完整重做，
// 返回前沒有任何副作用，可安全重放。DQ3 強制平行每秒數千次，原本每次都讓全體 worker 靜止。
// X64ARM_PARALLEL_CRT_PURE=0 關閉。
bool crt_pure_worker_service(const WindowsServiceDescriptor& descriptor) noexcept {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_PARALLEL_CRT_PURE");return !value || std::string_view(value)!="0";}();
    static constexpr std::string_view names[]{"wcsstr","wcschr","wcsrchr","wcscmp","wcsncmp","_finite","_isnan","memcmp"};
    return enabled && descriptor.family=="crt" && std::find(std::begin(names),std::end(names),descriptor.symbol)!=std::end(names);
}
// worker 上的 D3D11 寫入型方法（Get*、Check*、Map）只把結果寫到參數指向的位置。先核對每個參數值（含堆疊參數）
// 起算 4 KiB 的首尾頁都不是寫入時複製頁：共用映射鎖下不得私有化頁面（會改區域表與原生 MMU）。核對不通過或讀取
// 參數故障時交回協調者本地路徑。非指標參數落在 COW 頁只會保守地交回，不影響語義。
bool graphics_worker_outputs_private(const GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& descriptor) noexcept {
    const auto symbol=descriptor.symbol;
    if(symbol.find("Get")==std::string_view::npos && !symbol.starts_with("Check") && symbol!="Map")return true;
    try{
        // COM 的 this 亦由 WindowsCall 依 Win32 stdcall／Win64 暫存器與堆疊 ABI 取得。
        const WindowsCall arg(state,memory,descriptor);
        const auto address_mask=memory.execution_mode()==ExecutionMode::X86?UINT64_C(0xffffffff):UINT64_MAX;
        for(unsigned i=0;i<descriptor.arguments;++i){
            const auto value=arg(i);
            if(value<0x10000)continue;
            if(value>address_mask-4095)return false;
            if(memory.is_copy_on_write(GuestAddress{value}) || memory.is_copy_on_write(GuestAddress{value+4095}))return false;
        }
    }catch(...){return false;}
    return true;
}
bool WindowsServices::parallel_service(GuestAddress address) const noexcept {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_PARALLEL_SERVICES");return !setting || std::string_view(setting)!="0";}();
    if(!enabled || !runtime_.host_clock || !trace_services_.empty() || startup_profile::enabled() || runtime_.forced_exit_code())return false;
    if(graphics_worker_service(address) || d3d12_worker_service(address))return true; // 指標非空即可在 worker 執行。
    if(const auto* runtime=detail::runtime_stub_descriptor(stub_base_,address);runtime && crt_pure_worker_service(*runtime))return true;
    const auto symbol=name(address);
    if(x86_){
        static const bool win32=[] {const auto* value=std::getenv("X64ARM_X86_PARALLEL_SERVICES");return !value || std::string_view(value)!="0";}();
        static const bool heap=[] {const auto* value=std::getenv("X64ARM_X86_PARALLEL_HEAP");return !value || std::string_view(value)!="0";}();
        if(symbol=="HeapAlloc" || symbol=="HeapFree" || symbol=="HeapSize")return win32 && heap && guest_heap_chunked_;
        if(tick_clock_service(symbol))return win32 && parallel_tick_clock_enabled();
        return win32 && context_service_arguments(symbol).has_value() && (!srw_service(symbol) || parallel_srw_enabled());
    }
    if(tick_clock_service(symbol))return parallel_tick_clock_enabled();
    if(parallel_srw_enabled() && (srw_service(symbol) || msvcp_mutex_service(symbol)))return true;
    if(thrd_tail_service(symbol))return true;
    return symbol=="TlsGetValue" || symbol=="TlsGetValue2" || symbol=="TlsSetValue" ||
        symbol=="FlsGetValue" || symbol=="FlsGetValue2" || symbol=="FlsSetValue" ||
        symbol=="InitOnceBeginInitialize" || symbol=="InitOnceExecuteOnce" ||
        symbol=="WakeConditionVariable" || symbol=="WakeAllConditionVariable" || symbol=="SwitchToThread" ||
        symbol=="WakeByAddressSingle" || symbol=="WakeByAddressAll" ||
        symbol=="_control87" || symbol=="_controlfp" ||
        symbol=="GetLastError" || symbol=="SetLastError" || symbol=="GetCurrentThreadId" ||
        symbol=="QueryPerformanceCounter" || symbol=="QueryPerformanceFrequency" ||
        symbol=="EnterCriticalSection" || symbol=="TryEnterCriticalSection" || symbol=="LeaveCriticalSection" ||
        crt_memory_worker_service(symbol);
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
    const std::shared_lock mapping_guard(detail::parallel_mapping_mutex());
    struct CountScope {
        WindowsServices& self;std::uint64_t rip;std::chrono::steady_clock::time_point start;
        bool accepted{true};
        ~CountScope(){if(!accepted || !self.service_counts_enabled_)return;
            const auto ns=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());
            std::lock_guard lock(*self.worker_service_counts_mutex_);auto& c=self.worker_service_counts_[rip];++c.calls;c.ns+=ns;}
    } count_scope{*this,state.rip.value,service_counts_enabled_?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{}};
    if(const auto* graphics=graphics_worker_service(state.rip)){
        count_scope.accepted=false;
        // 鎖順序：映射（共用）→ D3D11。資格不符（裝置鎖、延後銷毀清單過長）時交回協調者，未改任何狀態。
        const std::lock_guard graphics_lock(graphics_mutex());
        const auto& descriptor=*graphics;
        if(!d3d11_local_eligible(state,memory,descriptor) || !graphics_worker_outputs_private(state,memory,descriptor))return false;
        count_scope.accepted=true;
        struct Local {~Local(){graphics_local()=false;}} local;graphics_local()=true;
        if(invoke_d3d11(state,memory,descriptor))throw std::logic_error("worker 上的 D3D11 服務意外終止客體");
        state.rep_compare={};
        return true;
    }
    if(const auto* d3d12=d3d12_worker_service(state.rip)){
        // 鎖順序與 D3D11 worker 相同：映射（共用）→ 圖形。讀取客體參數失敗等例外在改動前交回協調者重做（錄製與描述元複製可重做）。
        // 命令清單錄製（一條清單同時只由一個執行緒錄製）與只讀方法不取圖形鎖：它們只讀橋接層表格、只寫自己的清單；
        // 表格的結構變動（建立／釋放物件、索引增刪）都在全停或持獨占映射鎖的協調者本地路徑，與此處的共用映射鎖互斥。
        // 4 個 worker 同時錄製時圖形鎖競爭讓每次呼叫約 3～6 µs。X64ARM_D3D12_RECORD_UNLOCKED=0 恢復全部取鎖。
        static const bool record_unlocked=[] {const auto* value=std::getenv("X64ARM_D3D12_RECORD_UNLOCKED");return !value || std::string_view(value)!="0";}();
        const auto symbol=d3d12->symbol;
        // 描述元建立／複製（取樣器除外：要建 Metal 物件）同樣免圖形鎖：D3D12 規定為 free-threaded、應用程式不並行寫同一描述元，
        // 共用的鏡像待更新標記以無鎖點陣設定（電馭判客 Benchmark 每秒約 8 萬次 CopyDescriptors，取全域圖形鎖時每次約 2.8 µs）。
        const bool descriptor_write=symbol=="Device.CopyDescriptors" || symbol=="Device.CopyDescriptorsSimple" || symbol=="Device.CreateConstantBufferView" ||
            symbol=="Device.CreateShaderResourceView" || symbol=="Device.CreateUnorderedAccessView" || symbol=="Device.CreateRenderTargetView" || symbol=="Device.CreateDepthStencilView";
        // Allocator.Reset、Queue.Wait 實作只回傳成功（重播在提交時進行，GPU 端等待由提交順序保證），同樣免鎖：
        // 取鎖時每次平均 38 µs（等其他持鎖服務）。
        // AddRef／不歸零的 Release 以原子操作改計數，同樣免鎖。
        const bool reference=symbol.ends_with(".AddRef") || symbol.ends_with(".Release");
        const bool unlocked=record_unlocked && (symbol.starts_with("List.") || symbol=="Resource.GetGPUVirtualAddress" || symbol=="Device.GetDescriptorHandleIncrementSize" ||
            symbol=="Allocator.Reset" || symbol=="Queue.Wait" || descriptor_write || reference);
        std::unique_lock<std::recursive_mutex> graphics_lock(graphics_mutex(),std::defer_lock);if(!unlocked)graphics_lock.lock();
        if(symbol.ends_with(".AddRef") || symbol.ends_with(".Release") || symbol=="Resource.Map" || symbol=="Resource.Unmap"){
            bool done=false;
            try{done=symbol.starts_with("Resource.Map") || symbol=="Resource.Unmap"?d3d12_worker_map(state,memory,*d3d12):d3d12_worker_reference(state,memory,*d3d12);}catch(...){return false;}
            if(!done)return false;
            state.rep_compare={};return true;
        }
        struct Context {~Context(){d3d12_worker_context()=false;}} context;d3d12_worker_context()=true;
        std::optional<std::uint32_t> result;
        try{result=invoke_d3d12(state,memory,*d3d12);}catch(...){return false;}
        if(result)throw std::logic_error("worker 上的 D3D12 服務意外終止客體");
        state.rep_compare={};
        return true;
    }
    if(const auto* runtime=detail::runtime_stub_descriptor(stub_base_,state.rip);runtime && crt_pure_worker_service(*runtime)){
        std::optional<std::uint32_t> result;
        try{result=invoke_runtime(state,memory,*runtime);}catch(...){return false;}
        if(result)throw std::logic_error("worker 上的 CRT 唯讀服務意外終止客體");
        state.rep_compare={};
        return true;
    }
    const auto symbol=name(state.rip);
    if(!x86_ && thrd_tail_service(symbol)){
        std::uint32_t argument=0;
        if(symbol=="_Thrd_sleep"){try{argument=msvcp_wait_milliseconds(memory,state.reg(Gpr::Rcx));}catch(...){return false;}}
        const auto target=resolve_service("kernel32.dll",symbol=="_Thrd_sleep"?"Sleep":"SwitchToThread",false);if(!target.value)return false;
        state.reg(Gpr::Rcx)=argument;state.rip=target;state.rep_compare={};return true;
    }
    // worker 身分由交給引擎的 context 決定；客體更動 GS 不得冒用另一執行緒。
    // &thread.context 到執行緒編號的對應在程序期間不變（執行緒只增不刪、std::map 節點位址固定），每條 worker 以
    // 執行緒區域快取記住最近一次查到的對應，免取全域索引鎖；TEB 不符時清快取重查。協調者自己的 GuestState
    // （slot 0）不在索引中，沿用目前執行緒。
    struct IdentityCache {const WindowsServices* owner;const GuestState* state;std::uint64_t key;};
    thread_local IdentityCache identity{};
    const auto teb=x86_?state.fs_base:state.gs_base;
    auto key=threads_.current;bool cached=identity.owner==this && identity.state==&state;
    if(cached)key=identity.key;
    else{
        std::unique_lock<std::mutex> lock;const auto& index=resident_thread_index(lock);
        if(const auto context=index.contexts.find(&state);context!=index.contexts.end()){key=context->second;identity={this,&state,key};}
    }
    auto found=threads_.items.find(key);
    if(cached && (found==threads_.items.end() || found->second.teb!=teb)){
        identity={};key=threads_.current;
        std::unique_lock<std::mutex> lock;const auto& index=resident_thread_index(lock);
        if(const auto context=index.contexts.find(&state);context!=index.contexts.end()){key=context->second;identity={this,&state,key};}
        found=threads_.items.find(key);
    }
    if(found==threads_.items.end() || found->second.teb!=teb)return false;
    auto& thread=found->second;const auto tid=found->first;const bool current=tid==threads_.current;
    const unsigned unit=x86_?4U:8U;
    const auto error_offset=static_cast<unsigned>(x86_?X64ARM_TLS_X86_ERROR_OFFSET:X64ARM_TLS_X64_ERROR_OFFSET);
    // 所有存取先驗證。native_window 排除 COW、可執行寫入、外部 backing 及跨映射；
    // 失敗留給協調者處理，不在 worker 內改變映射或吞掉客體例外。
    const auto pointer=[&](std::uint64_t address,unsigned size,unsigned access)->void* {
        if((address&(std::min(size,unit)-1U)) || address>UINT64_MAX-size ||
           (x86_ && (address>UINT32_MAX || size>UINT64_C(0x100000000)-address)))return nullptr;
        // 一般 WindowsCall 在寫入後讀取返回位址；輸出若與返回槽重疊，交回完整路徑。
        const auto sp=state.reg(Gpr::Rsp);
        if((access&2U) && (sp>UINT64_MAX-unit || (address<sp+unit && address+size>sp)))return nullptr;
        // 對齊的 4／8 byte 存取不跨客體頁；沿用相同原生權限，避免每次 API
        // 都經 native_pointer 重寫全體 worker 共用的 JIT 頁面快取與 ready 旗標。
        const auto window=memory.native_window(GuestAddress{address},access);
        return window.data && window.after>=size?window.data:nullptr;
    };
    const auto load=[](const void* source,unsigned size){std::uint64_t value=0;std::memcpy(&value,source,size);return value;};
    const auto put=[](void* destination,std::uint64_t value,unsigned size){std::memcpy(destination,&value,size);};
    const auto sp=state.reg(Gpr::Rsp);
    auto* stack=pointer(sp,unit,1);if(!stack || sp>UINT64_MAX-unit)return false;
    const auto target=load(stack,unit);
    std::array<std::uint64_t,3> arguments{state.reg(Gpr::Rcx),state.reg(Gpr::Rdx),state.reg(Gpr::R8)};
    auto& first=arguments[0];auto& second=arguments[1];
    unsigned argument_count=0;
    if(x86_){
        const auto count=context_service_arguments(symbol);if(!count)return false;
        argument_count=*count;
        const auto extent=std::uint64_t{argument_count+1}*unit;
        if(extent>UINT64_C(0x100000000)-sp)return false;
        // 在任何客體副作用前核對全部引數；逐槽讀取容許合法框架跨頁。
        for(unsigned n=0;n<argument_count;++n){
            auto* slot=pointer(sp+(n+1)*unit,unit,1);if(!slot)return false;
            arguments[n]=load(slot,unit);
        }
    }
    std::uint64_t result=0;
    // 與一般服務一樣，服務邊界保持客體可見的記憶體順序；不依賴宿主是否已授權 TSO。
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if(x86_ && (symbol=="HeapAlloc" || symbol=="HeapFree" || symbol=="HeapSize")){
        using namespace detail::guest_heap_layout;
        const bool allocating=symbol=="HeapAlloc",freeing=symbol=="HeapFree";
        const auto heap=first,value=arguments[2];const auto flags=static_cast<std::uint32_t>(second);
        if(!guest_heap_chunked_ || !guest_heap_state_.value || (flags&~(allocating?9U:1U)) ||
           (allocating && value>guest_heap_small_limit+guest_heap_aligned_header))return false;
        const auto heap_lock=runtime_.heap_locks.find(heap);
        if(heap_lock!=runtime_.heap_locks.end() && heap_lock->second.owner)return false;
        auto private_heap=guest_private_heaps_.end();
        if(heap!=process_heap){
            if(!runtime_.private_heaps.contains(heap))return false;
            private_heap=guest_private_heaps_.find(heap);
            if(private_heap==guest_private_heaps_.end() || private_heap->second.heads.empty())return false;
        }
        // Win32 沒有直接改串列的客體常式。所有 worker 共用狀態頁的鎖，
        // 連同私有 heap 串列頭與 HeapWalk 變動世代一起發布；協調者只在全體停妥後操作。
        auto* guard=pointer(guest_heap_state_.value,4,3);if(!guard)return false;
        std::atomic_ref lock(*static_cast<std::uint32_t*>(guard));std::uint32_t expected=0;
        if(!lock.compare_exchange_strong(expected,1,std::memory_order_acquire)){
            // 鎖由同批 worker 持有；其完成服務或協調者要求一致邊界時，既有信箱會喚醒重試者。
            // 只在競爭時讀取一次 A/B 設定；此診斷區段只計碰撞，不代表完整 API 或等待成本。
            std::optional<entry_runtime_profile::Scope> contention;
            if(entry_runtime_profile::enabled())contention.emplace(entry_runtime_profile::Kind::Bridge,state.rip.value,"resident heap contention");
            static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_X86_HEAP_RETRY");
                return !setting || std::string_view(setting)!="0";}();
            retry=enabled;return false;
        }
        struct Unlock {std::atomic_ref<std::uint32_t> lock;~Unlock(){lock.store(0,std::memory_order_release);}} unlock{lock};
        std::uint64_t address=value,cls=allocating?guest_heap_class(std::max(UINT64_C(1),value)):0;
        void* guest_head=nullptr;std::uint64_t* private_head=nullptr;
        const auto head_for=[&](std::uint64_t index){
            if(private_heap!=guest_private_heaps_.end()){
                if(index>=private_heap->second.heads.size())return false;
                private_head=&private_heap->second.heads[static_cast<std::size_t>(index)];
            }else guest_head=pointer(guest_heap_state_.value+guest_heap_heads+index*8,8,3);
            return private_head || guest_head;
        };
        const auto read_head=[&]{return private_head?*private_head:load(guest_head,8);};
        const auto write_head=[&](std::uint64_t next){if(private_head)*private_head=next;else put(guest_head,next,8);};
        if(allocating){if(!head_for(cls))return false;address=read_head();}
        // 空串列、非配置區、損毀標頭、COW／權限變更均無副作用地交回原 provider。
        if(!address || !guest_heap_contains(address))return false;
        const auto chunk=address&~(guest_heap_chunk-1);
        const auto header=memory.native_window(GuestAddress{chunk},1);
        if(!header.data || header.after<32 || load(header.data,8)!=guest_heap_magic)return false;
        const auto* bytes=static_cast<const std::uint8_t*>(header.data);
        const auto stored_class=load(bytes+8,8),block=load(bytes+16,8),owner=load(bytes+24,8);
        if(!valid_block(address,stored_class,block) || (owner?owner:process_heap)!=heap ||
           (allocating && stored_class!=cls))return false;
        cls=stored_class;
        if(!allocating && !head_for(cls))return false;
        const auto payload=memory.native_window(GuestAddress{address},allocating || freeing?3U:1U);
        if(!payload.data || payload.after<block)return false;
        const auto written=freeing?UINT64_C(8):allocating && (flags&8U)?std::max(UINT64_C(1),value):0;
        if(written && address<sp+unit && address+written>sp)return false;
        if(allocating){
            const auto next=load(payload.data,8);
            if(next && (!guest_heap_contains(next) || next==address))return false;
            write_head(next);put(payload.data,0,8); // 串列指標清零，未用過的區塊全為零。
            if(flags&8U)std::memset(payload.data,0,static_cast<std::size_t>(std::max(UINT64_C(1),value)));
            result=address;++guest_heap_mutations_;
        }else if(freeing){put(payload.data,read_head(),8);write_head(address);++guest_heap_mutations_;result=1;}
        else result=block;
    }else if(tick_clock_service(symbol)){
        // 與 host-clock provider 共用開機單調時間軸；純讀取不推進共享時鐘、排程或 KUSER 頁。
        // timeGetTime／GetTickCount 保留 DWORD 回繞，GetTickCount64 在 Win32 以 EDX:EAX 返回。
        const auto milliseconds=detail::monotonic_ticks()/10000;
        result=symbol=="GetTickCount64"?milliseconds:static_cast<std::uint32_t>(milliseconds);
    }else if(symbol=="GetLastError" || symbol=="SetLastError"){
        auto* error=pointer(thread.teb+error_offset,4,symbol=="GetLastError"?1U:2U);if(!error)return false;
        // 原生 SetLastError 或客體直接寫入 TEB 後，以客體值為準。
        // 協調者的快取只在完整服務／排程邊界同步，不可蓋過 worker 的最新狀態。
        if(symbol=="GetLastError")result=load(error,4);
        else {put(error,static_cast<std::uint32_t>(first),4);if(current)last_error_=static_cast<std::uint32_t>(first);}
    }else if(symbol=="TlsGetValue" || symbol=="TlsGetValue2" || symbol=="TlsSetValue"){
        const auto index=static_cast<std::uint32_t>(first);const bool reading=symbol!="TlsSetValue";
        if(index>=runtime_.tls_used.size() || (!reading && !detail::tls_writable_index(index,runtime_.tls_used)) || !runtime_.image.base)return false;
        const auto offset=detail::tls_slot_offset(x86_,index);
        auto* slot=pointer(thread.teb+offset,unit,reading?1U:2U);if(!slot)return false;
        auto* error=symbol=="TlsGetValue"?pointer(thread.teb+error_offset,4,2):nullptr;
        if(symbol=="TlsGetValue" && !error)return false;
        if(reading){result=load(slot,unit);if(error){put(error,0,4);if(current)last_error_=0;}}
        // PE 的 TLS 真值位於 TEB；陣列只保留非 PE provider／狀態相容性，不能覆寫客體直接寫入。
        else {put(slot,second,unit);(current?runtime_.tls_values:thread.tls_values)[index]=second;result=1;}
    }else if(symbol=="FlsGetValue" || symbol=="FlsGetValue2" || symbol=="FlsSetValue"){
        const auto index=static_cast<std::uint32_t>(first);const bool reading=symbol!="FlsSetValue";
        if(index>=runtime_.fls_used.size() || !runtime_.fls_used[index])return false;
        auto* error=symbol=="FlsGetValue"?pointer(thread.teb+error_offset,4,2):nullptr;
        if(symbol=="FlsGetValue" && !error)return false;
        auto* mirror=!reading && fls_mirror_?pointer(thread.teb+0x1840+std::uint64_t{index}*8,8,2):nullptr;
        if(!reading && fls_mirror_ && !mirror)return false;
        auto& values=current?runtime_.fls_values:thread.fls_values;
        if(reading){result=values[index];if(error){put(error,0,4);if(current)last_error_=0;}}
        else {values[index]=second;if(mirror)put(mirror,second,8);result=1;}
    }else if(symbol=="InitOnceBeginInitialize" || symbol=="InitOnceExecuteOnce"){
        const bool begin=symbol=="InitOnceBeginInitialize";
        if(begin && (static_cast<std::uint32_t>(second)&~1U))return false;
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
        if(!pointer(first,unit,2))return false;
        // 等待集合在 epoch 內固定。沒有尚待喚醒者時，通知已完整完成；有等待者
        // 才交協調者更新等待狀態、重新取得鎖及排程，避免無效通知停止全體 worker。
        for(const auto& [id,other]:threads_.items){
            (void)id;
            if(other.status==ThreadStatus::Waiting && other.wait.kind==ThreadWaitKind::Condition &&
               other.wait.handles[0]==first && !other.wait.signaled)return false;
        }
    }else if(symbol=="WakeByAddressSingle" || symbol=="WakeByAddressAll"){
        // 與條件變數相同：等待集合在 epoch 內固定，沒有尚待喚醒的位址等待者時通知已完整完成。
        for(const auto& [id,other]:threads_.items){
            (void)id;
            if(other.status==ThreadStatus::Waiting && other.wait.kind==ThreadWaitKind::Address &&
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
        if(sp%16!=8 || sp>UINT64_MAX-40)return false;
        const auto home=memory.native_window(GuestAddress{sp+8},2);
        if(!home.data || home.after<32)return false;
        if(invoke_math(state,memory,{"crt",symbol,2}))throw std::logic_error("浮點控制服務意外終止客體");
        std::atomic_thread_fence(std::memory_order_seq_cst);return true;
    }else if(srw_service(symbol)){
        // 字組編碼與協調者路徑／客體常式相同：bit0 獨占、bit1 宿主有等待者、bit2 起為共用持有數。
        // 只以原子 CAS 改字組，不碰執行緒表；與協調者的 CAS 迴圈及客體 lock cmpxchg 互相線性化。
        auto* word=pointer(first,unit,3);if(!word)return false;
        const auto operate=[&]<class Word>(Word* storage){
        std::atomic_ref<Word> lock(*storage);
        const bool shared=symbol.ends_with("Shared");
        auto value=lock.load(std::memory_order_acquire);
        if(symbol.starts_with("Release")){
            for(;;){
                // 有等待者時須由原服務喚醒；未持有則交回原路徑回報錯誤。
                if((value&2) || (shared?!(value>>2):!(value&1))){detail::record_srw(detail::SrwEvent::WorkerDefer);return false;}
                if(lock.compare_exchange_weak(value,shared?value-Word{4}:value&~Word{1},std::memory_order_acq_rel,std::memory_order_acquire))break;
            }
            detail::record_srw(detail::SrwEvent::WorkerRelease);
        }else{
            // 取得條件與原服務相同（bit1 存在時仍可插隊取得）；Try 不等待，競爭即回 FALSE。
            const bool attempt=symbol.starts_with("Try");const bool spin=!attempt && active.size()>1;
            for(unsigned round=0;;){
                if(!(value&1) && (shared || !(value>>2))){
                    if(shared && (value>>2)==(std::numeric_limits<Word>::max()>>2)){detail::record_srw(detail::SrwEvent::WorkerDefer);return false;}
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
        return true;
        };
        if(!(x86_?operate(static_cast<std::uint32_t*>(word)):operate(static_cast<std::uint64_t*>(word))))return false;
    }else if(!x86_ && crt_memory_worker_service(symbol)){
        const auto destination=first,count=arguments[2];
        if(count){
            // 原生視窗以客體頁為單位：先逐頁取得目的（可寫）與來源（可讀）的全部片段，任何一頁不符就在寫入前
            // 交回協調者；寫入不得覆蓋返回位址槽。過大的範圍（>64 MiB）也交回，避免片段表過長。
            if(count>UINT64_C(64)<<20 || destination>UINT64_MAX-count || (destination<sp+unit && destination+count>sp))return false;
            using Segments=std::vector<std::pair<std::uint8_t*,std::size_t>>;
            thread_local Segments target_segments,source_segments;
            const auto collect=[&](std::uint64_t address,std::uint64_t size,unsigned access,Segments& out){
                out.clear();
                while(size){
                    const auto window=memory.native_window(GuestAddress{address},access);
                    if(!window.data || !window.after)return false;
                    const auto part=std::min<std::uint64_t>(window.after,size);
                    out.emplace_back(static_cast<std::uint8_t*>(window.data),static_cast<std::size_t>(part));
                    address+=part;size-=part;
                }
                return true;
            };
            if(!collect(destination,count,2,target_segments))return false;
            if(symbol=="memset"){
                for(const auto& [data,size]:target_segments)std::memset(data,static_cast<int>(static_cast<std::uint8_t>(second)),size);
            }else{
                if(second>UINT64_MAX-count || !collect(second,count,1,source_segments))return false;
                // 目的在來源之後且重疊時由尾端往前分段複製，維持 memmove 語義；片段內交給 std::memmove。
                const bool backward=destination>second && destination<second+count;
                std::size_t ti=0,si=0,to=0,so=0;
                if(backward){ti=target_segments.size();si=source_segments.size();}
                for(auto remaining=count;remaining;){
                    if(!backward){
                        const auto part=std::min(target_segments[ti].second-to,source_segments[si].second-so);
                        std::memmove(target_segments[ti].first+to,source_segments[si].first+so,part);
                        to+=part;so+=part;remaining-=part;
                        if(to==target_segments[ti].second){++ti;to=0;}
                        if(so==source_segments[si].second){++si;so=0;}
                    }else{
                        if(!to)to=target_segments[--ti].second;
                        if(!so)so=source_segments[--si].second;
                        const auto part=std::min(to,so);
                        to-=part;so-=part;remaining-=part;
                        std::memmove(target_segments[ti].first+to,source_segments[si].first+so,part);
                    }
                }
            }
        }
        result=destination;
    }else if(!x86_ && msvcp_mutex_service(symbol)){
        // 版面與協調者 msvcp_sync_handlers 相同：+0 型別、+16 SRW 字組、+72 擁有者執行緒編號、+76 持有數。
        // SRW 字組以 worker SRW 的同一編碼 CAS（bit0 獨占、bit1 宿主有等待者）；擁有者與持有數只由持有者寫入。
        // 競爭超過自旋上限、釋放時有等待者或狀態不符，一律在改動前交回協調者原路徑（阻塞與喚醒由它負責）。
        auto* word=pointer(first+16,8,3);auto* owner=pointer(first+72,4,3);auto* count=pointer(first+76,4,3);auto* type=pointer(first,4,1);
        if(!word || !owner || !count || !type)return false;
        std::atomic_ref<std::uint64_t> lock(*static_cast<std::uint64_t*>(word));
        const auto me=thread_identifier(tid);const auto held=static_cast<std::uint32_t>(load(count,4));
        const bool owned=held && static_cast<std::uint32_t>(load(owner,4))==me;
        if(symbol=="_Mtx_unlock"){
            if(!owned)return false;
            if(held>1)put(count,held-1U,4);
            else{
                auto value=lock.load(std::memory_order_acquire);
                if((value&2) || !(value&1)){detail::record_srw(detail::SrwEvent::WorkerDefer);return false;}
                put(owner,UINT32_MAX,4);put(count,0,4);
                // 期間出現等待者：仍持有鎖（他人不會改這兩欄），還原後交回協調者喚醒。
                while(!lock.compare_exchange_weak(value,value&~std::uint64_t{1},std::memory_order_acq_rel,std::memory_order_acquire))
                    if(value&2){put(owner,me,4);put(count,1,4);detail::record_srw(detail::SrwEvent::WorkerDefer);return false;}
                detail::record_srw(detail::SrwEvent::WorkerRelease);
            }
            result=0;
        }else if(owned){
            // 與協調者相同：遞迴或 plain 型別累加持有數，其餘回 _Busy（3）。
            const auto kind=static_cast<std::uint32_t>(load(type,4));
            if(((kind&~0x100U)!=1 && !(kind&0x100U)) || held==INT32_MAX)result=3;
            else{put(count,held+1U,4);result=0;}
        }else{
            const bool attempt=symbol=="_Mtx_trylock",spin=!attempt && active.size()>1;
            auto value=lock.load(std::memory_order_acquire);
            for(unsigned round=0;;){
                if(!(value&1) && !(value>>2)){
                    if(lock.compare_exchange_weak(value,value|1,std::memory_order_acq_rel,std::memory_order_acquire)){
                        put(owner,me,4);put(count,held+1U,4);result=0;
                        detail::record_srw(round?detail::SrwEvent::WorkerSpinAcquire:detail::SrwEvent::WorkerAcquire);break;
                    }
                    continue;
                }
                if(attempt){result=3;detail::record_srw(detail::SrwEvent::WorkerTryFail);break;}
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
        const auto bytes=x86_?24U:40U;
        if(!window.data || window.after<bytes)return false;
        auto* lock_count=pointer(first+unit,4,2);auto* recursion=pointer(first+unit+4,4,2);auto* owner=pointer(first+unit+8,unit,2);
        if(first>UINT64_MAX-bytes || !lock_count || !recursion || !owner)return false;
        auto& section=section_it->second;
        // kernel32 臨界區段依客體字組協定（windows_threads.cpp）：字組以 CAS 更新，與客體常式及協調者相容，不需宿主守衛。
        // 有等待者的完全釋放、他人持有時的 Enter 一律交回協調者（喚醒與阻塞由等待解析負責）。
        if(!section.guest || reinterpret_cast<std::uintptr_t>(lock_count)%4)return false;
        constexpr std::uint32_t free_word=UINT32_MAX,waiter_bit=0x40000000U,free_waiting=0x7fffffffU,count_mask=0x3fffffffU;
        std::atomic_ref word(*static_cast<std::uint32_t*>(lock_count));
        const auto me=thread_identifier(tid);
        const auto depth=static_cast<std::uint32_t>(load(recursion,4));
        if(symbol=="LeaveCriticalSection"){
            if(load(owner,unit)!=me || !depth)return false;
            if(depth>1){put(recursion,depth-1U,4);word.fetch_sub(1,std::memory_order_seq_cst);}
            else{
                auto value=word.load(std::memory_order_seq_cst);
                if(value&waiter_bit)return false;
                put(owner,0,unit);put(recursion,0,4);
                if(!word.compare_exchange_strong(value,free_word,std::memory_order_seq_cst)){put(owner,me,unit);put(recursion,1,4);return false;}
            }
        }else{
            auto value=word.load(std::memory_order_seq_cst);
            if(value==free_word){
                if(!word.compare_exchange_strong(value,0,std::memory_order_seq_cst))return false;
                put(recursion,1,4);put(owner,me,unit);result=1;
            }else if(value!=free_waiting && load(owner,unit)==me){
                if((value&count_mask)>=count_mask-1U || depth>=count_mask)return false;
                word.fetch_add(1,std::memory_order_seq_cst);put(recursion,depth+1U,4);result=1;
            }else if(symbol=="TryEnterCriticalSection" && value!=free_waiting)result=0;
            else{
                // 只有持有者正取得 worker 時，等待服務進度才可能完成；持有者尚未排程時交回共用等待流程，
                // 不能讓等待者佔著 worker 睡到下一個排程期限，反過來阻止持有者執行 LeaveCriticalSection。
                const auto holder_tid=load(owner,unit);
                const auto holder=std::find_if(threads_.items.begin(),threads_.items.end(),[&](const auto& entry){return holder_tid && thread_identifier(entry.first)==holder_tid;});
                retry=holder!=threads_.items.end() && (holder->first==threads_.current ||
                    std::find(active.begin(),active.end(),&holder->second.context)!=active.end());
                return false;
            }
        }
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    state.rip=GuestAddress{target};
    state.reg(Gpr::Rsp)=x86_?static_cast<std::uint32_t>(sp+4+argument_count*4):sp+8;
    state.reg(Gpr::Rax)=x86_?static_cast<std::uint32_t>(result):result;
    if(x86_ && symbol=="GetTickCount64")state.reg(Gpr::Rdx)=static_cast<std::uint32_t>(result>>32);
    return true;
}
#endif
}
