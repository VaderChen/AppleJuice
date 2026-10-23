#include "x64arm/startup_profile.hpp"
#include "x64arm/windows_services.hpp"
#include "resident_service_policy.hpp"
#include "crt_library.hpp"
#include "builtin_export_aliases.hpp"
#include "windows_scalar_math.hpp"
#include "guest_scalar_math_code.hpp"
#include "guest_thread_code.hpp"
#include "guest_thread_x86_code.hpp"
#include "guest_tls.hpp"
#include "guest_io_completion_code.hpp"
#include "io_wait_policy.hpp"
#include "x64arm/entry_runtime_profile.hpp"
#include "windows_crt_character.hpp"
#include "monotonic_clock.hpp"
#include "x64arm/windows_api_sets.hpp"
#include "x64arm/windows_raw_input.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_cpu_time.hpp"
#include "x64arm/processor_information.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/security_access.hpp"
#include "windows_profile_security.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <bit>
#include <atomic>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <chrono>
#include <charconv>
#include <unordered_map>

namespace x64arm {
WindowsServices::WindowsServices() {
    startup_profile::Scope phase("services","construct_body",true);
    native_angle_=make_native_angle();
    if(const auto* trace=std::getenv("X64ARM_TRACE_SERVICES"))trace_services_=trace;
    if(const auto* counts=std::getenv("X64ARM_SERVICE_COUNTS");counts && *counts){service_counts_enabled_=true;if(std::string_view(counts)!="1")service_counts_directory_=counts;}
    if(const auto* libraries=std::getenv("X64ARM_NATIVE_LIBRARIES"))native_libraries_.configure(libraries);
}
namespace { void report_guard_statistics() noexcept; }
void WindowsServices::dump_service_counts() {
    service_count_dumped_=std::chrono::steady_clock::now();
    FILE* out=stderr;std::string path;
    if(!service_counts_directory_.empty()){
        path=service_counts_directory_+"/service-counts-"+std::to_string(process_id())+".txt.tmp";
        out=std::fopen(path.c_str(),"w");if(!out)return;
    }
    const auto dump=[&](const char* label,const std::unordered_map<std::uint64_t,ServiceCount>& counts){
        std::vector<std::pair<std::uint64_t,ServiceCount>> rows(counts.begin(),counts.end());
        std::uint64_t calls=0,ns=0;for(const auto& [rip,c]:rows){calls+=c.calls;ns+=c.ns;}
        std::fprintf(out,"[service_counts] pid=%llu kind=%s entries=%zu calls=%llu ns=%llu\n",static_cast<unsigned long long>(process_id()),label,rows.size(),
            static_cast<unsigned long long>(calls),static_cast<unsigned long long>(ns));
        std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.second.ns>b.second.ns;});
        for(std::size_t n=0;n<rows.size() && n<64;++n){
            const auto symbol=name(GuestAddress{rows[n].first});
            std::fprintf(out,"[service_counts]   %s rip=0x%llx calls=%llu ns=%llu avg_ns=%llu name=%.*s\n",label,static_cast<unsigned long long>(rows[n].first),
                static_cast<unsigned long long>(rows[n].second.calls),static_cast<unsigned long long>(rows[n].second.ns),
                static_cast<unsigned long long>(rows[n].second.calls?rows[n].second.ns/rows[n].second.calls:0),static_cast<int>(symbol.size()),symbol.data());
        }
    };
    dump("boundary",service_counts_);
    {std::lock_guard lock(*worker_service_counts_mutex_);dump("worker",worker_service_counts_);}
    if(out!=stderr){std::fclose(out);std::rename(path.c_str(),path.substr(0,path.size()-4).c_str());}
}
WindowsServices::~WindowsServices(){if(service_counts_enabled_)dump_service_counts();report_guard_statistics();while(!policy_sections_.empty())discard_policy_thread(policy_sections_.begin()->second.thread);release_url_cache();
    if(std::getenv("X64ARM_IO_WAIT_PROFILE"))std::fprintf(stderr,"[io_wait] guest_packets=%llu guest_timeouts=%llu immediate=%llu waits=%llu\n",
        static_cast<unsigned long long>(io_wait_stats_.packets),static_cast<unsigned long long>(io_wait_stats_.timeouts),
        static_cast<unsigned long long>(io_wait_stats_.immediate),static_cast<unsigned long long>(io_wait_stats_.waits));
}
bool WindowsServices::close_wait_object(std::uint64_t handle){
    const auto found=runtime_.wait_objects.find(handle);if(found==runtime_.wait_objects.end())return false;
    const auto mutex=found->second.kind==WaitObject::Kind::Mutex?found->second.mutex:0,identity=found->second.identity;
    // 關閉 handle 不使已進入的等待重新解析失效的 handle；以內部參照保留同一物件。
    std::uint64_t retained{};
    for(auto& [id,thread]:threads_.items){
        (void)id;
        if(thread.status!=ThreadStatus::Waiting || (thread.wait.kind!=ThreadWaitKind::Objects && thread.wait.kind!=ThreadWaitKind::GuiInput))continue;
        for(auto& pending:thread.wait.handles)if(pending==handle){
            if(!retained){retained=runtime_.allocate_wait_handle();runtime_.wait_objects.emplace(retained,found->second);threads_.retained_wait_handles.push_back(retained);}
            pending=retained;
        }
    }
    change_notifications_.erase(handle);runtime_.wait_objects.erase(found);runtime_.handle_flags.erase(handle);
    if(identity && std::none_of(runtime_.wait_objects.begin(),runtime_.wait_objects.end(),[&](const auto& entry){return entry.second.identity==identity;})){cancel_waitable_timer_apcs(identity);runtime_.wait_states.erase(identity);}
    if(mutex && std::none_of(runtime_.wait_objects.begin(),runtime_.wait_objects.end(),[&](const auto& entry){return entry.second.kind==WaitObject::Kind::Mutex && entry.second.mutex==mutex;})){
        auto& object=runtime_.mutex_objects.at(mutex);
        if(object.shared && object.shared->owned()){object.shared->detach_handles();object.name.clear();}
        else runtime_.mutex_objects.erase(mutex);
    }
    return true;
}
namespace {
// X64ARM_TRACE_CLOCK=1：宿主時鐘模式下回報程序內時間曾超前硬體計數器（每程序一次）。
bool trace_clock() noexcept {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_TRACE_CLOCK");return setting && *setting && std::string_view(setting)!="0";}();
    return enabled;
}
}
std::uint64_t WindowsServices::timestamp_counter(const GuestState& state,GuestMemory& memory){
    if(!runtime_.host_clock)return state.tsc;
    retired(memory,0);
    // 宿主時鐘模式的 TSC 依客體處理器身分的標稱頻率計數（宿主未公告標稱值時用最高頻率），與 CPUID 0x16 同源；
    // 兩者皆未知才沿用 100 ns 刻度。由同一宿主單調時鐘換算，跨執行緒與程序保持單調。
    const auto& identity=memory.processor_identity();const std::uint64_t mhz=identity.base_mhz?identity.base_mhz:identity.maximum_mhz;
    if(!mhz)return runtime_.clock_ticks;
    return static_cast<std::uint64_t>(static_cast<unsigned __int128>(runtime_.clock_ticks)*mhz/10U);
}
void WindowsServices::set_host_clock(GuestMemory& memory, bool enabled) {
    if (!initialized_) throw std::logic_error("時鐘模式需要已初始化的 Windows 服務");
    if (runtime_.host_clock == enabled) return;
    if (runtime_.clock_ticks || runtime_.machine_clock) throw std::logic_error("時鐘模式必須在客體開始執行前設定");
    runtime_.host_clock = enabled;
    if (enabled) {
        // 宿主 steady epoch 在各程序相同，父子 QPC／uptime 不可各自從零開始。
        const auto absolute = UINT64_C(116444736000000000) + static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::duration<std::int64_t,std::ratio<1,10000000>>>(std::chrono::system_clock::now().time_since_epoch()).count());
        runtime_.clock_ticks = detail::monotonic_ticks();
        runtime_.filetime_base = absolute - runtime_.clock_ticks;
        runtime_.process_creation_time=absolute;
        runtime_.process_creation_clock_ticks=runtime_.clock_ticks;
        if(const auto cpu=host_process_cpu_time()){runtime_.process_start_user=cpu->user;runtime_.process_start_kernel=cpu->kernel;}
    }
    runtime_.host_clock_pending = 0;
    retired(memory, 0);
}
void WindowsServices::prepare_machine_clock(){
    if(runtime_.machine_clock || !runtime_.network.external_io)return;
    const auto lifetime=runtime_.clock_ticks>=runtime_.process_creation_clock_ticks?runtime_.clock_ticks-runtime_.process_creation_clock_ticks:0;
    runtime_.machine_clock=MachineClock::open(mapping_session(),runtime_.host_clock,runtime_.clock_ticks,runtime_.local_system_filetime());
    if(runtime_.machine_clock){const auto time=runtime_.system_time_state();runtime_.clock_ticks=time.monotonic;
        runtime_.process_creation_time=lifetime>time.filetime?0:time.filetime-lifetime;runtime_.process_creation_clock_ticks=lifetime>time.monotonic?0:time.monotonic-lifetime;}
}
void WindowsServices::retired(GuestMemory& memory, std::uint64_t instructions) {
    if (!initialized_) return;
    if(instructions){prepare_machine_clock();prepare_machine_shutdown();}
    if(runtime_.lowio_standard_dirty)refresh_crt_lowio(memory);
    if (runtime_.host_clock) {
        // 共享時間頁與排程使用同一個單調時鐘；API 入口與等待迴圈會立即同步。
        // 純運算每 4096 指令同步一次，避免每條客體指令都讀宿主時鐘。
        if (instructions && instructions < 4096 - runtime_.host_clock_pending) {
            runtime_.host_clock_pending += instructions;
            return;
        }
        runtime_.host_clock_pending = 0;
        // 宿主時鐘直接採用硬體計數器；程序內數值若曾被推前，不能延續成「先跳前再凍結」。
        const auto hardware=detail::monotonic_ticks();
        if(runtime_.clock_ticks>hardware+UINT64_C(1000000) && trace_clock()){
            static std::atomic<bool> reported{};
            if(!reported.exchange(true))std::fprintf(stderr,"host_clock_ahead pid=%d ahead_ms=%llu\n",static_cast<int>(::getpid()),static_cast<unsigned long long>((runtime_.clock_ticks-hardware)/10000));
        }
        runtime_.clock_ticks=hardware;
    } else runtime_.clock_ticks += instructions;
    // 一次快照同時提供單調與系統時間，避免 synchronize 後又重新同步。
    const auto time=runtime_.system_time_state();runtime_.clock_ticks=time.monotonic;
    std::array<std::uint8_t, 24> times{};
    if constexpr (std::endian::native == std::endian::little) {
        // KSYSTEM_TIME 保留 LowPart、High1Time、High2Time 的原排列與每次更新。
        // 用完整 word 編碼，避免每條客體指令都產生 24 次單 byte store。
        const auto ticks = time.monotonic, absolute = time.filetime;
        const auto high_ticks = static_cast<std::uint32_t>(ticks >> 32);
        const auto high_absolute = static_cast<std::uint32_t>(absolute >> 32);
        const std::array words{static_cast<std::uint32_t>(ticks), high_ticks, high_ticks,
                               static_cast<std::uint32_t>(absolute), high_absolute, high_absolute};
        times = std::bit_cast<decltype(times)>(words);
    } else {
        const std::array values{time.monotonic, time.filetime};
        for (std::size_t item = 0; item < values.size(); ++item) {
            const auto value = values[item];
            for (unsigned byte = 0; byte < 8; ++byte) times[item * 12 + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
            for (unsigned byte = 0; byte < 4; ++byte) times[item * 12 + 8 + byte] = static_cast<std::uint8_t>(value >> (32 + byte * 8));
        }
    }
    memory.update_readonly(GuestAddress{0x7ffe0008}, times);
}
void WindowsServices::set_standard_input(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > output_limit) throw std::invalid_argument("stdin 快照超過 16 MiB 上限");
    runtime_.standard_input = CrtFile{std::vector<std::uint8_t>(bytes.begin(), bytes.end()), 0, false, false};
    runtime_.standard_input.standard_handle=0x100;
    runtime_.lowio_standard_dirty=true;
}
namespace {
struct Service { std::string_view name; unsigned arguments;std::uint32_t wide_arguments{}; };
constexpr std::array services{
    Service{"OutputDebugStringA",1},Service{"OutputDebugStringW",1},Service{"CreatePipe", 4}, Service{"GetStdHandle", 1}, Service{"WriteFile", 5}, Service{"ReadFileEx",5}, Service{"WriteFileEx",5}, Service{"ExitProcess", 1},
    Service{"GetLastError", 0}, Service{"SetLastError", 1}, Service{"GetCommandLineW", 0},
    Service{"HeapSetInformation",4}, Service{"HeapQueryInformation",5}, Service{"HeapLock",1}, Service{"HeapUnlock",1}, Service{"HeapCreate",3}, Service{"HeapDestroy",1}, Service{"HeapValidate",3}, Service{"GetProcessHeap", 0}, Service{"HeapReAlloc", 4}, Service{"HeapAlloc", 3}, Service{"HeapFree", 3}, Service{"HeapSize", 3},
    Service{"CreateFileW", 7}, Service{"CreateFileA", 7}, Service{"CreateFile2",5}, Service{"ReadFile", 5}, Service{"CloseHandle", 1},
    Service{"GetFileSizeEx", 2}, Service{"SetFilePointerEx", 4,1U<<1}, Service{"GetProcessHeaps",2}, Service{"HeapWalk",2}
};
// 診斷：X64ARM_TRACE_HEAP_PATHS=1 時每秒輸出 process heap 小物件與一般 heap 兩條路徑的次數及耗時。
struct HeapPathStats {
    std::uint64_t guest_alloc{},guest_alloc_ns{},general_alloc{},general_alloc_ns{},guest_free{},guest_free_ns{},
        general_free{},general_free_ns{},general_large_free{},other{},other_ns{},skip_heap{},skip_locked{},skip_flags{},skip_full{},last_heap{},last_flags{};
    std::chrono::steady_clock::time_point next{};
};
bool trace_heap_paths(){static const bool enabled=std::getenv("X64ARM_TRACE_HEAP_PATHS")!=nullptr;return enabled;}
// x86 可成長私有 heap 改用級距配置區；X64ARM_GUEST_HEAP_PRIVATE=0 停用，恢復一般 heap（同成品對照用）。
bool guest_heap_private_requested(){
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_GUEST_HEAP_PRIVATE");return !setting || std::string_view(setting)!="0";}();
    return enabled;
}
HeapPathStats heap_path_stats;
void report_heap_paths(std::uint32_t pid,std::size_t objects){
    auto& s=heap_path_stats;const auto now=std::chrono::steady_clock::now();
    if(now<s.next)return;
    if(s.next!=std::chrono::steady_clock::time_point{}){
        const auto unix_us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::fprintf(stderr,"heap_paths pid=%u unix_us=%lld guest_alloc=%llu/%llu general_alloc=%llu/%llu guest_free=%llu/%llu general_free=%llu/%llu large_free=%llu other=%llu/%llu objects=%zu skip_heap=%llu skip_locked=%llu skip_flags=%llu skip_full=%llu last_heap=0x%llx last_flags=0x%llx process_heap=0x%llx\n",
            pid,static_cast<long long>(unix_us),
            static_cast<unsigned long long>(s.guest_alloc),static_cast<unsigned long long>(s.guest_alloc_ns/1000),
            static_cast<unsigned long long>(s.general_alloc),static_cast<unsigned long long>(s.general_alloc_ns/1000),
            static_cast<unsigned long long>(s.guest_free),static_cast<unsigned long long>(s.guest_free_ns/1000),
            static_cast<unsigned long long>(s.general_free),static_cast<unsigned long long>(s.general_free_ns/1000),
            static_cast<unsigned long long>(s.general_large_free),static_cast<unsigned long long>(s.other),static_cast<unsigned long long>(s.other_ns/1000),objects,
            static_cast<unsigned long long>(s.skip_heap),static_cast<unsigned long long>(s.skip_locked),static_cast<unsigned long long>(s.skip_flags),
            static_cast<unsigned long long>(s.skip_full),static_cast<unsigned long long>(s.last_heap),static_cast<unsigned long long>(s.last_flags),
            static_cast<unsigned long long>(WindowsServices::process_heap));
    }
    s=HeapPathStats{};s.next=now+std::chrono::seconds(1);
}
}
bool WindowsServices::dispatch_system_call(GuestState& state,GuestMemory& memory){
    return dispatch_system_call(state,memory,SystemCallEntry::Syscall);
}

namespace {
// 客體端小型 CRT 常式（Windows x64 ABI，RAX 回傳；只使用 volatile 暫存器，不動 RSP）。
// 每個匯入一段（128-byte 對齊），末尾 8 bytes 為原 CRT 服務 stub 的絕對位址。超過客體處理範圍時
// RCX/RDX/R8 與 RSP 均未改動，經 `jmp [rip+slot]` 尾跳回服務，沿用宿主實作、上限與故障契約。
//   memcpy/memmove/RtlMoveMemory：長度 ≤64；不重疊或目的在來源之前時 8-byte 區塊前向、尾端逐位元組，
//     目的落在來源之後且重疊時逐位元組反向。讀寫只觸及 [位址, 位址+長度)。
//   memset：長度 ≤64，8-byte 區塊填值、尾端逐位元組。memcmp：長度 ≤16 先依序讀遍左、右範圍（與宿主相同的故障順序）再比較，
//     回傳第一個不同 byte 的差（32-bit，零擴展）。
//   strlen：逐 byte 到 8-byte 對齊後以對齊字組找 NUL（不跨頁、不設長度上限，與原生相同，不再交回服務）。strcmp：64 bytes 內出現差異或 NUL，依序先讀左再讀右。
//   strncmp：長度 >1 MiB 交宿主（宿主拒絕）；否則同 strcmp，最多 64 bytes 且不超過長度。
//   strchr：先在 64 bytes 內確定字串長度（宿主同樣先讀完整字串），再搜尋；字元 0 回傳結尾。
//   strcpy：先在 64 bytes 內確定來源長度，再複製含 NUL 的 len+1 bytes，重疊時方向同 memmove。
// 原始碼見 bench/jpeg-resident-services-2026-10-15/guest-crt-routines.s。
constexpr std::array<std::uint8_t,0x558> guest_crt_code{
    0x48,0x89,0xc8,0x49,0x83,0xf8,0x40,0x77,0x57,0x4d,0x85,0xc0,0x74,0x40,0x49,0x89,0xc9,0x49,0x29,0xd1,0x4d,0x39,0xc1,0x72,
    0x36,0x49,0x83,0xf8,0x08,0x72,0x1d,0x4c,0x8b,0x12,0x4c,0x89,0x11,0x48,0x83,0xc2,0x08,0x48,0x83,0xc1,0x08,0x49,0x83,0xe8,
    0x08,0x49,0x83,0xf8,0x08,0x73,0xe8,0x4d,0x85,0xc0,0x74,0x12,0x44,0x0f,0xb6,0x12,0x44,0x88,0x11,0x48,0xff,0xc2,0x48,0xff,
    0xc1,0x49,0xff,0xc8,0x75,0xee,0xc3,0x46,0x0f,0xb6,0x54,0x02,0xff,0x46,0x88,0x54,0x01,0xff,0x49,0xff,0xc8,0x75,0xf0,0xc3,
    0xff,0x25,0x02,0x00,0x00,0x00,0x66,0x90,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,
    0x00,0x00,0x66,0x0f,0x1f,0x44,0x00,0x00,0x48,0x89,0xc8,0x49,0x83,0xf8,0x40,0x77,0x57,0x4d,0x85,0xc0,0x74,0x40,0x49,0x89,
    0xc9,0x49,0x29,0xd1,0x4d,0x39,0xc1,0x72,0x36,0x49,0x83,0xf8,0x08,0x72,0x1d,0x4c,0x8b,0x12,0x4c,0x89,0x11,0x48,0x83,0xc2,
    0x08,0x48,0x83,0xc1,0x08,0x49,0x83,0xe8,0x08,0x49,0x83,0xf8,0x08,0x73,0xe8,0x4d,0x85,0xc0,0x74,0x12,0x44,0x0f,0xb6,0x12,
    0x44,0x88,0x11,0x48,0xff,0xc2,0x48,0xff,0xc1,0x49,0xff,0xc8,0x75,0xee,0xc3,0x46,0x0f,0xb6,0x54,0x02,0xff,0x46,0x88,0x54,
    0x01,0xff,0x49,0xff,0xc8,0x75,0xf0,0xc3,0xff,0x25,0x02,0x00,0x00,0x00,0x66,0x90,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x0f,0x1f,0x44,0x00,0x00,0x48,0x89,0xc8,0x49,0x83,0xf8,0x40,0x77,
    0x57,0x4d,0x85,0xc0,0x74,0x40,0x49,0x89,0xc9,0x49,0x29,0xd1,0x4d,0x39,0xc1,0x72,0x36,0x49,0x83,0xf8,0x08,0x72,0x1d,0x4c,
    0x8b,0x12,0x4c,0x89,0x11,0x48,0x83,0xc2,0x08,0x48,0x83,0xc1,0x08,0x49,0x83,0xe8,0x08,0x49,0x83,0xf8,0x08,0x73,0xe8,0x4d,
    0x85,0xc0,0x74,0x12,0x44,0x0f,0xb6,0x12,0x44,0x88,0x11,0x48,0xff,0xc2,0x48,0xff,0xc1,0x49,0xff,0xc8,0x75,0xee,0xc3,0x46,
    0x0f,0xb6,0x54,0x02,0xff,0x46,0x88,0x54,0x01,0xff,0x49,0xff,0xc8,0x75,0xf0,0xc3,0xff,0x25,0x02,0x00,0x00,0x00,0x66,0x90,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x0f,0x1f,0x44,0x00,0x00,
    0x48,0x89,0xc8,0x49,0x83,0xf8,0x40,0x77,0x57,0x4d,0x85,0xc0,0x74,0x40,0x49,0x89,0xc9,0x49,0x29,0xd1,0x4d,0x39,0xc1,0x72,
    0x36,0x49,0x83,0xf8,0x08,0x72,0x1d,0x4c,0x8b,0x12,0x4c,0x89,0x11,0x48,0x83,0xc2,0x08,0x48,0x83,0xc1,0x08,0x49,0x83,0xe8,
    0x08,0x49,0x83,0xf8,0x08,0x73,0xe8,0x4d,0x85,0xc0,0x74,0x12,0x44,0x0f,0xb6,0x12,0x44,0x88,0x11,0x48,0xff,0xc2,0x48,0xff,
    0xc1,0x49,0xff,0xc8,0x75,0xee,0xc3,0x46,0x0f,0xb6,0x54,0x02,0xff,0x46,0x88,0x54,0x01,0xff,0x49,0xff,0xc8,0x75,0xf0,0xc3,
    0xff,0x25,0x02,0x00,0x00,0x00,0x66,0x90,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,
    0x00,0x00,0x66,0x0f,0x1f,0x44,0x00,0x00,0x48,0x89,0xc8,0x49,0x83,0xf8,0x40,0x77,0x3e,0x4d,0x85,0xc0,0x74,0x38,0x49,0x83,
    0xf8,0x08,0x72,0x28,0x44,0x0f,0xb6,0xca,0x49,0xba,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x4d,0x0f,0xaf,0xca,0x4c,0x89,
    0x09,0x48,0x83,0xc1,0x08,0x49,0x83,0xe8,0x08,0x49,0x83,0xf8,0x08,0x73,0xef,0x4d,0x85,0xc0,0x74,0x0a,0x88,0x11,0x48,0xff,
    0xc1,0x49,0xff,0xc8,0x75,0xf6,0xc3,0xff,0x25,0x03,0x00,0x00,0x00,0x0f,0x1f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,
    0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x49,0x83,0xf8,0x10,0x77,0x42,0x31,0xc0,
    0x4d,0x85,0xc0,0x74,0x3a,0x45,0x31,0xc9,0x46,0x0f,0xb6,0x14,0x09,0x49,0xff,0xc1,0x4d,0x39,0xc1,0x72,0xf3,0x45,0x31,0xc9,
    0x46,0x0f,0xb6,0x14,0x0a,0x49,0xff,0xc1,0x4d,0x39,0xc1,0x72,0xf3,0x45,0x31,0xc9,0x42,0x0f,0xb6,0x04,0x09,0x46,0x0f,0xb6,
    0x14,0x0a,0x44,0x29,0xd0,0x75,0x08,0x49,0xff,0xc1,0x4d,0x39,0xc1,0x72,0xe9,0xc3,0xff,0x25,0x02,0x00,0x00,0x00,0x66,0x90,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,
    0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,
    0x48,0x89,0xc8,0xa8,0x07,0x74,0x0a,0x80,0x38,0x00,0x74,0x3e,0x48,0xff,0xc0,0xeb,0xf2,0x49,0xb8,0x01,0x01,0x01,0x01,0x01,
    0x01,0x01,0x01,0x49,0xb9,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x48,0x8b,0x10,0x49,0x89,0xd2,0x4d,0x29,0xc2,0x48,0xf7,
    0xd2,0x49,0x21,0xd2,0x4d,0x21,0xca,0x75,0x06,0x48,0x83,0xc0,0x08,0xeb,0xe6,0x4d,0x0f,0xbc,0xd2,0x49,0xc1,0xea,0x03,0x4c,
    0x01,0xd0,0x48,0x29,0xc8,0xc3,0xff,0x25,0x24,0x00,0x00,0x00,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x45,0x31,0xc9,0x42,0x0f,0xb6,0x04,0x09,0x46,0x0f,0xb6,0x14,0x0a,0x44,0x29,0xd0,
    0x75,0x10,0x45,0x85,0xd2,0x74,0x0b,0x49,0xff,0xc1,0x49,0x83,0xf9,0x40,0x72,0xe3,0xeb,0x01,0xc3,0xff,0x25,0x07,0x00,0x00,
    0x00,0x0f,0x1f,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,
    0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,
    0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,
    0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x90,0x49,0x81,0xf8,0x00,0x00,0x10,0x00,0x77,
    0x2a,0x45,0x31,0xc9,0x31,0xc0,0x4d,0x39,0xc1,0x73,0x1f,0x49,0x83,0xf9,0x40,0x73,0x1a,0x42,0x0f,0xb6,0x04,0x09,0x46,0x0f,
    0xb6,0x14,0x0a,0x44,0x29,0xd0,0x75,0x0a,0x45,0x85,0xd2,0x74,0x05,0x49,0xff,0xc1,0xeb,0xda,0xc3,0xff,0x25,0x07,0x00,0x00,
    0x00,0x0f,0x1f,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,
    0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,
    0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x0f,0x1f,0x44,0x00,0x00,
    0x31,0xc0,0x80,0x3c,0x01,0x00,0x74,0x0b,0x48,0xff,0xc0,0x48,0x83,0xf8,0x40,0x72,0xf1,0xeb,0x28,0x44,0x0f,0xb6,0xca,0x45,
    0x85,0xc9,0x74,0x18,0x45,0x31,0xd2,0x49,0x39,0xc2,0x73,0x14,0x46,0x38,0x0c,0x11,0x74,0x05,0x49,0xff,0xc2,0xeb,0xf0,0x4a,
    0x8d,0x04,0x11,0xc3,0x48,0x01,0xc8,0xc3,0x31,0xc0,0xc3,0xff,0x25,0x07,0x00,0x00,0x00,0x0f,0x1f,0x80,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,
    0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x66,0x2e,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,
    0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00,0x45,0x31,0xc9,0x42,0x80,0x3c,0x0a,0x00,0x74,0x0b,0x49,0xff,0xc1,0x49,0x83,0xf9,
    0x40,0x72,0xf0,0xeb,0x35,0x48,0x89,0xc8,0x49,0xff,0xc1,0x49,0x89,0xca,0x49,0x29,0xd2,0x4d,0x39,0xca,0x72,0x13,0x44,0x0f,
    0xb6,0x1a,0x44,0x88,0x19,0x48,0xff,0xc2,0x48,0xff,0xc1,0x49,0xff,0xc9,0x75,0xee,0xc3,0x46,0x0f,0xb6,0x5c,0x0a,0xff,0x46,
    0x88,0x5c,0x09,0xff,0x49,0xff,0xc9,0x75,0xf0,0xc3,0xff,0x25,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};
struct GuestMemoryRoutine {std::string_view family,symbol;unsigned entry,slot;};
constexpr std::array guest_memory_routines{
    GuestMemoryRoutine{"crt","memcpy",0x000,0x068},
    GuestMemoryRoutine{"crt","memmove",0x080,0x0e8},
    GuestMemoryRoutine{"kernel32.dll","RtlMoveMemory",0x100,0x168},
    GuestMemoryRoutine{"ntdll.dll","RtlMoveMemory",0x180,0x1e8},
    GuestMemoryRoutine{"crt","memset",0x200,0x250},
    GuestMemoryRoutine{"crt","memcmp",0x280,0x2d0},
    GuestMemoryRoutine{"crt","strlen",0x300,0x378},
    GuestMemoryRoutine{"crt","strcmp",0x380,0x3b0},
    GuestMemoryRoutine{"crt","strncmp",0x400,0x440},
    GuestMemoryRoutine{"crt","strchr",0x480,0x4c8},
    GuestMemoryRoutine{"crt","strcpy",0x500,0x550}};
// callbacks 預留 gateway 範圍內、第一頁之外的獨立唯讀可執行頁。
constexpr std::uint64_t guest_memory_page_offset=0x80000;
constexpr std::uint64_t guest_scalar_math_page_offset=0x81000;
constexpr std::uint64_t guest_thread_page_offset=0x82000;
constexpr std::uint64_t guest_tls_bitmap_page_offset=X64ARM_TLS_BITMAP_PAGE_OFFSET;
constexpr std::uint64_t guest_io_page_offset=0x8b000;
// CFG 客體快速路徑：常式頁與 4096 項已驗證目標表（直接映射，整個目標位址比對）。
// 檢查入口在 +0x00（rcx 為目標，命中 ret），dispatch 入口在 +0x40（rax 為目標，命中 jmp rax）；
// 兩者只使用 r10／r11 與旗標。未命中經 +0x80／+0x88 的 cell 跳到宿主完整檢查的服務 stub。
// 原始碼：mov %rcx,%r10; shr $4,%r10; mov %rcx,%r11; shr $20,%r11; xor %r11,%r10; and $0xfff,%r10d;
// movabs $table,%r11; cmp %rcx,(%r11,%r10,8); jne slow; ret; slow: jmp *cell(%rip)。
// 0x82000 已由客體執行緒常式使用、0x8b000／0x8c000 為 I/O 完成與配置區狀態頁；快速路徑放在其後，避免啟用時映射重疊。
constexpr std::uint64_t guard_code_page_offset=0x8d000,guard_table_page_offset=0x8e000;
constexpr std::size_t guard_table_entries=4096,guard_table_bytes=guard_table_entries*8;
static_assert(guard_code_page_offset!=guest_thread_page_offset && guard_code_page_offset!=guest_scalar_math_page_offset &&
              guard_code_page_offset!=guest_memory_page_offset && guard_code_page_offset!=guest_io_page_offset);
static_assert(guest_io_page_offset+0x1000<=guard_table_page_offset || guest_io_page_offset>=guard_table_page_offset+guard_table_bytes);
static_assert(guest_thread_page_offset+GuestMemory::guest_page_size<=guest_tls_bitmap_page_offset &&
              guest_tls_bitmap_page_offset+GuestMemory::guest_page_size<=guest_io_page_offset);
constexpr std::size_t guard_check_table_slot=0x1a,guard_dispatch_table_slot=0x5a,guard_slow_check_cell=0x80,guard_slow_dispatch_cell=0x88;
constexpr std::array<std::uint8_t,0x90> guard_fast_code{
    0x49,0x89,0xca,0x49,0xc1,0xea,0x04,0x49,0x89,0xcb,0x49,0xc1,0xeb,0x14,0x4d,0x31,
    0xda,0x41,0x81,0xe2,0xff,0x0f,0x00,0x00,0x49,0xbb,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x4b,0x39,0x0c,0xd3,0x75,0x01,0xc3,0xff,0x25,0x51,0x00,0x00,0x00,0xcc,
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0x49,0x89,0xc2,0x49,0xc1,0xea,0x04,0x49,0x89,0xc3,0x49,0xc1,0xeb,0x14,0x4d,0x31,
    0xda,0x41,0x81,0xe2,0xff,0x0f,0x00,0x00,0x49,0xbb,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x4b,0x39,0x04,0xd3,0x75,0x02,0xff,0xe0,0xff,0x25,0x18,0x00,0x00,0x00,
    0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
// 兩路組關聯：雜湊選出相鄰兩槽（同一快取列），任一槽相符即通過；未命中時宿主以 MRU 置換。
// 交替呼叫兩個同槽目標（例如 vtable 分派）不再互相擠出。來源：同目錄外的組語經 clang 組譯後抄錄，
// RIP 相對跳躍位移指向 0x80／0x88 慢路徑 cell。
constexpr std::array<std::uint8_t,0x90> guard_fast_code_two_way{
    0x49,0x89,0xca,0x49,0xc1,0xea,0x04,0x49,0x89,0xcb,0x49,0xc1,0xeb,0x14,0x4d,0x31,
    0xda,0x41,0x81,0xe2,0xfe,0x0f,0x00,0x00,0x49,0xbb,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x4f,0x8d,0x1c,0xd3,0x49,0x39,0x0b,0x74,0x06,0x49,0x39,0x4b,0x08,0x75,
    0x01,0xc3,0xff,0x25,0x48,0x00,0x00,0x00,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0x49,0x89,0xc2,0x49,0xc1,0xea,0x04,0x49,0x89,0xc3,0x49,0xc1,0xeb,0x14,0x4d,0x31,
    0xda,0x41,0x81,0xe2,0xfe,0x0f,0x00,0x00,0x49,0xbb,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x4f,0x8d,0x1c,0xd3,0x49,0x39,0x03,0x74,0x06,0x49,0x39,0x43,0x08,0x75,
    0x02,0xff,0xe0,0xff,0x25,0x0f,0x00,0x00,0x00,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
bool guard_two_way() noexcept {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_CFG_GUARD_WAYS");return !setting || std::string_view(setting)!="1";}();
    return enabled;
}
constexpr std::size_t guard_table_index(std::uint64_t target) noexcept { return static_cast<std::size_t>(((target>>4)^(target>>20))&(guard_table_entries-1)); }
// 只有撤銷可執行映射（含模組卸載）、涉及可執行區段的權限變更、快照還原與模式切換會使已驗證目標失效；
// 新增映射與可執行頁寫入不會，不因此清表。各計數只增不減，差值亦單調。
// 診斷：X64ARM_TRACE_CFG_GUARD 啟用時統計快速路徑未命中（含槽位被其他目標佔用的衝突）與整表清空次數。
struct GuardStatistics {std::uint64_t misses{},collisions{},flushes{};} guard_statistics;
// 入口累積時間診斷（X64ARM_ENTRY_RUNTIME_DIR）同時輸出此統計，供與服務入口次數對照。
bool trace_cfg_guard() noexcept {static const bool enabled=std::getenv("X64ARM_TRACE_CFG_GUARD")!=nullptr || std::getenv("X64ARM_ENTRY_RUNTIME_DIR")!=nullptr;return enabled;}
void report_guard_statistics() noexcept {if(trace_cfg_guard())std::fprintf(stderr,"cfg_guard misses=%llu collisions=%llu flushes=%llu\n",static_cast<unsigned long long>(guard_statistics.misses),static_cast<unsigned long long>(guard_statistics.collisions),static_cast<unsigned long long>(guard_statistics.flushes));}
std::uint64_t guard_table_key(const GuestMemory& memory) noexcept {
    const auto& g=memory.generation_stats();
    return g.executable_unmaps+(g.protect-g.non_executable_protect)+g.restores+g.mode_changes;
}
bool guest_memory_enabled() {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_GUEST_CRT_MEMORY");return !setting || std::string_view(setting)!="0";}();
    return enabled;
}
bool guest_scalar_math_enabled() {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_GUEST_SCALAR_MATH");return !setting || std::string_view(setting)!="0";}();
    return enabled && scalar_math_requested();
}
bool guest_strtod_enabled() {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_GUEST_STRTOD");
        return !setting || std::string_view(setting)!="0";}();
    return enabled;
}
}

void WindowsServices::initialize(GuestMemory& memory, std::u16string_view command_line, const std::filesystem::path& filesystem_root, const std::filesystem::path& c_drive, bool writable_files, const std::filesystem::path& install_root, const std::map<char,std::filesystem::path>& drives, std::string_view certificate_roots_file, bool resident_translation) {
    startup_profile::Scope phase("services","initialize",true);
    if (initialized_) throw std::logic_error("Windows 服務已初始化");
    if (command_line.size() > 32766 || command_line.find(u'\0') != std::u16string_view::npos) throw std::invalid_argument("Windows command line 過長或包含 NUL");
    (void)utf16_to_utf8(command_line);
    if (writable_files && filesystem_root.empty() && c_drive.empty() && install_root.empty() && drives.empty()) throw std::invalid_argument("read-write 模式需要 --fs-root、--c-drive 或 --install-root 映射");
    x86_ = memory.execution_mode() == ExecutionMode::X86;
    const auto& processor=native_processor_information();
    memory.set_processor_identity({processor.name,processor.nominal_mhz,processor.maximum_mhz,0});
    const auto layout=windows_address_layout::for_mode(memory.execution_mode());
    unresolved_base_=GuestAddress{layout.unresolved};stub_base_=GuestAddress{layout.services};
    callback_return_=GuestAddress{layout.callbacks};startup_entry_=GuestAddress{layout.callbacks+0x10};exception_entry_=GuestAddress{layout.callbacks+0x20};
    thread_return_=GuestAddress{layout.callbacks+0x30};thread_start_=GuestAddress{layout.callbacks+0x40};process_exit_retry_=GuestAddress{layout.callbacks+0x50};
    if (x86_) {
        command_line_address_ = GuestAddress{0x70000000}; runtime_base_ = GuestAddress{0x70010000};
        teb_base_ = GuestAddress{0x70200000}; peb_base_ = GuestAddress{0x70210000}; exception_base_ = GuestAddress{0x70300000};
        next_allocation_ = 0x10000000; runtime_.next_virtual = 0x30000000; runtime_.next_module_base = 0x50000000;
    }
    // 配置政策實驗：從低位址空洞搜尋，讓完整頁可使用既有直接頁表。
    // 不縮減 x64 位址上限；共用 allocator 仍避開映射、保留區及存活物件，必要時向高位址延伸。
    if(!x86_){
        const auto* setting=std::getenv("X64ARM_LOW_HEAP");
        if(setting && std::string_view(setting)=="1")next_allocation_=0x10000000;
        const auto* virtual_setting=std::getenv("X64ARM_LOW_VIRTUAL");
        if(virtual_setting && std::string_view(virtual_setting)=="1")runtime_.next_virtual=0x30000000;
    }
    // 客體可讀取函式入口（例如檢查 inline hook）。每個 16-byte host gateway
    // 提供可讀、不可寫的 trap 編碼；執行仍由精確入口的 services dispatch 完成。
    // 未登錄／偏移入口不會因映射存在而回傳成功，UD2 仍走非法指令契約。
    const auto gateway=[&](GuestAddress base,std::size_t requested){
        if(requested>windows_address_layout::gateway_capacity)throw std::runtime_error("Windows gateway 超出固定配置容量");
        const auto size=(requested+GuestMemory::guest_page_size-1)&~(GuestMemory::guest_page_size-1);
        std::vector<std::uint8_t> bytes(size);
        for(std::size_t n=0;n<size;n+=2){bytes[n]=0x0f;bytes[n+1]=0x0b;}
        // SYSENTER 返回固定入口的真正 RET；與 gateway 一次建立可執行映射，
        // 不用唯讀資料更新接口修改程式碼，返回位址仍由 CPU 做權限／故障檢查。
        if(x86_ && base==callback_return_ && size>0xf0)bytes[0xf0]=0xc3;
        memory.map(base,size,read_execute,bytes);
    };
    {startup_profile::Scope gateways("services","gateways",true);
        gateway(stub_base_,(services.size()+runtime_service_descriptors().size())*16);
        gateway(unresolved_base_,4096*16); // 與 bind_import 的已登錄數量上限一致。
        gateway(callback_return_,4096); // 內部回呼與 stock WNDPROC 同一入口頁。
        static const bool guest_io=[] {const auto* setting=std::getenv("X64ARM_GUEST_IO_COMPLETION");
            return !setting || std::string_view(setting)!="0";}();
        if(detail::io_wait_fastpath_enabled() && guest_io && trace_services_.empty() && !startup_profile::enabled()){
            static_assert(detail::guest_io_completion_code.size()<=GuestMemory::guest_page_size);
            static_assert(guest_io_page_offset+0x1000<=guard_table_page_offset || guest_io_page_offset>=guard_table_page_offset+guard_table_bytes);
            guest_io_completion_=GuestAddress{callback_return_.value+guest_io_page_offset};
            std::vector<std::uint8_t> bytes(GuestMemory::guest_page_size,0xcc);
            std::copy(detail::guest_io_completion_code.begin(),detail::guest_io_completion_code.end(),bytes.begin());
            memory.map(guest_io_completion_,GuestMemory::guest_page_size,read_execute,bytes);
        }
        static const bool thread_routines=[] {const auto* setting=std::getenv("X64ARM_GUEST_THREAD");
            return !setting || std::string_view(setting)!="0";}();
        if(thread_routines && trace_services_.empty() && !startup_profile::enabled()){
            static_assert(detail::guest_thread_code.size()<=GuestMemory::guest_page_size);
            static_assert(detail::guest_thread_x86_code.size()<=GuestMemory::guest_page_size);
            static_assert(std::tuple_size_v<decltype(runtime_.tls_used)> == detail::guest_thread_tls_slots);
            static_assert(detail::guest_thread_x86_tls_slots == detail::guest_thread_tls_slots);
            // 唯讀程式碼與可變 TEB 分離；每次讀取當前 FS／GS，不保存上一執行緒的值。
            guest_thread_routines_=GuestAddress{callback_return_.value+guest_thread_page_offset};
            // FlsGetValue 客體常式讀 TEB 鏡像。微基準每次約省 33 ns，但 703／103 同成品對照開啟反而慢 1–3%
            // （跨客體碼頁呼叫的成本高於常駐執行緒邊界），預設關閉；X64ARM_GUEST_FLS=1 才維護鏡像並導向常式。
            static const bool fls_routines=[] {const auto* setting=std::getenv("X64ARM_GUEST_FLS");return setting && std::string_view(setting)=="1";}();
            fls_mirror_=!x86_ && fls_routines;
            // 有慢路徑 cell 的常式（SRW 鎖）在初始化填入同名 kernel32 服務 stub；之後程式頁保持唯讀可執行。
            const auto table=runtime_service_descriptors();
            std::vector<std::uint8_t> bytes(GuestMemory::guest_page_size,0xcc);
            const auto code=x86_?std::span<const std::uint8_t>{detail::guest_thread_x86_code}:
                                  std::span<const std::uint8_t>{detail::guest_thread_code};
            std::copy(code.begin(),code.end(),bytes.begin());
            // 配置生命週期由協調者管理，讀寫槽本身留在客體。獨立共享頁不佔用 TEB／FLS 鏡像，
            // 新執行緒不需複製配置狀態；其新映射 TEB 的 TLS 值仍由零開始。
            guest_tls_bitmap_=GuestAddress{callback_return_.value+guest_tls_bitmap_page_offset};
            std::vector<std::uint8_t> bitmap(GuestMemory::guest_page_size);
            for(std::size_t index=0;index<runtime_.tls_used.size();++index)
                if(runtime_.tls_used[index])bitmap[index/8]|=static_cast<std::uint8_t>(1U<<(index%8));
            memory.map(guest_tls_bitmap_,GuestMemory::guest_page_size,read_write,bitmap);
            const auto bitmap_patch=x86_?detail::guest_thread_x86_tls_bitmap_patch:detail::guest_thread_tls_bitmap_patch;
            const unsigned bitmap_width=x86_?4U:8U;
            if(bitmap_patch+bitmap_width>code.size() || (x86_ && guest_tls_bitmap_.value>UINT32_MAX))
                throw std::logic_error("TLS 配置位元圖位址修補超出常式或 Win32 範圍");
            for(unsigned n=0;n<bitmap_width;++n)bytes[bitmap_patch+n]=static_cast<std::uint8_t>(guest_tls_bitmap_.value>>(8*n));
            const auto patch_services=[&](const auto& routines,unsigned width){for(const auto& routine:routines){
                if(!routine.slot)continue;
                const auto found=std::find_if(table.begin(),table.end(),[&](const auto& d){return d.family=="kernel32.dll" && d.symbol==routine.symbol;});
                if(found==table.end() || routine.slot+width>code.size())throw std::logic_error("客體執行緒常式的原服務或慢路徑不存在");
                const auto stub=stub_base_.value+(services.size()+static_cast<std::size_t>(found-table.begin()))*16;
                if(width==4 && stub>UINT32_MAX)throw std::logic_error("客體執行緒常式的原服務超出 Win32 範圍");
                for(unsigned n=0;n<width;++n)bytes[routine.slot+n]=static_cast<std::uint8_t>(stub>>(8*n));
            }};
            if(x86_)patch_services(detail::guest_thread_x86_routines,4);
            else patch_services(detail::guest_thread_routines,8);
            memory.map(guest_thread_routines_,GuestMemory::guest_page_size,read_execute,bytes);
        }
        if(!x86_ && guest_memory_enabled()){
            // 小型記憶體常式以客體碼提供；長度較大時尾跳的目的地即原服務 stub。
            const auto table=runtime_service_descriptors();
            std::vector<std::uint8_t> bytes(GuestMemory::guest_page_size,0xcc);
            std::copy(guest_crt_code.begin(),guest_crt_code.end(),bytes.begin());
            for(const auto& routine:guest_memory_routines){
                const auto found=std::find_if(table.begin(),table.end(),[&](const auto& d){return d.family==routine.family && d.symbol==routine.symbol;});
                if(found==table.end())throw std::logic_error("客體 CRT 常式缺少對應服務");
                const auto stub=stub_base_.value+(services.size()+static_cast<std::size_t>(found-table.begin()))*16;
                for(unsigned n=0;n<8;++n)bytes[routine.slot+n]=static_cast<std::uint8_t>(stub>>(8*n));
            }
            guest_memory_routines_=GuestAddress{callback_return_.value+guest_memory_page_offset};
            memory.map(guest_memory_routines_,GuestMemory::guest_page_size,read_execute,bytes);
        }
        // PE32 沒有客體常式頁；process heap 的小型配置仍可由服務端使用同一套 chunk 級距配置區。
        if(x86_)(void)guest_heap_setup(memory);
        if(!x86_ && guest_scalar_math_enabled()){
            // 沿用客體 CRT 常式與 resolver；一般數值不經 HostServices 或物化完整 GuestState。
            // 只在初始化填入各自原服務的慢路徑，之後程式頁保持唯讀且可執行。
            static_assert(detail::guest_scalar_math_code.size()<=GuestMemory::guest_page_size);
            const auto table=runtime_service_descriptors();
            std::vector<std::uint8_t> bytes(GuestMemory::guest_page_size,0xcc);
            std::copy(detail::guest_scalar_math_code.begin(),detail::guest_scalar_math_code.end(),bytes.begin());
            // 小物件配置區：狀態頁位址寫入配置常式慢路徑 slot 前 8 bytes 的 cell；未啟用時保持 0，常式一律交回原服務。
            const bool heap=guest_heap_setup(memory);
            const bool locale_lock=guest_locale_lock_setup(memory);
            for(const auto& routine:detail::guest_scalar_math_routines){
                const auto found=std::find_if(table.begin(),table.end(),[&](const auto& d){return d.family=="crt" && d.symbol==routine.symbol;});
                if(found==table.end() || routine.slot+8>detail::guest_scalar_math_code.size())throw std::logic_error("客體純量常式的原服務或慢路徑不存在");
                const auto stub=stub_base_.value+(services.size()+static_cast<std::size_t>(found-table.begin()))*16;
                for(unsigned n=0;n<8;++n)bytes[routine.slot+n]=static_cast<std::uint8_t>(stub>>(8*n));
                const bool heap_routine=routine.symbol=="malloc" || routine.symbol=="free" || routine.symbol=="_aligned_malloc" || routine.symbol=="_aligned_free";
                if(heap && heap_routine)for(unsigned n=0;n<8;++n)bytes[routine.slot-8+n]=static_cast<std::uint8_t>(guest_heap_state_.value>>(8*n));
                const bool locale_routine=routine.symbol=="_lock_locales" || routine.symbol=="_unlock_locales";
                if(locale_lock && locale_routine)for(unsigned n=0;n<8;++n)bytes[routine.slot-8+n]=static_cast<std::uint8_t>(guest_locale_lock_state_.value>>(8*n));
            }
            guest_scalar_math_routines_=GuestAddress{callback_return_.value+guest_scalar_math_page_offset};
            memory.map(guest_scalar_math_routines_,GuestMemory::guest_page_size,read_execute,bytes);
        }
        initialize_guest_crt_algorithms(memory,GuestAddress{stub_base_.value+services.size()*16});
        // 常駐執行器直接接續已驗證目標，省去完整服務邊界；明確 0／1 仍可覆寫。
        // 傳統平行批次曾因頻繁喚醒增加成本，呼叫端未選定常駐執行器時維持原預設。
        // 在載入初始化時完成配置，不在執行中改寫已被轉譯器當成常數的 CFG cell。
        static const int guard_setting=[] {const auto* setting=std::getenv("X64ARM_CFG_FAST_PATH");return setting?(std::string_view(setting)=="1"?1:0):-1;}();
        const bool guard_fast_path=guard_setting>=0?guard_setting==1:resident_translation;
        if(!x86_ && trace_services_.empty() && guard_fast_path){
            // CFG 檢查／dispatch 的客體快速路徑：以已驗證目標表避免每次間接呼叫都離開 JIT 進入服務分派。
            // 未命中仍跳到原服務 stub 做完整檢查並填表；映射撤銷或權限變更後整表清空（refresh_guard_table）。
            std::vector<std::uint8_t> code(GuestMemory::guest_page_size,0xcc);
            if(guard_two_way())std::copy(guard_fast_code_two_way.begin(),guard_fast_code_two_way.end(),code.begin());
            else std::copy(guard_fast_code.begin(),guard_fast_code.end(),code.begin());
            guard_fast_code_=GuestAddress{callback_return_.value+guard_code_page_offset};
            guard_fast_table_=GuestAddress{callback_return_.value+guard_table_page_offset};
            const auto patch=[&](std::size_t at,std::uint64_t value){for(unsigned n=0;n<8;++n)code[at+n]=static_cast<std::uint8_t>(value>>(8*n));};
            patch(guard_check_table_slot,guard_fast_table_.value);patch(guard_dispatch_table_slot,guard_fast_table_.value);
            patch(guard_slow_check_cell,callback_return_.value+0x60);patch(guard_slow_dispatch_cell,callback_return_.value+0x70);
            memory.map(guard_fast_code_,GuestMemory::guest_page_size,read_execute,code);
            // 空項為全 1（非正規位址），不會與任何合法目標相等。
            memory.map(guard_fast_table_,guard_table_bytes,read_write,std::vector<std::uint8_t>(guard_table_bytes,0xff));
            guard_table_generation_=guard_table_key(memory);
        }}
    auto files = [&]{startup_profile::Scope filesystem("services","filesystem",true);
        return ReadOnlyFiles(filesystem_root, c_drive, writable_files, install_root, drives);}();
    files.set_certificate_roots_file(certificate_roots_file);
    files.set_dos_path_normalization(true);
    // Windows 的相對路徑必須由明確的客體工作目錄解析，不能擴張 backend 的相對根目錄能力。
    if(filesystem_root.empty() && !c_drive.empty()){
        const auto error=files.set_current_directory("C:/");
        if(error)throw std::runtime_error("無法設定 Windows 初始工作目錄，錯誤 "+std::to_string(error));
    }
    const auto ensure_directory=[](ReadOnlyFiles& target,std::string_view directory){
        const auto path="C:/"+std::string(directory);const auto info=target.information(path);
        if(!info.error){if(!(info.metadata.attributes&16))throw std::runtime_error("Windows 系統路徑不是目錄："+path);return;}
        if(info.error==5)return;
        if(info.error!=2 && info.error!=3)throw std::runtime_error("無法查詢 Windows 系統目錄："+path+"，錯誤 "+std::to_string(info.error));
        const auto error=target.create_directory(path);
        if(error && error!=183 && error!=5)throw std::runtime_error("無法建立 Windows 系統目錄："+path+"，錯誤 "+std::to_string(error));
    };
    // 共用安裝目錄保留原本的系統初始化權限，不能套用一般使用者的
    // default DACL，否則 LocalSystem 服務會失去存取新 Sandbox 的權限。
    if(writable_files && !c_drive.empty()){
        startup_profile::Scope directories("services","system_directories",true);
        security::NativeFileIdentityScope identities;
        for(const auto directory:{"Program Files","Program Files/Common Files","Program Files (x86)","Program Files (x86)/Common Files"})ensure_directory(files,directory);
    }
    const auto bytes = utf16_bytes(command_line);
    memory.map(command_line_address_, (bytes.size()+4095)&~std::size_t{4095}, read_write, bytes);
    filesystem_ = std::move(files); initialize_runtime(memory, command_line); initialized_ = true;
    {startup_profile::Scope security("services","security_context",true);refresh_file_security_context();}
    if(writable_files && !c_drive.empty()){
        startup_profile::Scope directories("services","profile_directories",true);
        // 同一批初始化重用成功的 SID／UUID 轉換；每次檔案授權仍讀取當前 ACL。
        // 離開本批次即清除，不把身分或權限結果跨客體呼叫保存。
        security::NativeFileIdentityScope identities;
        // 先載入客體權杖，再查詢與建立標準目錄；不能以尚未初始化的
        // 宿主身分判斷已存在的 Windows DACL。保留使用者明確限制的路徑。
        for(const auto directory:{"Users","Users/User"})ensure_directory(filesystem_,directory);
        const auto& token=runtime_.token_objects.at(1);
        initialize_windows_profile_security(filesystem_,"C:/Users/User",token.user.sid,token.primary_group);
        // 新子目錄必須從已初始化的 profile 繼承使用者權限。
        for(const auto directory:{"Users/User/AppData","Users/User/AppData/Local","Users/User/AppData/Local/Temp","Users/User/AppData/Roaming","Users/User/Desktop","Users/User/Documents","Users/User/Downloads"})ensure_directory(filesystem_,directory);
    }
    runtime_.process_creation_time=runtime_.filetime_base;
    memory.map(GuestAddress{0x7ffe0000}, GuestMemory::guest_page_size, permission(Access::Read));
    retired(memory, 0);
}
std::u16string WindowsServices::map_executable(const std::filesystem::path& path) {
    return utf8_to_utf16(filesystem_.map_executable(path));
}
GuestAddress WindowsServices::resolve(std::string_view dll, std::string_view symbol) const {
    return resolve_service(dll,symbol,true);
}
// 匯出列舉的未命中屬正常查詢；共用解析規則，但不配置例外或展開堆疊。
GuestAddress WindowsServices::resolve_service(std::string_view dll,std::string_view symbol,bool required) const {
    std::string lower(dll);
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if(angle_module(lower) && (!native_angle_ || !native_angle_->supports(symbol))){if(!required)return GuestAddress{};throw std::invalid_argument("ANGLE Metal 入口未啟用："+lower+"!"+std::string(symbol));}
    if(native_library_module(lower) && !native_libraries_.enabled(lower)){if(!required)return GuestAddress{};throw std::invalid_argument("原生函式庫代理未啟用："+lower+"!"+std::string(symbol));}
    if(const auto host=windows_api_set_host(lower);host!=lower)lower=host;
    if(lower=="ntdll.dll" && symbol.starts_with("Zw")){const auto native=std::string("Nt")+std::string(symbol.substr(2));if(const auto target=resolve_service(lower,native,false);target.value)return target; }
    static const auto aliases=[] {
        std::map<std::pair<std::string_view,std::string_view>,const detail::ServiceExportAlias*> index;
        for(const auto& alias:detail::service_export_aliases)index.emplace(std::pair{alias.family,alias.symbol},&alias);
        return index;
    }();
    const auto alias_family=lower=="wsock32.dll" && symbol!="#1141" && symbol!="#1142"?std::string_view{"ws2_32.dll"}:std::string_view{lower};
    if(const auto found=aliases.find({alias_family,symbol});found!=aliases.end()){
        const auto& alias=*found->second;lower=alias.target_family;symbol=alias.target_symbol;
    }
    if (lower == "wsock32.dll") lower = "ws2_32.dll";
    // 舊版 CNG 匯入程式庫亦經 NCrypt provider 匯出 BCrypt 入口。
    if(lower=="ncrypt.dll" && symbol.starts_with("BCrypt"))lower="bcrypt.dll";
    if(lower=="oleaut32.dll" && symbol.starts_with('#')) {
        unsigned ordinal=0;
        for(const auto c:symbol.substr(1)){if(c<'0' || c>'9' || ordinal>6553){if(!required)return GuestAddress{};throw std::invalid_argument("OLE ordinal 不合法");}ordinal=ordinal*10+static_cast<unsigned>(c-'0');}
        if(ordinal>65535){if(!required)return GuestAddress{};throw std::invalid_argument("OLE ordinal 不合法");}
        const auto named=ole_ordinal_name(static_cast<std::uint16_t>(ordinal));if(!named.empty())symbol=named;
    }
    if (lower == "kernel32.dll"){
        if(guest_thread_routines_.value && runtime_.image.base && trace_services_.empty() && !startup_profile::enabled()){
            const auto entries=[](const auto& source) {
                std::unordered_map<std::string_view,std::size_t> table;
                for(const auto& routine:source)table.try_emplace(routine.symbol,routine.entry);
                return table;
            };
            static const auto win32_routines=entries(detail::guest_thread_x86_routines);
            static const auto win64_routines=entries(detail::guest_thread_routines);
            // SRW 家族可獨立同成品對照；初始化、等待者／喚醒與錯誤仍沿原 provider。
            static const bool srw_routines=[] {const auto* value=std::getenv("X64ARM_GUEST_SRW");return !value || std::string_view(value)!="0";}();
            const auto& routines=x86_?win32_routines:win64_routines;
            if(const auto found=routines.find(symbol);found!=routines.end() &&
               (srw_routines || symbol.find("SRWLock")==symbol.npos) && (fls_mirror_ || !symbol.starts_with("FlsGetValue")))
                return GuestAddress{guest_thread_routines_.value+found->second};
        }
        // 核心服務與擴充 descriptor 相同：只索引不可變身分，保留首筆優先。
        static const auto core_index=[] {
            std::unordered_map<std::string_view,std::size_t> table;
            for(std::size_t i=0;i<services.size();++i)table.try_emplace(services[i].name,i);
            return table;
        }();
        if(const auto found=core_index.find(symbol);found!=core_index.end())
            return GuestAddress{stub_base_.value+found->second*16};
    }
    // D3DX9／10 的純數學匯出具有相同 ABI；限定已實作的數學服務，保留各版繪圖入口。
    if(lower=="d3dx10_43.dll")for(const auto& entry:math_service_descriptors())
        if(entry.family=="d3dx9_43.dll" && entry.symbol==symbol){lower="d3dx9_43.dll";break;}
    const auto family = crt_library(lower) ? "crt" : lower;
    if(lower=="msvcp100.dll" || lower=="msvcp140.dll")if(const auto data=msvcp100_data(symbol,lower=="msvcp140.dll");data.value)return data;
    if(lower=="msvcrt.dll"){
        if(symbol=="__pioinfo")return GuestAddress{runtime_.lowio_table};
        if(symbol=="__badioinfo")return GuestAddress{runtime_.bad_ioinfo};
    }
    if (family == "crt") {
        // 舊 MSVCRT 與 UCRT 的 locale 前綴不同；共用物件管理與 NLS，保留各自的客體 ABI。
        if(lower=="msvcrt.dll" || lower.starts_with("msvcr")){
            if(symbol=="_create_locale" || symbol=="__create_locale")symbol="__x64arm_create_locale_legacy";
            else if(symbol=="_wcreate_locale")symbol="__x64arm_wcreate_locale_legacy";
            else if(symbol=="_get_current_locale" || symbol=="__get_current_locale")symbol="__x64arm_current_locale_legacy";
        }
        // UCRT 私有轉送入口保留原函式 ABI，直接共用已實作的公開服務。
        if(lower=="api-ms-win-crt-private-l1-1-0.dll" && symbol.starts_with("_o_"))symbol.remove_prefix(3);
        // MSVCRT 的 C++ 配置入口共用程序 heap、釋放檢查與 new-handler 重試。
        if(symbol=="??2@YAPAXI@Z" || symbol=="??_U@YAPAXI@Z" || symbol=="??2@YAPEAX_K@Z" || symbol=="??_U@YAPEAX_K@Z")symbol="malloc";
        if(symbol=="??3@YAXPAX@Z" || symbol=="??_V@YAXPAX@Z" || symbol=="??3@YAXPEAX@Z" || symbol=="??_V@YAXPEAX@Z")symbol="free";
        // C++14 sized delete 仍使用同一個 heap；大小由配置器保存，cdecl 多餘參數由 caller 清理。
        if(symbol=="??3@YAXPAXI@Z" || symbol=="??_V@YAXPAXI@Z" || symbol=="??3@YAXPEAX_K@Z" || symbol=="??_V@YAXPEAX_K@Z")symbol="free";
        // 系統 msvcrt 的 x86 舊式匯出仍使用 32-bit time_t；UCRT 的無後綴入口使用 64-bit。
        if(x86_ && legacy_32bit_time(lower))for(const auto& [name,alias]:{std::pair{"time","_time32"},std::pair{"gmtime","_gmtime32"},std::pair{"localtime","_localtime32"},std::pair{"mktime","_mktime32"},std::pair{"_mkgmtime","_mkgmtime32"},std::pair{"difftime","_difftime32"},std::pair{"ctime","_ctime32"},std::pair{"_wctime","_wctime32"}})if(symbol==name){symbol=alias;break;}
        if(symbol=="_calloc_crt" || symbol=="_calloc_base")symbol="calloc";
        if(symbol=="getwc")symbol="fgetwc";
        if(symbol=="putwc")symbol="fputwc";
        if(symbol=="strtoumax")symbol="strtoull";
        if(symbol=="strtoimax")symbol="strtoll";
        if(symbol=="_hypot")symbol="hypot";
        if(symbol=="__p__timezone")symbol="__timezone";
        if(symbol=="__p__daylight")symbol="__daylight";
        if(symbol=="__p__tzname")symbol="__tzname";
        if(symbol=="_iob")return GuestAddress{runtime_base_.value+0x800};
        if (symbol == "__set_app_type") symbol = "_set_app_type";
        const std::array data{
            std::pair{"__argc", 0U}, std::pair{"__argv", 8U},
            std::pair{"__wargv", 56U}, std::pair{"__initenv", 112U},
            std::pair{"_environ", 16U}, std::pair{"__winitenv", 120U},
            std::pair{"_wenviron", 64U}, std::pair{"_fmode", 24U},
            std::pair{"_commode", 32U}, std::pair{"_acmdln", 48U}, std::pair{"_wcmdln", 104U},
            // 軟體 x87 除法不具有早期 Pentium FDIV 缺陷；保留 CRT 可寫資料 ABI。
            std::pair{"_adjust_fdiv", 72U}, std::pair{"_timezone",80U},
            std::pair{"_daylight",84U}, std::pair{"_tzname",88U}
        };
        for (const auto& [name, offset] : data) if (symbol == name) return GuestAddress{runtime_base_.value + offset};
    }
    // descriptor 與 stub 序號在程序期間固定；保留重複登錄時第一筆優先的語義。
    static const auto index=[] {
        using Key=std::pair<std::string_view,std::string_view>;
        struct Hash {std::size_t operator()(const Key& key)const noexcept {
            const auto first=std::hash<std::string_view>{}(key.first),second=std::hash<std::string_view>{}(key.second);
            return first^(second+0x9e3779b9U+(first<<6)+(first>>2));
        }};
        std::unordered_map<Key,std::size_t,Hash> result;
        const auto table=runtime_service_descriptors();
        result.reserve(table.size());
        for(std::size_t i=0;i<table.size();++i)result.try_emplace(std::pair{table[i].family,table[i].symbol},i);
        return result;
    }();
    // DLL 明確登錄的版本 ABI 優先於 CRT 家族別名；例如 FH4 只屬於 vcruntime140_1。
    // 客體端常式只取代匯入位址；追蹤服務時維持宿主入口，讓每次呼叫仍可觀察。
    const auto routine=[&](std::size_t index){
        const auto& descriptor=runtime_service_descriptors()[index];
        if(guest_crt_algorithms_.base && trace_services_.empty() && descriptor.family=="crt")
            if(const auto found=guest_crt_algorithms_.export_names.find(std::string(descriptor.symbol));
               found!=guest_crt_algorithms_.export_names.end())
                return GuestAddress{guest_crt_algorithms_.base+found->second};
        if(guest_memory_routines_.value && trace_services_.empty())
            for(const auto& entry:guest_memory_routines)if(entry.family==descriptor.family && entry.symbol==descriptor.symbol)
                return GuestAddress{guest_memory_routines_.value+entry.entry};
        if(guest_scalar_math_routines_.value && trace_services_.empty() && descriptor.family=="crt" &&
           (descriptor.symbol!="strtod" || guest_strtod_enabled()))
            for(const auto& entry:detail::guest_scalar_math_routines)if(entry.symbol==descriptor.symbol)
                return GuestAddress{guest_scalar_math_routines_.value+entry.entry};
        return GuestAddress{stub_base_.value+(services.size()+index)*16};
    };
    if(lower!=family)if(const auto exact=index.find({lower,symbol});exact!=index.end())return routine(exact->second);
    if(const auto found=index.find({family,symbol});found!=index.end())return routine(found->second);
    if(!required)return GuestAddress{};
    throw std::invalid_argument("未支援的 PE import：" + std::string(dll) + "!" + std::string(symbol));
}
// 名稱索引只保存不可變契約，不保存任何程序地址、架構或原生 provider 狀態。
std::map<std::string,std::uint64_t> WindowsServices::builtin_export_targets(std::string_view family) const {
    startup_profile::Scope measurement("builtin_exports",family,true);
    static const auto names=[] {
        std::map<std::string_view,std::set<std::string>> index;
        std::set<std::string_view> all;
        for(const auto& service:services){index["kernel32.dll"].emplace(service.name);all.emplace(service.name);}
        for(const auto& service:runtime_service_descriptors()){
            index[service.family].emplace(service.symbol);all.emplace(service.symbol);
            if(service.family=="ntdll.dll" && service.symbol.starts_with("Nt"))
                index[service.family].emplace("Zw"+std::string(service.symbol.substr(2)));
            if(service.family=="bcrypt.dll" && service.symbol.starts_with("BCrypt"))index["ncrypt.dll"].emplace(service.symbol);
        }
        for(const auto& alias:detail::service_export_aliases)
            if(alias.symbol.starts_with('#') || all.contains(alias.symbol) || alias.family=="ntdll.dll")
                index[alias.family].emplace(alias.symbol);
        for(const auto& [ordinal,name]:detail::ole_ordinal_exports){(void)name;index["oleaut32.dll"].emplace("#"+std::to_string(ordinal));}
        for(const auto& service:math_service_descriptors())if(service.family=="d3dx9_43.dll")index["d3dx10_43.dll"].emplace(service.symbol);
        // CRT 別名仍經 resolve_service 選擇版本及 x86 ABI；只加入既有可列舉名稱。
        for(const auto symbol:{"_create_locale","__create_locale","_wcreate_locale","_get_current_locale","__get_current_locale",
            "??2@YAPAXI@Z","??_U@YAPAXI@Z","??2@YAPEAX_K@Z","??_U@YAPEAX_K@Z",
            "??3@YAXPAX@Z","??_V@YAXPAX@Z","??3@YAXPEAX@Z","??_V@YAXPEAX@Z",
            "??3@YAXPAXI@Z","??_V@YAXPAXI@Z","??3@YAXPEAX_K@Z","??_V@YAXPEAX_K@Z",
            "getwc","putwc","strtoumax","strtoimax","_hypot","__p__timezone","__p__daylight","__p__tzname"})
            if(all.contains(symbol))index["crt"].emplace(symbol);
        for(const auto symbol:{"_calloc_crt","_calloc_base","__set_app_type"})index["crt"].emplace(symbol);
        for(const auto symbol:all)if(symbol.starts_with("_o_"))index["api-ms-win-crt-private-l1-1-0.dll"].emplace(symbol);
        index["wsock32.dll"].insert(index["ws2_32.dll"].begin(),index["ws2_32.dll"].end());
        return index;
    }();
    std::map<std::string,std::uint64_t> result;
    const auto guest_entry=[&](GuestAddress target){
        if(guest_memory_routines_.value)
            for(const auto& entry:guest_memory_routines)if(target.value==guest_memory_routines_.value+entry.entry)return true;
        if(guest_scalar_math_routines_.value)
            for(const auto& entry:detail::guest_scalar_math_routines)if(target.value==guest_scalar_math_routines_.value+entry.entry)return true;
        if(guest_crt_algorithms_.base)
            for(const auto& [name,entry]:guest_crt_algorithms_.export_names){
                (void)name;if(target.value==guest_crt_algorithms_.base+entry)return true;
            }
        return false;
    };
    const auto add_family=[&](std::string_view name){
        const auto found=names.find(name);if(found==names.end())return;
        for(const auto& symbol:found->second){
            const auto target=resolve_service(family,symbol,false);
            // 客體函式仍是同一個 CRT 匯出；只有精確入口可發布，不把資料槽當成函式。
            if(target.value && (handles(target) || guest_entry(target)))result.try_emplace(symbol,target.value);
        }
    };
    add_family(windows_api_set_host(family));
    if(crt_library(family))add_family("crt");
    return result;
}
GuestAddress WindowsServices::bind_import(std::string_view dll, std::string_view symbol, bool trap_unresolved) {
    try { return resolve(dll, symbol); }
    catch (const std::invalid_argument&) { if (!trap_unresolved) throw; }
    const auto key = std::string(dll) + "!" + std::string(symbol);
    auto found = std::find(unresolved_imports_.begin(), unresolved_imports_.end(), key);
    if (found == unresolved_imports_.end()) {
        if (unresolved_imports_.size() >= 4096) throw std::invalid_argument("未解析 import 超過上限");
        unresolved_imports_.push_back(key); found = std::prev(unresolved_imports_.end());
    }
    return GuestAddress{unresolved_base_.value + static_cast<std::uint64_t>(found - unresolved_imports_.begin()) * 16};
}
void WindowsServices::complete_call(GuestState& state, GuestMemory& memory, std::uint64_t value) {
    if (!x86_) { WindowsCall(state,memory,0).return_to(state,value);return; }
    if (!stock_gui_class(state.rip.value).empty()) { WindowsCall(state,memory,4).return_to(state,value);return; }
    if (state.rip.value >= stub_base_.value && (state.rip.value-stub_base_.value)%16 == 0) {
        const auto index=(state.rip.value-stub_base_.value)/16;
        if(index<services.size()){WindowsCall(state,memory,services[index].arguments,true,services[index].wide_arguments).return_to(state,value);return;}
        const auto extended=runtime_service_descriptors();
        if(index-services.size()<extended.size()){WindowsCall(state,memory,extended[index-services.size()]).return_to(state,value);return;}
    }
    throw HostCallFault("Win32 非同步呼叫缺少原始 API 入口，無法決定堆疊清理量");
}
void WindowsServices::install_guard(GuestMemory& memory, const RuntimeImage& image) {
    if(!image.guard_cf)return;
    // GetProcessMitigationPolicy 的 EnableXfg 維持關閉；所有相容入口仍做實際 CFG 檢查。
    // x64 指向客體快速路徑（已驗證目標直接通過，其餘仍由宿主完整檢查）；x86 或追蹤時直接指向服務 stub。
    const auto check=guard_fast_code_.value?guard_fast_code_.value:callback_return_.value+0x60;
    const auto dispatch=guard_fast_code_.value?guard_fast_code_.value+0x40:callback_return_.value+0x70;
    for(const auto& [cell,target]:std::array<std::pair<std::uint64_t,std::uint64_t>,5>{{{image.guard_check_cell,check},{image.guard_dispatch_cell,dispatch},{image.guard_xfg_check_cell,check},{image.guard_xfg_dispatch_cell,dispatch},{image.guard_xfg_table_dispatch_cell,dispatch}}}){
        if(!cell)continue;const unsigned unit=x86_?4U:8U;std::array<std::uint8_t,8> data{};for(unsigned n=0;n<unit;++n)data[n]=static_cast<std::uint8_t>(target>>(n*8));
        // 與 IAT 一樣只在新映像初始化期間寫入；保留唯讀頁的穩定性，
        // 同頁其他 loader patch 仍可完成，客體不能改寫 CFG 入口。
        memory.initialize_readonly(GuestAddress{cell},std::span(data).first(unit));
    }
}
bool WindowsServices::guard_target(const GuestMemory& memory,std::uint64_t target) const {
    if(target>=stub_base_.value && target-stub_base_.value<(services.size()+runtime_service_descriptors().size())*16 && (target-stub_base_.value)%16==0)return true;
    if(!stock_gui_class(target).empty())return true;
    const auto mapping=memory.query(GuestAddress{target});if(!mapping || !(mapping->permissions&permission(Access::Execute)))return false;
    const auto* image=module_image(target,true);if(!image || !image->guard_cf)return true;
    const auto rva=static_cast<std::uint32_t>(target-image->base);
    const auto first=std::lower_bound(image->guard_targets.begin(),image->guard_targets.end(),rva&~15U);
    for(auto at=first;at!=image->guard_targets.end() && (*at&~15U)==(rva&~15U);++at)if(*at==rva || (*at&15U))return true;
    return false;
}
void WindowsServices::remember_guard_target(GuestMemory& memory,std::uint64_t target) noexcept {
    if(!guard_fast_table_.value)return;
    std::array<std::uint8_t,8> bytes{};for(unsigned n=0;n<8;++n)bytes[n]=static_cast<std::uint8_t>(target>>(8*n));
    try{
        if(guard_two_way()){
            // 新目標放在第一槽，原第一槽移到第二槽；兩槽都只放已驗證的目標，中途被讀到也只是較舊的有效內容。
            const auto set=guard_fast_table_.value+(guard_table_index(target)&~std::size_t{1})*8;
            const auto first=memory.load(GuestAddress{set},64);
            if(first==target || memory.load(GuestAddress{set+8},64)==target)return;
            std::array<std::uint8_t,8> moved{};for(unsigned n=0;n<8;++n)moved[n]=static_cast<std::uint8_t>(first>>(8*n));
            memory.write(GuestAddress{set+8},moved);memory.write(GuestAddress{set},bytes);
        }else memory.write(GuestAddress{guard_fast_table_.value+guard_table_index(target)*8},bytes);
    }catch(const std::exception&){}
}
void WindowsServices::refresh_guard_table(GuestMemory& memory) noexcept {
    // 已驗證目標可能已失效：在客體恢復執行前整表清空，之後由完整檢查重新填入。
    if(!guard_fast_table_.value || guard_table_generation_==guard_table_key(memory))return;
    guard_table_generation_=guard_table_key(memory);++guard_statistics.flushes;
    static const std::vector<std::uint8_t> empty(guard_table_bytes,0xff);
    try{memory.write(guard_fast_table_,empty);}catch(const std::exception&){}
}
bool WindowsServices::handles(GuestAddress address) const noexcept {
    // 所有服務與 callback 入口都位於這個保留地址區，且以 16 bytes 對齊。
    // 一般客體程式碼不必呼叫控制項查詢或取得完整 API 表。
    if (address.value < unresolved_base_.value || (address.value & 15U)) return false;
    if(!stock_gui_class(address.value).empty())return true;
    if (address.value >= unresolved_base_.value && address.value - unresolved_base_.value < unresolved_imports_.size() * 16 &&
        (address.value - unresolved_base_.value) % 16 == 0) return true;
    if(address.value==callback_return_.value+0xe0 || address.value==callback_return_.value+0x60 || address.value==callback_return_.value+0x70)return true;
    return address.value == callback_return_.value+0xa0 || address.value == callback_return_.value+0x80 || address == thread_start_ || address == thread_return_ || address == process_exit_retry_ || address == callback_return_ || address == startup_entry_ || address == exception_entry_ || (address.value >= stub_base_.value && address.value - stub_base_.value < (services.size() + runtime_service_descriptors().size()) * 16 &&
        (address.value - stub_base_.value) % 16 == 0);
}
std::string_view WindowsServices::name(GuestAddress address) const noexcept {
    if(!stock_gui_class(address.value).empty())return "Windows 控制項 WNDPROC";
    if (address.value >= unresolved_base_.value && address.value - unresolved_base_.value < unresolved_imports_.size() * 16 &&
        (address.value - unresolved_base_.value) % 16 == 0) return unresolved_imports_[(address.value - unresolved_base_.value) / 16];
    if(address.value==callback_return_.value+0xe0)return "NT syscall return";
    if(address.value==callback_return_.value+0x60)return "CFG call target 檢查";
    if(address.value==callback_return_.value+0x70)return "CFG dispatch";
    if (address.value == callback_return_.value+0xa0) return "Windows 強制執行緒結束";
    if (address == thread_start_) return "Windows 執行緒啟動";
    if (address.value == callback_return_.value+0x80) return "Windows Fiber 返回";
    if (address == thread_return_) return "Windows 執行緒返回";
    if (address == process_exit_retry_) return "Windows 程序結束等待";
    if (address == exception_entry_) return "Windows 例外派送";
    if (address == callback_return_) return "客體 callback 返回";
    if (address == startup_entry_) return "PE TLS 啟動";
    if (!handles(address)) return {};
    const auto index = (address.value - stub_base_.value) / 16;
    return index < services.size() ? services[index].name : runtime_service_descriptors()[index - services.size()].symbol;
}
bool WindowsServices::same_state(const WindowsServices& other) const {
    if(runtime_.machine_clock || other.runtime_.machine_clock)return false; // 共用機器 UTC 含跨程序狀態。
    if(detached_console_!=other.detached_console_)return false;
    if(filesystem_.certificate_roots_file()!=other.filesystem_.certificate_roots_file())return false;
    if(native_operations_!=other.native_operations_ || next_native_operation_!=other.next_native_operation_)return false;
    if(native_v8_aborted_!=other.native_v8_aborted_ || native_v8_ || other.native_v8_)return false; // 原生 V8 heap／callback 棧不能視為可複製的客體狀態。
    if(native_libraries_.configuration()!=other.native_libraries_.configuration())return false;
    if(authentication_backend_ || other.authentication_backend_)return false;
    if(com_running_table_ || other.com_running_table_)return false; // ROT 含跨程序機器資料，不能當成 deterministic 複本。
    if(com_rpc_ || other.com_rpc_)return false; // IPC 端點與 peer lifetime 不是客體狀態複本。
    if(dhcp_backend_ || other.dhcp_backend_)return false;
    if(printing_ || other.printing_)return false; // PDF 印表機含共用可變工作狀態。
    if(etw_class_callbacks_!=other.etw_class_callbacks_)return false;
    if(!etw_sessions_.empty() || !other.etw_sessions_.empty() || !etw_consumers_.empty() || !other.etw_consumers_.empty() ||
       !etw_processes_.empty() || !other.etw_processes_.empty() || !etw_callbacks_.empty() || !other.etw_callbacks_.empty() ||
       !etw_enable_waits_.empty() || !other.etw_enable_waits_.empty())return false; // ETW 原生檔案與執行中的 callback 不宣告分支等價。
    if(minidump_ || other.minidump_)return false; // 正在客體回呼中的 dump 不宣告可作分支等價比較。
    if(!same_ole_streams(other))return false;
    if(!same_storage(other))return false;
    if(demangle_ || other.demangle_)return false; // 客體配置回呼含共用可變狀態。
    if(device_configuration_ || other.device_configuration_)return false; // 原生裝置快照與 SetupAPI 集合不可假設為 deterministic 複本。
    if(bluetooth_ || other.bluetooth_)return false; // 原生請求、訂閱與 GATT 快取含外部生命週期。
    if(machine_shutdown_ || other.machine_shutdown_)return false; // 共用關機狀態不可作 deterministic 分支等價比較。
    if(std::any_of(consoles_.begin(),consoles_.end(),[](const auto& console){return console!=nullptr;}) ||
       std::any_of(other.consoles_.begin(),other.consoles_.end(),[](const auto& console){return console!=nullptr;}))return false;
    if(threadpool_ || other.threadpool_)return false; // 回呼池含共用可變狀態，不宣告可作 deterministic 複本比較。
    // 原生裝置與解碼 session 屬外部資源，不可當成可複製的 deterministic 狀態。
    if (!autoproxy_.empty() || !other.autoproxy_.empty())return false;
    if(!proxy_resolvers_.empty() || !other.proxy_resolvers_.empty() || !http_callbacks_.empty() || !other.http_callbacks_.empty())return false;
    if(proxy_result_allocations_!=other.proxy_result_allocations_)return false;
    if (acm_ || other.acm_ || media_foundation_ || other.media_foundation_ || wasapi_ || other.wasapi_ || !wave_outputs_.empty() || !other.wave_outputs_.empty() || !wave_inputs_.empty() || !other.wave_inputs_.empty() || audio_input_backend_ || other.audio_input_backend_ || !wave_callbacks_.empty() || !other.wave_callbacks_.empty())return false;
    if (!midi_inputs_.empty() || !other.midi_inputs_.empty())return false;
    if (!midi_outputs_.empty() || !other.midi_outputs_.empty())return false;
    if (!power_requests_.empty() || !other.power_requests_.empty())return false;
    // KTM 交易會在提交時改動宿主檔案，PDH 取樣讀取宿主即時資料；兩者都不宣告可作分支等價比較。
    if(transactions_open() || other.transactions_open() || pdh_ || other.pdh_)return false;
    if(privilege_acquisitions_!=other.privilege_acquisitions_ || wer_runtime_modules_!=other.wer_runtime_modules_ || gpu_scheduling_classes_!=other.gpu_scheduling_classes_)return false;
    if(dde_ || other.dde_)return false; // DDEML 實例含客體回呼與資料 handle 的共用可變狀態。
    if(winrt_class_factories_!=other.winrt_class_factories_ || winrt_factory_cookies_!=other.winrt_factory_cookies_ || apartment_shutdowns_!=other.apartment_shutdowns_)return false;
    if(url_security_managers_!=other.url_security_managers_ || url_security_table_!=other.url_security_table_ || dxva2_managers_!=other.dxva2_managers_ || dxva2_services_!=other.dxva2_services_ ||
       dxva2_manager_table_!=other.dxva2_manager_table_ || dxva2_decoder_table_!=other.dxva2_decoder_table_ || dxva2_processor_table_!=other.dxva2_processor_table_)return false;
    if(network_incidents_!=other.network_incidents_ || font_packages_!=other.font_packages_ || next_font_package_!=other.next_font_package_)return false;
    if(com_spy_operations_!=other.com_spy_operations_ || com_spies_!=other.com_spies_ || com_spy_bypass_!=other.com_spy_bypass_ ||
       next_com_spy_operation_!=other.next_com_spy_operation_ || next_com_spy_cookie_!=other.next_com_spy_cookie_ || touch_injection_feedback_!=other.touch_injection_feedback_)return false;
    if(power_notification_backend_ || other.power_notification_backend_ || !power_notifications_.empty() || !other.power_notifications_.empty())return false;
    if(data_protection_ || other.data_protection_)return false;
    if((runtime_.network.external_io && !shared_tokens_.empty()) || (other.runtime_.network.external_io && !other.shared_tokens_.empty()))return false;
    if(power_monitor_ || other.power_monitor_)return false; // 原生電源來源不能宣告 deterministic 分支等價。
    if (!address_notifications_.empty() || !other.address_notifications_.empty())return false;
    if (!canceled_http_io_.empty() || !other.canceled_http_io_.empty())return false;
    if (mappings_ || other.mappings_ || processes_ || other.processes_ || !opengl_.contexts.empty() || !other.opengl_.contexts.empty() || !rasters_.empty() || !other.rasters_.empty() || dmusic_ || other.dmusic_ || dsound_ || other.dsound_ || dinput_ || other.dinput_ || d3d9_ || other.d3d9_ || d3d11_ || other.d3d11_ || dwrite_ || other.dwrite_ || runtime_.host_clock || other.runtime_.host_clock) return false;
    if(opengl_.pixel_formats!=other.opengl_.pixel_formats || opengl_.swap_intervals!=other.opengl_.swap_intervals || opengl_.current!=other.opengl_.current || opengl_.next_context!=other.opengl_.next_context || opengl_.completed!=other.opengl_.completed)return false;
    if (unresolved_imports_ != other.unresolved_imports_) return false;
    if (threads_ != other.threads_) return false;
    if(registry_notifications_!=other.registry_notifications_ || next_registry_notification_!=other.next_registry_notification_)return false;
    if(timer_workers_exhausted_!=other.timer_workers_exhausted_ || timer_queues_!=other.timer_queues_ || queue_timers_!=other.queue_timers_ || next_timer_queue_handle_!=other.next_timer_queue_handle_)return false;
    if(registered_waits_!=other.registered_waits_ || wait_workers_!=other.wait_workers_ || next_registered_wait_!=other.next_registered_wait_)return false;
    if (gui_ != other.gui_) return false;
    if(font_enumerations_!=other.font_enumerations_)return false;
    if (exceptions_ != other.exceptions_) return false;
    if (runtime_ != other.runtime_) return false;
    if(live_stdio_!=other.live_stdio_ || parent_stdio_!=other.parent_stdio_ || live_input_eof_!=other.live_input_eof_)return false;
    if(serial_ports_!=other.serial_ports_ || native_serial_paths_!=other.native_serial_paths_ || serial_ports_scanned_!=other.serial_ports_scanned_)return false;
    if (stdout_ != other.stdout_ || stderr_ != other.stderr_ || last_error_ != other.last_error_ ||
        next_handle_ != other.next_handle_ || next_allocation_ != other.next_allocation_ || allocations_ != other.allocations_ || free_heap_blocks_ != other.free_heap_blocks_ || free_heap_sizes_ != other.free_heap_sizes_ ||
        heap_quick_blocks_ != other.heap_quick_blocks_ || heap_quick_bins_ != other.heap_quick_bins_ || heap_quick_pages_ != other.heap_quick_pages_ ||
        standard_open_ != other.standard_open_ || initialized_ != other.initialized_ || files_.size() != other.files_.size()) return false;
    for (const auto& [handle, file] : files_) {
        const auto found = other.files_.find(handle);
        if(file.native || file.pipe || file.serial || file.hid || (found!=other.files_.end() && (found->second.native || found->second.pipe || found->second.serial || found->second.hid)))return false;
        if (found == other.files_.end() || file.key != found->second.key || file.object != found->second.object ||
            file.path != found->second.path || file.directory_cursor != found->second.directory_cursor || file.directory_loaded != found->second.directory_loaded || file.entries != found->second.entries || file.completion_port != found->second.completion_port || file.completion_key != found->second.completion_key || file.completion_modes != found->second.completion_modes || file.io_signaled != found->second.io_signaled || file.sharing != found->second.sharing || file.metadata != found->second.metadata || file.access != found->second.access || file.mode != found->second.mode || *file.bytes != *found->second.bytes) return false;
    }
    return true;
}
unsigned WindowsServices::runtime_compute_boundary(GuestAddress address) const noexcept {
    if(address.value>=stub_base_.value && address.value-stub_base_.value<services.size()*16 &&
       (address.value-stub_base_.value)%16==0){
        // 固定 kernel32 表與延伸 descriptor 使用同一分類入口；熱呼叫不逐次比對名稱。
        static constexpr auto classes=[] {
            std::array<std::uint8_t,services.size()> result{};
            for(std::size_t index=0;index<services.size();++index){
                const auto name=services[index].name;
                if(name=="GetProcessHeap" || name=="HeapAlloc" || name=="HeapReAlloc" ||
                   name=="HeapFree" || name=="HeapSize" || name=="HeapValidate")result[index]=8;
            }
            return result;
        }();
        return classes[(address.value-stub_base_.value)/16];
    }
    const auto runtime_begin=stub_base_.value+services.size()*16;
    const auto runtime_table=runtime_service_descriptors();
    if(address.value<runtime_begin || address.value-runtime_begin>=runtime_table.size()*16 || (address.value-runtime_begin)%16)return 0;
    // descriptor 表在程序期間固定；首次使用時分類一次，之後每次呼叫只做索引。
    static const auto classes=[runtime_table] {
        constexpr std::array string_operations{"strlen","strcmp","strncmp","strcpy","strncpy",
            "strchr","strrchr","strstr","strcat","strncat","wcscat","wcsncat","_errno"};
        constexpr std::array thread_operations{"TlsAlloc","TlsFree","TlsGetValue","TlsGetValue2","TlsSetValue",
            // 臨界區與條件變數喚醒只改程序內鎖表與客體結構；競爭時與 SRW 一樣經 schedule 等待。
            // SleepConditionVariable* 有逾時，期限需要入口更新的時鐘，仍走完整入口。
            "InitializeSRWLock","AcquireSRWLockExclusive","AcquireSRWLockShared",
            "TryAcquireSRWLockExclusive","TryAcquireSRWLockShared","ReleaseSRWLockExclusive","ReleaseSRWLockShared"};
        constexpr std::array identity_operations{"GetCurrentThreadId","GetCurrentProcessId","GetCurrentThread","GetCurrentProcess"};
        constexpr std::array allocation_operations{"malloc","calloc","realloc","_recalloc","free","_msize",
            "_aligned_malloc","_aligned_realloc","_aligned_free","_aligned_offset_malloc",
            "_aligned_offset_realloc","_aligned_recalloc","_aligned_offset_recalloc","_aligned_msize"};
        static const bool cs_boundary=[] {const auto* setting=std::getenv("X64ARM_CS_BOUNDARY");return !setting || std::string_view(setting)!="0";}();
        constexpr std::array cs_operations{"InitializeCriticalSection","InitializeCriticalSectionAndSpinCount","InitializeCriticalSectionEx",
            "EnterCriticalSection","TryEnterCriticalSection","LeaveCriticalSection","WakeConditionVariable","WakeAllConditionVariable"};
        static const bool time_boundary=[] {const auto* setting=std::getenv("X64ARM_TIME_BOUNDARY");return !setting || std::string_view(setting)!="0";}();
        constexpr std::array time_operations{"QueryPerformanceCounter","QueryPerformanceFrequency","GetSystemTimeAsFileTime","GetTickCount"};
        static const bool resident_compute=[] {const auto* setting=std::getenv("X64ARM_RESIDENT_COMPUTE_SERVICES");return !setting || std::string_view(setting)!="0";}();
        constexpr std::array once_operations{"InitOnceInitialize","InitOnceBeginInitialize","InitOnceComplete","InitOnceExecuteOnce"};
        constexpr std::array extended_time_operations{"GetTickCount64","GetSystemTimePreciseAsFileTime",
            "QueryUnbiasedInterruptTime","QueryUnbiasedInterruptTimePrecise"};
        static const bool fls_boundary=[] {const auto* setting=std::getenv("X64ARM_FLS_BOUNDARY");return !setting || std::string_view(setting)!="0";}();
        static const bool crt_compute=[] {const auto* setting=std::getenv("X64ARM_CRT_COMPUTE_BOUNDARY");return !setting || std::string_view(setting)!="0";}();
        constexpr std::array compute_operations{"sprintf","vsprintf","sprintf_s","vsprintf_s","swprintf_s","vswprintf_s",
            "snprintf","vsnprintf","_snprintf","_vsnprintf","_snwprintf","_vsnwprintf","_snprintf_s","_vsnprintf_s","_snwprintf_s","_vsnwprintf_s",
            "_scprintf","_vscprintf","_scwprintf","_vscwprintf",
            "__stdio_common_vsprintf","__stdio_common_vsprintf_s","__stdio_common_vsnprintf_s","__stdio_common_vsprintf_p",
            "__stdio_common_vswprintf","__stdio_common_vswprintf_s","__stdio_common_vsnwprintf_s","__stdio_common_vswprintf_p",
            "strtod","strtof","strtol","strtoul","strtoll","strtoull","_strtoi64","_strtoui64",
            "wcstod","wcstof","wcstol","wcstoul","wcstoll","wcstoull","_wcstoi64","_wcstoui64"};
        constexpr std::array conversion_operations{"strtod","strtof","strtol","strtoul","strtoll","strtoull","_strtoi64","_strtoui64",
            "wcstod","wcstof","wcstol","wcstoul","wcstoll","wcstoull","_wcstoi64","_wcstoui64"};
        std::vector<std::uint8_t> result(runtime_table.size());
        for(std::size_t index=0;index<runtime_table.size();++index){
            const auto& descriptor=runtime_table[index];
            if((descriptor.family=="crt" && (descriptor.symbol=="memcpy" || descriptor.symbol=="memmove" ||
                                             descriptor.symbol=="memset" || descriptor.symbol=="memcmp")) ||
               ((descriptor.family=="kernel32.dll" || descriptor.family=="ntdll.dll") && descriptor.symbol=="RtlMoveMemory"))result[index]=1;
            else if(descriptor.family=="crt" &&
                    std::find(string_operations.begin(),string_operations.end(),descriptor.symbol)!=string_operations.end())result[index]=2;
            else if(scalar_math_operation(descriptor))result[index]=3;
            else if(descriptor.family=="kernel32.dll" &&
                    std::find(thread_operations.begin(),thread_operations.end(),descriptor.symbol)!=thread_operations.end())result[index]=4;
            // 臨界區與條件變數喚醒只改程序內鎖表與客體結構；競爭時與 SRW 一樣經 schedule 等待。
            // SleepConditionVariable* 有逾時，期限需要入口更新的時鐘，仍走完整入口。
            else if(cs_boundary && descriptor.family=="kernel32.dll" &&
                    std::find(cs_operations.begin(),cs_operations.end(),descriptor.symbol)!=cs_operations.end())result[index]=4;
            // 計時查詢只需要入口的時鐘同步（machine clock 準備與 retired），不需 GUI 資源、HTTP 輪詢等其餘前置處理。
            else if(time_boundary && descriptor.family=="kernel32.dll" &&
                    (std::find(time_operations.begin(),time_operations.end(),descriptor.symbol)!=time_operations.end() ||
                     (resident_compute && std::find(extended_time_operations.begin(),extended_time_operations.end(),descriptor.symbol)!=extended_time_operations.end())))result[index]=9;
            else if(time_boundary && resident_compute && descriptor.family=="kernel32.dll" &&
                    (descriptor.symbol=="GetSystemTime" || descriptor.symbol=="GetLocalTime"))result[index]=11;
            // once 的競爭與回呼仍由原 provider 更新 context／epoch；完整狀態邊界
            // 可直接續行成功查詢，也可將等待、回呼或映射變更交回既有協調流程。
            else if(resident_compute && descriptor.family=="kernel32.dll" &&
                    std::find(once_operations.begin(),once_operations.end(),descriptor.symbol)!=once_operations.end())result[index]=4;
            // FLS 取值／設值只讀寫目前執行緒的 FLS 陣列，與 TLS 同屬執行緒邊界；FlsAlloc／FlsFree 涉及回呼仍走完整入口。
            else if(fls_boundary && descriptor.family=="kernel32.dll" &&
                    (descriptor.symbol=="FlsGetValue" || descriptor.symbol=="FlsGetValue2" || descriptor.symbol=="FlsSetValue"))result[index]=4;
            else if(descriptor.family=="kernel32.dll" &&
                    std::find(identity_operations.begin(),identity_operations.end(),descriptor.symbol)!=identity_operations.end())result[index]=5;
            else if(descriptor.family=="crt" &&
                    std::find(allocation_operations.begin(),allocation_operations.end(),descriptor.symbol)!=allocation_operations.end())result[index]=6;
            else if(crt_character_operation(descriptor))result[index]=7;
            // 寫入緩衝區的格式化與字串轉數值：只讀寫客體記憶體、errno 與 invalid-parameter 回呼，
            // 不碰 LastError、GUI、I/O 或排程；轉換家族另列 10，沿用同一 provider。
            // 格式化的完整 FP 常駐邊界已有實測退步，先保留既有入口並最佳化服務本體。
            else if(crt_compute && descriptor.family=="crt" &&
                    std::find(compute_operations.begin(),compute_operations.end(),descriptor.symbol)!=compute_operations.end())
                result[index]=std::find(conversion_operations.begin(),conversion_operations.end(),descriptor.symbol)!=conversion_operations.end()?10:8;
        }
        return result;
    }();
    const auto boundary=classes[(address.value-runtime_begin)/16];
    return x86_ && boundary==10?8:boundary;
}
bool WindowsServices::x86_heap_compute_boundary(GuestAddress address) const noexcept {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_X86_HEAP_BOUNDARY");
        return !setting || std::string_view(setting)!="0";}();
    // 只接入已核對 WindowsCall 的 Win32 heap 家族。HeapLock 等待與 SEH 清理保留完整入口；
    // 不讀取客體參數來猜測是否阻塞，也不讓客體回呼跨過未完成的例外邊界。
    return enabled && x86_ && initialized_ && trace_services_.empty() && !startup_profile::enabled() &&
        !runtime_.forced_exit_code() && !threads_.yield && runtime_.heap_locks.empty() &&
        exceptions_.stack.empty() && runtime_compute_boundary(address)==8;
}
#ifdef X64ARM_HOST_SERVICES_RESIDENT_SERVICE
bool WindowsServices::resident_service(GuestAddress address) const noexcept {
    // 與 invoke 使用相同動態資格；x86 先納入無等待／回呼的 heap 計算邊界。
    if(x86_)return x86_heap_compute_boundary(address);
    if(!trace_services_.empty() || startup_profile::enabled() || runtime_.forced_exit_code())return false;
    // 只推進排序／搜尋狀態並準備下一個比較器，沒有在宿主內執行巢狀客體碼。
    // 由共用葉節點邊界物化完整狀態；映射／排程變動與例外仍交既有核心處理。
    if(address==callback_return_ && reusable_sort_callback()){
        static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_CRT_RESIDENT_SORT");return !setting || std::string_view(setting)!="0";}();
        return enabled;
    }
    const auto boundary=runtime_compute_boundary(address);
    // 程序期間固定的家族政策與開關一次物化；共用一個初始化守衛。
    // 一般呼叫直接索引，動態資格仍在上方逐次核對。
    static const auto policies=[] {
        const auto enabled=[](const char* name){const auto* value=std::getenv(name);return !value || std::string_view(value)!="0";};
        struct Policies {std::array<bool,12> classes{};bool indexed{};};
        Policies policy;
        policy.indexed=enabled("X64ARM_RESIDENT_COMPUTE_SERVICES");
        if(!policy.indexed)return policy;
        auto& result=policy.classes;
        result[1]=enabled("X64ARM_CRT_MEMORY");
        result[2]=enabled("X64ARM_CRT_STRINGS");
#ifdef X64ARM_HOST_SERVICES_RESIDENT_HOST_FP
        result[3]=scalar_math_requested();
#endif
        result[4]=enabled("X64ARM_THREAD_BOUNDARY");
        result[5]=enabled("X64ARM_IDENTITY_BOUNDARY");
        result[6]=enabled("X64ARM_ALLOCATION_BOUNDARY");
        result[7]=crt_character_requested();
        // 0 與固定 Heap／格式化類別 8 保持關閉；9～11 只可能來自
        // 已通過位址、對齊及 descriptor 範圍檢查的延伸表。
        result[9]=result[10]=result[11]=true;
        return policy;
    }();
    if(policies.indexed)return boundary<policies.classes.size() && policies.classes[boundary];
    if(boundary==1){
        static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_CRT_MEMORY");return !setting || std::string_view(setting)!="0";}();
        return enabled;
    }
    if(boundary==2){
        static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_CRT_STRINGS");return !setting || std::string_view(setting)!="0";}();
        return enabled;
    }
    if(boundary==4){
        static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_THREAD_BOUNDARY");return !setting || std::string_view(setting)!="0";}();
        return enabled;
    }
    if(boundary==5){
        static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_IDENTITY_BOUNDARY");return !setting || std::string_view(setting)!="0";}();
        return enabled;
    }
    if(boundary==6){
        static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_ALLOCATION_BOUNDARY");return !setting || std::string_view(setting)!="0";}();
        return enabled;
    }
    if(boundary==7)return crt_character_requested();
#ifdef X64ARM_HOST_SERVICES_RESIDENT_HOST_FP
    if(boundary==3)return scalar_math_requested();
#endif
    return false;
}
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_HOST_FP
bool WindowsServices::resident_service_host_fp(GuestAddress address) const noexcept {
    const auto boundary=runtime_compute_boundary(address);
    // 非 C locale 可進入 ICU；沿用 SDK 的宿主 FP 邊界，不假設外部函式完全不碰浮點環境。
    // 數值轉換自行套用客體捨入模式，日期轉換會進宿主 libc，沿用完整 FP 邊界。
    // 純整數的 counter／tick／FILETIME 查詢不切換 FPCR，也不重建客體 FP 核准狀態。
    return boundary==3 || boundary==7 || (!x86_ && (boundary==10 || boundary==11));
}
#endif
#ifdef X64ARM_HOST_SERVICES_MANAGED_FP
bool WindowsServices::resident_service_manages_fp(GuestAddress address) const noexcept {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_AOT_MANAGED_FP");return !value || std::string_view(value)!="0";}();
#ifdef X64ARM_HOST_SERVICES_RETAINED_FP
    static const bool retained=[] {const auto* value=std::getenv("X64ARM_AOT_SERVICE_FP_PRESERVE");return !value || std::string_view(value)!="0";}();
#endif
    const auto boundary=runtime_compute_boundary(address);
    return enabled && (boundary==3
#ifdef X64ARM_HOST_SERVICES_RETAINED_FP
                       || (retained && boundary==7)
#endif
                       );
}
std::optional<std::uint32_t> WindowsServices::invoke_managed_service(GuestState& state,GuestMemory& memory,
                                                                  const ResidentHostEnvironment& environment) {
    return invoke_boundary(state,memory,&environment);
}
#endif
#ifdef X64ARM_HOST_SERVICES_SCALAR_NATIVE
bool WindowsServices::resident_scalar_service(GuestAddress address) const noexcept {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_AOT_SCALAR_NATIVE");return !value || std::string_view(value)!="0";}();
    return enabled && !x86_ && scalar_math_requested() && runtime_compute_boundary(address)==3;
}
bool WindowsServices::invoke_resident_scalar(ResidentScalarCall& call,GuestMemory& memory) noexcept {
    const GuestAddress address{call.entry};
    entry_runtime_profile::Scope duration(entry_runtime_profile::Kind::Service,address.value,
        entry_runtime_profile::enabled()?name(address):std::string_view{});
    if(!resident_scalar_service(address) || !resident_service(address))return false;
    const auto index=(address.value-stub_base_.value-services.size()*16)/16;
    const auto& descriptor=runtime_service_descriptors()[index];
    const auto* operation=scalar_math_operation(descriptor);
    if(!operation)return false;
    return invoke_native_scalar_math(call,memory,*operation,runtime_.errno_cell);
}
#endif
#ifdef X64ARM_HOST_SERVICES_SCALAR_RECIPE
unsigned WindowsServices::resident_scalar_recipe(GuestAddress address) const noexcept {
    // 與純量值服務相同的靜態資格；配方只負責一般值，特殊值仍由核心交回 invoke_resident_scalar。
    if(!resident_scalar_service(address) || !resident_service(address))return 0;
    const auto index=(address.value-stub_base_.value-services.size()*16)/16;
    return scalar_math_recipe(runtime_service_descriptors()[index]);
}
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_CONTEXT_SCALAR
bool WindowsServices::resident_context_scalar_service(GuestAddress address) const noexcept {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_AOT_WORKER_SCALAR");return !value || std::string_view(value)!="0";}();
    return enabled && resident_scalar_service(address) && resident_service(address);
}
bool WindowsServices::invoke_resident_context_scalar(ResidentScalarCall& call,GuestMemory& memory,
                                                     const ResidentScalarErrors& errors) noexcept {
    const GuestAddress address{call.entry};
    entry_runtime_profile::Scope duration(entry_runtime_profile::Kind::Service,address.value,
        entry_runtime_profile::enabled()?name(address):std::string_view{});
    if(!resident_context_scalar_service(address))return false;
    const auto index=(address.value-stub_base_.value-services.size()*16)/16;
    const auto* operation=scalar_math_operation(runtime_service_descriptors()[index]);
    return operation && invoke_native_context_scalar_math(call,memory,*operation,errors);
}
#endif
#ifdef X64ARM_HOST_SERVICES_SCALAR_ERRORS
namespace {
ResidentScalarErrors scalar_error_cell(GuestMemory& memory,std::uint64_t address,bool x86) noexcept {
    static const bool enabled=[] {
        const auto* range=std::getenv("X64ARM_SCALAR_RANGE_NATIVE");
        const auto* errors=std::getenv("X64ARM_SCALAR_ERRNO_RESIDENT");
        return (!range || std::string_view(range)!="0") && (!errors || std::string_view(errors)!="0");
    }();
    if(!enabled || x86 || !address || !scalar_math_requested())return {};
    // native_pointer 拒絕 COW、程式碼、外部及宿主更新頁；每次發布或客體
    // 重綁都重新借用，不保存跨映射世代或執行緒身分的指標。
    auto* cell=static_cast<std::uint32_t*>(memory.native_pointer(GuestAddress{address},4,permission(Access::Write)));
    return {cell,address,33,34};
}
}
ResidentScalarErrors WindowsServices::resident_scalar_errors(GuestMemory& memory) noexcept {
    return scalar_error_cell(memory,runtime_.errno_cell,x86_);
}
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_CONTEXT_ERRORS
ResidentScalarErrors WindowsServices::resident_context_errors(const GuestState& state,GuestMemory& memory) noexcept {
    const auto thread=threads_.items.find(resident_context_key(state));
    if(thread==threads_.items.end() || thread->second.teb!=state.gs_base)return {};
    return scalar_error_cell(memory,thread->second.errno_cell,x86_);
}
#endif
#ifdef X64ARM_HOST_SERVICES_COORDINATED_SERVICE
bool WindowsServices::resident_coordinated_service(GuestState& state,GuestMemory& memory) {
    if(x86_ || !runtime_.host_clock || !trace_services_.empty() || startup_profile::enabled() ||
       runtime_.forced_exit_code() || debugger_stopped())return false;
    // CFG 分派是控制流程邊界，不是 OS 呼叫：只核對目標、不改 LastError／權杖／GUI／IO，也不切換執行緒，
    // 可在其他 worker 繼續執行時由協調者完成（renderer 每輪數萬次，是 worker 完整服務出口的大宗）。
    if(state.rip.value==callback_return_.value+0x60 || state.rip.value==callback_return_.value+0x70){
        const auto key=resident_context_key(state);const auto thread=threads_.items.find(key);
        if(thread==threads_.items.end() || thread->second.teb!=state.gs_base)return false;
        const auto previous=threads_.current;
        struct RestoreCurrent {WindowsServices& self;std::uint64_t previous;~RestoreCurrent(){self.threads_.current=previous;}} restore_current{*this,previous};
        threads_.current=key;resident_service_requests_.erase(key);
        refresh_guard_table(memory);
        const auto result=invoke_service(state,memory);
        if(result)throw std::logic_error("CFG 分派服務意外終止客體");
        refresh_guard_table(memory);
        return true;
    }
    const auto begin=stub_base_.value+services.size()*16;
    const auto table=runtime_service_descriptors();
    if(state.rip.value<begin || (state.rip.value-begin)%16 || (state.rip.value-begin)/16>=table.size())return false;
    const auto& descriptor=table[(state.rip.value-begin)/16];
    // 虛擬記憶體家族沿用同一個 provider；不重新實作配置、權限、錯誤或撤銷語義。
    // 跨程序操作可能等待另一個程序，保留完整服務邊界。
    const auto symbol=descriptor.symbol;
    const auto mapping=detail::resident_mapping_service(symbol);
    const bool extended=mapping==detail::ResidentMappingService::Process;
    const bool allocation2=mapping==detail::ResidentMappingService::Allocation2;
    if(descriptor.family!="kernel32.dll" || mapping==detail::ResidentMappingService::None)return false;
    if((extended || allocation2) && !(allocation2 && !state.reg(Gpr::Rcx)) &&
       !current_process_handle(state.reg(Gpr::Rcx)))return false;
    const auto key=resident_context_key(state);const auto thread=threads_.items.find(key);
    if(thread==threads_.items.end() || thread->second.teb!=state.gs_base)return false;
    const auto previous=threads_.current;const auto saved_error=last_error_;
    const auto entry_error=static_cast<std::uint32_t>(memory.load(GuestAddress{state.gs_base+0x68},32));
    struct Restore {
        WindowsServices& self;std::uint64_t previous,current;std::uint32_t error;
        ~Restore(){self.threads_.current=previous;if(current!=previous)self.last_error_=error;}
    } restore{*this,previous,key,saved_error};
    threads_.current=key;last_error_=entry_error;
    entry_runtime_profile::Scope duration(entry_runtime_profile::Kind::Service,state.rip.value,symbol);
    refresh_guard_table(memory);
    const auto result=invoke_process_memory(state,memory,descriptor);
    if(result)throw std::logic_error("虛擬記憶體服務意外終止客體");
    if(last_error_!=entry_error)memory.store(GuestAddress{state.gs_base+0x68},32,last_error_);
    refresh_guard_table(memory);
    ++state.tsc;state.rep_compare={};
    return true;
}
#endif
#include "registry_service_dependencies.inc"
bool WindowsServices::defer_registry_io_call(GuestState& state,GuestMemory& memory){
    if(!registry_io_busy_ || threads_.current==registry_io_owner_ || runtime_.forced_exit_code())return false;
    // 續行尚未 pop callback frame，普通入口也尚未讀寫 API 輸出；等待後可安全重新進入。
    bool dependent=true;
    const auto table=runtime_service_descriptors();
    if(state.rip.value>=stub_base_.value && (state.rip.value-stub_base_.value)%16==0){
        const auto index=(state.rip.value-stub_base_.value)/16;
        if(index<services.size())dependent=registry_dependent_service({"kernel32.dll",services[index].name,services[index].arguments,services[index].wide_arguments});
        else if(index-services.size()<table.size())dependent=registry_dependent_service(table[index-services.size()]);
    }
    if(!dependent)return false;
    auto& thread=threads_.items.at(threads_.current);
    thread.registry_deferred_wait.emplace(std::move(thread.wait));
    thread.status=ThreadStatus::Waiting;thread.wait={};
    thread.wait.kind=ThreadWaitKind::RegistryIo;
    schedule(state,memory);return true;
}
std::optional<std::uint32_t> WindowsServices::invoke(GuestState& state, GuestMemory& memory) {
    return invoke_boundary(state,memory);
}
std::optional<std::uint32_t> WindowsServices::invoke_boundary(GuestState& state,GuestMemory& memory,
                                                            const ResidentHostEnvironment* environment) {
    // 協調者已接手此執行緒的完整 provider；成功、等待與例外均由同一流程完成。
    // worker 的窄服務不經這裡，也不能提早清掉尚未處理的協調請求。
    resident_service_requests_.erase(threads_.current);
#ifdef X64ARM_HOST_SERVICES_MANAGED_FP
    if(registry_io_busy_ && environment){environment->restore();environment=nullptr;}
#endif
    if(defer_registry_io_call(state,memory))return std::nullopt;
    entry_runtime_profile::Scope duration(entry_runtime_profile::Kind::Service,state.rip.value,
        entry_runtime_profile::enabled()?name(state.rip):std::string_view{});
    static const bool trace_slow_calls=std::getenv("X64ARM_TRACE_SLOW_CALLS")!=nullptr;
    struct CountScope {
        WindowsServices& self;std::uint64_t rip,thread;bool trace;std::chrono::steady_clock::time_point start;
        const char* phase_name{"boundary"};std::chrono::steady_clock::time_point phase_start{start};
        void phase(const char* next){
            if(!trace)return;
            const auto now=std::chrono::steady_clock::now();
            const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(now-phase_start).count();
            if(elapsed>=5000){const auto symbol=self.name(GuestAddress{rip});
                std::fprintf(stderr,"slow_service_phase pid=%llu thread=%llu phase=%s duration_us=%lld end_unix_us=%llu name=%.*s\n",
                    static_cast<unsigned long long>(self.process_id()),static_cast<unsigned long long>(thread),phase_name,
                    static_cast<long long>(elapsed),static_cast<unsigned long long>(startup_profile::unix_us()),static_cast<int>(symbol.size()),symbol.data());}
            phase_name=next;phase_start=now;
        }
        ~CountScope(){if(!self.service_counts_enabled_ && !trace)return;
            phase("finished");
            const auto now=std::chrono::steady_clock::now();
            const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(now-start).count();
            if(trace && elapsed>=5000){const auto symbol=self.name(GuestAddress{rip});
                std::fprintf(stderr,"slow_service pid=%llu thread=%llu current=%llu rip=0x%llx duration_us=%lld end_unix_us=%llu name=%.*s\n",
                    static_cast<unsigned long long>(self.process_id()),static_cast<unsigned long long>(thread),static_cast<unsigned long long>(self.threads_.current),
                    static_cast<unsigned long long>(rip),static_cast<long long>(elapsed),static_cast<unsigned long long>(startup_profile::unix_us()),static_cast<int>(symbol.size()),symbol.data());}
            if(!self.service_counts_enabled_)return;auto& c=self.service_counts_[rip];++c.calls;
            c.ns+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now-start).count());
            if(!self.service_counts_directory_.empty() && (++self.service_count_events_&1023)==0 && now-self.service_count_dumped_>std::chrono::seconds(5))self.dump_service_counts();}
    } count_scope{*this,state.rip.value,threads_.current,trace_slow_calls,(service_counts_enabled_ || trace_slow_calls)?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{}};
#ifdef X64ARM_HOST_SERVICES_MANAGED_FP
    // 非數學服務仍沿用宿主環境；同一入口也支援動態資格撤銷後的完整流程。
    if(environment && runtime_compute_boundary(state.rip)!=3
#ifdef X64ARM_HOST_SERVICES_RETAINED_FP
       && runtime_compute_boundary(state.rip)!=7
#endif
       ){environment->restore();environment=nullptr;}
#endif
    // 服務可能撤銷可執行映射或移除執行權限；客體恢復執行前先讓 CFG 快速路徑的已驗證目標失效。
    // 進入時也先核對，讓本次服務（例如 CFG 完整檢查）填入的目標不被先前的變更一併清掉。
    refresh_guard_table(memory);
    struct Refresh { WindowsServices& self;GuestMemory& memory;~Refresh(){self.refresh_guard_table(memory);} } refresh{*this,memory};
    // CFG 是控制流程邊界，不是 OS 呼叫：沿用完整目標檢查，但不更新
    // LastError、權杖、GUI 資源或 IO。時間與喚醒仍由指令退休流程處理。
    // 啟用 API trace 或待處理 job 結束時，保留原有完整入口行為。
    poll_machine_shutdown();
    if(trace_services_.empty() && !runtime_.forced_exit_code() &&
       (state.rip.value==callback_return_.value+0x60 || state.rip.value==callback_return_.value+0x70)){
        startup_profile::Scope measurement("api",startup_profile::enabled()?name(state.rip):std::string_view{});
        return invoke_service(state,memory);
    }
    // 純量 CRT 與記憶體操作是計算邊界；時間、排程與 IO 輪詢仍由 after_instructions／wake_threads 處理。
    // 此類操作不改 LastError、權杖或 GUI 資源，無需每次執行完整 OS 入口前後處理。
    const auto runtime_begin=stub_base_.value+services.size()*16;
    const auto runtime_table=runtime_service_descriptors();
    const auto* runtime_descriptor=state.rip.value>=runtime_begin && state.rip.value-runtime_begin<runtime_table.size()*16 &&
        (state.rip.value-runtime_begin)%16==0?&runtime_table[(state.rip.value-runtime_begin)/16]:nullptr;
    const auto boundary=runtime_compute_boundary(state.rip);
    const bool memory_operation=boundary==1,string_operation=boundary==2;
    if(boundary==8 && x86_heap_compute_boundary(state.rip)){
        // 仍由同一 provider 執行 stdcall 參數、返回、配置、釋放及錯誤處理。
        // HeapLock／SEH 不在此資格內；服務不切換執行緒，TSC 只由核心退休一次。
        // 新增／撤銷映射及可執行頁寫入由 resident 服務的世代／MMU 邊界立即退出處理。
        // 時間與 I/O 輪詢沿用有界 after_instructions／wake_threads。
        if(runtime_.image.base)last_error_=static_cast<std::uint32_t>(memory.load(GuestAddress{state.fs_base+0x34},32));
        const auto result=invoke_service(state,memory);
        if(runtime_.image.base)memory.store(GuestAddress{state.fs_base+0x34},32,last_error_);
        return result;
    }
    if(!x86_ && trace_services_.empty() && !startup_profile::enabled() && !runtime_.forced_exit_code() &&
       runtime_descriptor){
        const auto& descriptor=*runtime_descriptor;
        // 類別 3 已由 runtime_compute_boundary 判定；不再每次呼叫以 descriptor 位址查表。
        if(boundary==3 && scalar_math_requested())if(const auto* operation=scalar_math_operation(descriptor)){
            invoke_scalar_math(state,memory,descriptor,*operation,runtime_.errno_cell,false,environment);return std::nullopt;
        }
        static const bool memory_boundary=[] {const auto* setting=std::getenv("X64ARM_CRT_MEMORY");return !setting || std::string_view(setting)!="0";}();
        // 沿用同一 provider 的範圍檢查、COW、別名、故障與可執行頁失效流程。
        static const bool string_boundary=[] {const auto* setting=std::getenv("X64ARM_CRT_STRINGS");return !setting || std::string_view(setting)!="0";}();
        // 字串與 errno 位址查詢沒有 OS／I/O 副作用；仍使用原有 ABI、範圍檢查及 COW。
        // 寫入 TEB 的合法客體操作須同步內部 LastError，不能由完整入口覆寫回舊值。
        if((memory_boundary && memory_operation) || (string_boundary && string_operation) ||
           (boundary==7 && crt_character_requested())){
            const auto result=boundary==7?invoke_crt_character(state,memory,descriptor,environment):
                                          invoke_runtime(state,memory,descriptor);
            if(runtime_.image.base)last_error_=static_cast<std::uint32_t>(memory.load(GuestAddress{state.gs_base+0x68},32));
            return result;
        }
        static const bool thread_boundary=[] {const auto* setting=std::getenv("X64ARM_THREAD_BOUNDARY");return !setting || std::string_view(setting)!="0";}();
        static const bool identity_boundary=[] {const auto* setting=std::getenv("X64ARM_IDENTITY_BOUNDARY");return !setting || std::string_view(setting)!="0";}();
        static const bool allocation_boundary=[] {const auto* setting=std::getenv("X64ARM_ALLOCATION_BOUNDARY");return !setting || std::string_view(setting)!="0";}();
        if((thread_boundary && boundary==4) || (identity_boundary && boundary==5) || (allocation_boundary && boundary==6) || boundary==8 || boundary==9 || boundary==10 || boundary==11){
            if(boundary==9 || boundary==11){prepare_machine_clock();if(runtime_.host_clock)retired(memory,0);}
            // 與完整入口相同：另一執行緒持有 HeapLock 時，共用 process heap 的 CRT 配置先等待。
            if(boundary==6 && await_heap_service(state,memory,descriptor))return std::nullopt;
            // TLS／SRW、身分查詢與 CRT 配置只操作程序內的狀態。
            // 身分仍由既有 provider 依目前排程 context 取得，不快取前一執行緒的結果。
            // 配置沿用同一 provider 的 errno、new-handler、invalid-parameter callback
            // 與精確故障；native MMU／程式碼映射變更會由核心常駐邊界交回宿主處理。
            // 共用 provider 保留 TEB LastError、鎖所有權、等待及精確故障；排程變更會更新
            // concurrency epoch，讓核心的常駐服務邊界立即交回宿主重新驗證。
            if(runtime_.image.base)last_error_=static_cast<std::uint32_t>(memory.load(GuestAddress{state.gs_base+0x68},32));
            const auto entry_error=last_error_;
            const auto tsc=state.tsc,nested=nested_instruction_count();
            struct PreserveThreadClock {
                GuestState& state;HostServices& services;std::uint64_t tsc,nested;
                ~PreserveThreadClock(){state.tsc=tsc+services.nested_instruction_count()-nested;}
            } clock{state,*this,tsc,nested};
            const auto result=invoke_runtime(state,memory,descriptor);
            // TEB 值仍等於進入時讀到的值才略過寫回（寫回含 COW 檢查，占此路徑宿主時間約兩成）。
            if(runtime_.image.base && last_error_!=entry_error)memory.store(GuestAddress{state.gs_base+0x68},32,last_error_);
            return result;
        }
    }
#ifdef X64ARM_HOST_SERVICES_MANAGED_FP
    // 追蹤、關機或其他資格在進入後改變時，完整 provider 必須看到原宿主環境。
    if(environment)environment->restore();
#endif
    // CRT 排序／搜尋的比較器返回只推進計算狀態，不是新的 OS 呼叫。
    // 仍同步 TEB LastError 與退休時間；I/O、排程及關機由原有退休流程處理。
    // 有 GUI／Shell 邊界、Win32 SEH、追蹤或強制結束時保留完整入口。
    if(!x86_ && trace_services_.empty() && !startup_profile::enabled() && !runtime_.forced_exit_code() &&
       state.rip==callback_return_ && reusable_sort_callback()) {
            if(runtime_.image.base)last_error_=static_cast<std::uint32_t>(memory.load(GuestAddress{state.gs_base+0x68},32));
            const auto tsc=state.tsc, nested=nested_instruction_count();
            struct PreserveCallbackClock {
                GuestState& state;HostServices& services;std::uint64_t tsc,nested;
                ~PreserveCallbackClock(){state.tsc=tsc+services.nested_instruction_count()-nested;}
            } clock{state,*this,tsc,nested};
            const auto result=resume_callback(state,memory);
            if(runtime_.image.base)memory.store(GuestAddress{state.gs_base+0x68},32,last_error_);
            return result;
    }
    startup_profile::Scope measurement("api",startup_profile::enabled()?name(state.rip):std::string_view{});
    count_scope.phase("preamble");
    process_io::Scope io_scope(runtime_.io_counters,io_publisher_.get());
    struct GuiResources {
        GuiState& gui;
        explicit GuiResources(GuiState& value):gui(value) {
            startup_profile::Scope timer("api_preamble","gui_resources");gui.observe_resources();
        }
        ~GuiResources() {
            startup_profile::Scope timer("api_preamble","gui_resources");gui.observe_resources();
        }
    };
    std::optional<GuiResources> resource_scope;
    {
        startup_profile::Scope preamble("api_preamble","entry");
        poll_http_io();
        if(const auto code=runtime_.forced_exit_code()){record_job_exit(*code);return *code;}
        refresh_file_security_context();
        prune_x86_exception_callbacks(state,memory);
        resource_scope.emplace(gui_);
        notify_concurrency_changed();
        prepare_machine_clock();
        prepare_machine_shutdown();
        poll_machine_shutdown();
        if(const auto code=runtime_.forced_exit_code()){record_job_exit(*code);return *code;}
        if(runtime_.host_clock)retired(memory,0);
        if(runtime_.image.base)last_error_=static_cast<std::uint32_t>(memory.load(GuestAddress{x86_?state.fs_base+0x34:state.gs_base+0x68},32));
    }
    struct PreserveClock {
        GuestState& state;HostServices& services;std::uint64_t tsc,nested;
        ~PreserveClock(){state.tsc=tsc+services.nested_instruction_count()-nested;}
    } clock{state,*this,state.tsc,nested_instruction_count()};
    const std::string_view trace=trace_services_;
    count_scope.phase("trace");
    const auto service_name=trace.empty()?std::string_view{}:name(state.rip);
    const bool tracing=!trace.empty() && (trace=="*" || trace.find(service_name)!=trace.npos);
    std::uint64_t trace_duplicate_output{};
    if(tracing && service_name=="DuplicateHandle")try{trace_duplicate_output=WindowsCall(state,memory,7)(3);}catch(const MemoryFault&){}
    if(tracing)std::fprintf(stderr,"[api] %.*s rcx=%llx rdx=%llx r8=%llx r9=%llx rsp=%llx\n",static_cast<int>(service_name.size()),service_name.data(),static_cast<unsigned long long>(state.reg(Gpr::Rcx)),static_cast<unsigned long long>(state.reg(Gpr::Rdx)),static_cast<unsigned long long>(state.reg(Gpr::R8)),static_cast<unsigned long long>(state.reg(Gpr::R9)),static_cast<unsigned long long>(state.reg(Gpr::Rsp)));
    if(tracing) {try {std::fprintf(stderr,"[api] return=%llx\n",static_cast<unsigned long long>(memory.load(GuestAddress{state.reg(Gpr::Rsp)},x86_?32:64)));}catch(const MemoryFault&) {}}
    if(tracing){
        std::fprintf(stderr,"[api] pid=%llu thread=%llu ticks=%llu\n",static_cast<unsigned long long>(process_id()),static_cast<unsigned long long>(threads_.current),static_cast<unsigned long long>(runtime_.clock_ticks));
        // 可選的 frame pointer 鏈僅供診斷；省略 frame pointer 的客體不冒稱完整堆疊。
        if(std::getenv("X64ARM_TRACE_SERVICE_FRAMES")){
            auto frame=state.reg(Gpr::Rbp);const auto width=x86_?4U:8U;
            for(unsigned depth=0;depth<24 && frame>=state.reg(Gpr::Rsp) && frame-state.reg(Gpr::Rsp)<16*1024*1024;++depth){
                try{
                    const auto next=memory.load(GuestAddress{frame},static_cast<std::uint8_t>(width*8));
                    const auto address=memory.load(GuestAddress{frame+width},static_cast<std::uint8_t>(width*8));
                    std::fprintf(stderr,"[api] frame%u fp=%llx return=%llx\n",depth,static_cast<unsigned long long>(frame),static_cast<unsigned long long>(address));
                    if(next<=frame)break;frame=next;
                }catch(const MemoryFault&){break;}
            }
        }
        // 診斷指定位置的客體狀態；僅讀取，不更動記憶體或 API 結果。
        static const auto observed_address=[]()->std::optional<std::uint64_t> {
            const auto* setting=std::getenv("X64ARM_TRACE_MEMORY_ADDRESS");if(!setting || !*setting)return std::nullopt;
            std::string_view value{setting};if(value.starts_with("0x") || value.starts_with("0X"))value.remove_prefix(2);
            std::uint64_t address{};const auto parsed=std::from_chars(value.data(),value.data()+value.size(),address,16);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())return std::nullopt;
            return address;
        }();
        if(observed_address)try {
            const auto value=memory.load(GuestAddress{*observed_address},64);
            std::fprintf(stderr,"[api] observe address=%llx value=%llx rbx=%llx rbp=%llx rsi=%llx rdi=%llx\n",static_cast<unsigned long long>(*observed_address),static_cast<unsigned long long>(value),static_cast<unsigned long long>(state.reg(Gpr::Rbx)),static_cast<unsigned long long>(state.reg(Gpr::Rbp)),static_cast<unsigned long long>(state.reg(Gpr::Rsi)),static_cast<unsigned long long>(state.reg(Gpr::Rdi)));
        }catch(const MemoryFault&){}
        try {
            const WindowsCall args(state,memory,6);
            const auto trace_text=[&](const char* label,std::uint64_t pointer,bool wide) {
                if(pointer){const auto value=wide?utf16_to_utf8(read_utf16(memory,GuestAddress{pointer})):read_ansi(memory,pointer);std::fprintf(stderr,"[api] %s=%s\n",label,value.c_str());}
            };
            if(service_name=="GetProcAddress") {
                if(args(1)>0xffff)trace_text("symbol",args(1),false);
                else std::fprintf(stderr,"[api] ordinal=%llu\n",static_cast<unsigned long long>(args(1)));
            }else if(service_name.starts_with("LoadLibrary"))trace_text("library",args(0),service_name.ends_with('W'));
            else if(service_name.starts_with("CreateFileMapping"))trace_text("mapping",args(5),service_name.ends_with('W'));
            else if(service_name.starts_with("OpenFileMapping"))trace_text("mapping",args(2),service_name.ends_with('W'));
            else if(service_name=="CreateProcessW" || service_name=="CreateProcessA")trace_text("command",args(1),service_name.ends_with('W'));
            else if(service_name.starts_with("RegOpenKey") || service_name.starts_with("RegCreateKey"))trace_text("registry_key",args(1),service_name.ends_with('W'));
            else if(service_name.starts_with("RegQueryValue") || service_name.starts_with("RegSetValue"))trace_text("registry_value",args(1),service_name.ends_with('W'));
            else if(service_name.starts_with("OpenService") || service_name.starts_with("CreateService"))trace_text("service_name",args(1),service_name.ends_with('W'));
            else if(service_name=="RoGetActivationFactory" || service_name=="RoActivateInstance"){
                const auto value=utf16_to_utf8(winrt_string_content(memory,args(0)));std::fprintf(stderr,"[api] runtime_class=%s\n",value.c_str());
                if(service_name=="RoGetActivationFactory" && args(1)){std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{args(1)},iid);std::fprintf(stderr,"[api] activation_iid=");for(const auto byte:iid)std::fprintf(stderr,"%02x",static_cast<unsigned>(byte));std::fprintf(stderr,"\n");}
            }
            else if(service_name=="GdiInterop.CreateFontFromLOGFONT")trace_text("logfont_face",args(1)+28,true);
            else if(service_name=="Factory.CreateTextFormat")trace_text("font_family",args(1),true);
            else if(service_name=="CreateFontA" || service_name=="CreateFontW"){
                const WindowsCall font_args(state,memory,14);trace_text("font_face",font_args(13),service_name.ends_with('W'));
                std::fprintf(stderr,"[api] font height=%d width=%d weight=%u charset=%u\n",static_cast<std::int32_t>(font_args(0)),static_cast<std::int32_t>(font_args(1)),static_cast<unsigned>(font_args(4)),static_cast<unsigned>(font_args(8)));
            }
            else if(service_name.starts_with("EnumFontFamiliesEx") || service_name.starts_with("CreateFontIndirect")){
                const auto pointer=args(service_name.starts_with("EnumFont")?1:0);
                trace_text("logfont_face",pointer+28,service_name.ends_with('W'));
                std::fprintf(stderr,"[api] logfont charset=%u pitch_family=%u flags=%x\n",static_cast<unsigned>(memory.load(GuestAddress{pointer+23},8)),static_cast<unsigned>(memory.load(GuestAddress{pointer+27},8)),service_name.starts_with("EnumFont")?static_cast<unsigned>(args(4)):0U);
            }
        }catch(const MemoryFault&){}catch(const std::invalid_argument&){}catch(const HostCallFault&){}
        if(service_name.starts_with("CreateMutex") || service_name.starts_with("CreateEvent") || service_name.starts_with("CreateSemaphore") || service_name.starts_with("CreateWaitableTimer")){
            try {
                const WindowsCall args(state,memory,6);const auto sa=args(0);
                if(sa){
                    const auto descriptor=memory.load(GuestAddress{sa+(x86_?4:8)},x86_?32:64);
                    if(descriptor){
                        const auto sd=security::read_descriptor(memory,descriptor,x86_,true);
                        std::fprintf(stderr,"[api] security control=%x owner_bytes=%zu group_bytes=%zu\n",sd.control,sd.data[0].size(),sd.data[1].size());
                        for(unsigned part:{2U,3U})if(!sd.data[part].empty()){
                            std::fprintf(stderr,"[api] %s=",part==2?"sacl":"dacl");
                            for(const auto byte:sd.data[part])std::fprintf(stderr,"%02x",byte);
                            std::fprintf(stderr,"\n");
                        }
                    }
                }
            }catch(const MemoryFault&){}catch(const SecurityDescriptorError&){}
        }
        if(service_name=="CreateFileA" || service_name=="CreateFileW" || service_name.starts_with("GetFileAttributes") || service_name.starts_with("DeleteFile") || service_name.starts_with("CreateDirectory") || service_name.starts_with("GetFullPathName") || service_name.starts_with("SetCurrentDirectory")){
            try{const auto pointer=x86_?memory.load(GuestAddress{state.reg(Gpr::Rsp)+4},32):state.reg(Gpr::Rcx);const auto path=service_name.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{pointer})):read_ansi(memory,pointer);std::fprintf(stderr,"[api] path=%s\n",path.c_str());}catch(const MemoryFault&){}catch(const std::invalid_argument&){}
        }
    }
    if(tracing && state.rip.value>=stub_base_.value && state.rip.value<stub_base_.value+(services.size()+runtime_service_descriptors().size())*16) {
        const auto index=(state.rip.value-stub_base_.value)/16;const auto arguments=index<services.size()?services[index].arguments:runtime_service_descriptors()[index-services.size()].arguments;
        const auto wide=index<services.size()?services[index].wide_arguments:runtime_service_descriptors()[index-services.size()].wide_arguments;
        const auto aggregate=index<services.size()?0U:runtime_service_descriptors()[index-services.size()].aggregate_arguments;
        for(unsigned i=x86_?0:4;i<arguments;++i) {try {std::fprintf(stderr,"[api] arg%u=%llx\n",i,static_cast<unsigned long long>(WindowsCall(state,memory,arguments,true,wide,aggregate)(i)));}catch(const MemoryFault&) {break;}}
    }
    count_scope.phase("body");
    const auto result = invoke_service(state, memory);
    count_scope.phase("completion");
    // 記憶體操作可合法寫入 TEB 本身；完整入口也不得用呼叫前的快照蓋回輸出。
    if((memory_operation || string_operation) && runtime_.image.base)last_error_=static_cast<std::uint32_t>(memory.load(GuestAddress{x86_?state.fs_base+0x34:state.gs_base+0x68},32));
    if(tracing)std::fprintf(stderr,"[api] %.*s rax=%llx last_error=%u errno=%llu\n",static_cast<int>(service_name.size()),service_name.data(),static_cast<unsigned long long>(state.reg(Gpr::Rax)),last_error_,static_cast<unsigned long long>(memory.load(GuestAddress{runtime_.errno_cell},32)));
    if(trace_duplicate_output && state.reg(Gpr::Rax))try{std::fprintf(stderr,"[api_duplicate_output] pid=%u thread=%llu handle=%llx\n",process_id(),static_cast<unsigned long long>(threads_.current),static_cast<unsigned long long>(memory.load(GuestAddress{trace_duplicate_output},x86_?32:64)));}catch(const MemoryFault&){}
    if (runtime_.image.base) memory.store(GuestAddress{x86_ ? state.fs_base + 0x34 : state.gs_base + 0x68}, 32, last_error_);
    return result;
}
std::optional<std::uint32_t> WindowsServices::invoke_service(GuestState& state, GuestMemory& memory) {
    if (!handles(state.rip)) {
        char text[96];std::snprintf(text,sizeof text,"未註冊的 Windows 服務入口 rip=0x%llx",static_cast<unsigned long long>(state.rip.value));
        throw HostCallFault(text);
    }
    if(state.rip.value==callback_return_.value+0xe0)return finish_system_call(state,memory);
    if(!stock_gui_class(state.rip.value).empty())return invoke_stock_gui(state,memory);
    if (state.rip.value >= unresolved_base_.value && state.rip.value - unresolved_base_.value < unresolved_imports_.size() * 16)
        throw HostCallFault("呼叫尚未實作的 PE import：" + std::string(name(state.rip)));
    if(state.rip.value==callback_return_.value+0x60 || state.rip.value==callback_return_.value+0x70){
        const bool dispatch=state.rip.value==callback_return_.value+0x70;
        const auto raw_target=state.reg(dispatch?Gpr::Rax:Gpr::Rcx);const auto target=x86_?static_cast<std::uint32_t>(raw_target):raw_target;
        if((x86_ && dispatch) || !guard_target(memory,target))return 0xc0000409U;
        if(guard_fast_table_.value && trace_cfg_guard()){
            ++guard_statistics.misses;
            try{
                const auto set=guard_fast_table_.value+(guard_table_index(target)&(guard_two_way()?~std::size_t{1}:~std::size_t{0}))*8;
                const auto slot=memory.load(GuestAddress{set},64);
                if(slot!=UINT64_MAX && slot!=target && (!guard_two_way() || memory.load(GuestAddress{set+8},64)!=UINT64_MAX))++guard_statistics.collisions;
            }catch(const std::exception&){}
        }
        remember_guard_target(memory,target);
        if(dispatch)state.rip=GuestAddress{target};
        else {const auto return_address=memory.load(GuestAddress{state.reg(Gpr::Rsp)},x86_?32:64);state.rip=GuestAddress{return_address};state.reg(Gpr::Rsp)+=x86_?4U:8U;}
        return std::nullopt;
    }
    if (state.rip.value == callback_return_.value+0xa0) return force_finish_thread(state,memory);
    if (state.rip.value == callback_return_.value+0x80) return exit_thread(state,memory,0);
    if (state.rip == thread_return_) return exit_thread(state, memory, static_cast<std::uint32_t>(state.reg(Gpr::Rax)));
    if (state.rip == process_exit_retry_) return finish_process(state, memory, static_cast<std::uint32_t>(state.reg(Gpr::Rcx)));
    if (state.rip == thread_start_) {
        if(await_loader(state,memory))return std::nullopt;
        state.rip = GuestAddress{threads_.items.at(threads_.current).entry};
        auto callbacks=module_thread_callbacks(2);
        for (const auto target : runtime_.image.tls_callbacks) callbacks.push_back({target, {runtime_.image.base, 2, 0}, true});
        if (!callbacks.empty()) start_loader_callbacks(state, memory, std::move(callbacks), Continuation::Startup);
        else (void)dispatch_user_apc(state,memory,UINT64_MAX);
        return std::nullopt;
    }
    if (state.rip == exception_entry_) return advance_exception(state, memory);
    if (state.rip == callback_return_) return resume_callback(state, memory);
    if (state.rip == startup_entry_) {
        auto initial = state; initial.rip = GuestAddress{runtime_.image.base + memory.load(GuestAddress{runtime_.image.base + memory.load(GuestAddress{runtime_.image.base + 60}, 32) + 40}, 32)};
        state = initial;
        ModuleOperation operation;operation.startup=true;operation.created=runtime_.module_order;
        return start_module_operation(state,memory,std::move(operation));
    }
    const auto index = (state.rip.value - stub_base_.value) / 16;
    if(index>=services.size()){
        const auto& descriptor=runtime_service_descriptors()[index-services.size()];
        if(await_heap_service(state,memory,descriptor))return std::nullopt;
        return invoke_runtime(state,memory,descriptor);
    }
    const auto& service=services[index];
    if(await_heap_service(state,memory,{"kernel32.dll",service.name,service.arguments,service.wide_arguments}))return std::nullopt;
    const auto symbol = service.name;
    const WindowsCall frame(state, memory, service.arguments,true,service.wide_arguments);
    const auto arg = [&](unsigned index) { return frame(index); };
    const auto dword = [&](unsigned index) { return static_cast<std::uint32_t>(arg(index)); };
    if (symbol == "ExitProcess") return finish_process(state, memory, dword(0));
    std::uint64_t returned = 0;
    const auto fail = [&](std::uint32_t error) { last_error_ = error; returned = 0; };
    if(symbol=="OutputDebugStringA" || symbol=="OutputDebugStringW"){
        if(!arg(0)){frame.return_to(state,0);return std::nullopt;}
        const auto text=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
        if(debugger_attached()){
            const auto length=symbol.ends_with('W')?read_utf16(memory,GuestAddress{arg(0)}).size()+1:text.size()+1;
            auto caller=state;frame.return_to(caller,0);
            (void)emit_debug_event(caller,memory,debugger::DebugString{arg(0),static_cast<std::uint16_t>(std::min<std::size_t>(length,UINT16_MAX)),symbol.ends_with('W')});
            state=caller;return std::nullopt;
        }
        if(stderr_.size()<output_limit)stderr_.append(text,0,output_limit-stderr_.size());
        // 子程序可能長期存活；選用即時診斷時也交付偵錯字串，避免必須終止程序才看到初始化失敗原因。
        static const bool live_debug=std::getenv("X64ARM_TRACE_DEBUG_STRING")!=nullptr;
        if(live_debug){std::fwrite(text.data(),1,text.size(),stderr);std::fflush(stderr);}
    }else if (symbol == "GetStdHandle") {
        switch (dword(0)) {
        case static_cast<std::uint32_t>(-10): returned = runtime_.standard_handles[0]; break;
        case static_cast<std::uint32_t>(-11): returned = runtime_.standard_handles[1]; break;
        case static_cast<std::uint32_t>(-12): returned = runtime_.standard_handles[2]; break;
        default: returned = UINT64_MAX; last_error_ = 87; break;
        }
    } else if (symbol == "GetLastError") returned = last_error_;
    else if (symbol == "SetLastError") last_error_ = dword(0);
    else if (symbol == "GetCommandLineW") {
        if (!initialized_) throw HostCallFault("Windows command line 尚未初始化");
        returned = command_line_address_.value;
    } else if (symbol == "GetProcessHeap") returned = process_heap;
    else if(symbol=="HeapSetInformation" || symbol=="HeapQueryInformation") {
        if(arg(0)!=process_heap && !runtime_.private_heaps.contains(arg(0)))fail(6);
        else if(dword(1)!=0)fail(50);
        else if(arg(3)<4 || !arg(2)){if(symbol=="HeapQueryInformation" && arg(4))memory.store(GuestAddress{arg(4)},x86_?32:64,4);fail(122);}
        else if(symbol=="HeapQueryInformation"){
            memory.store(GuestAddress{arg(2)},32,0);if(arg(4))memory.store(GuestAddress{arg(4)},x86_?32:64,4);returned=1;
        } else if(memory.load(GuestAddress{arg(2)},32)!=0)fail(50); // 尚未實作 LFH；明確拒絕此可選策略。
        else returned=1;
    }
    else if(symbol=="GetProcessHeaps") {
        const auto count=runtime_.private_heaps.size()+1,capacity=static_cast<std::size_t>(dword(0)),copied=std::min(count,capacity);const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
        if(copied && !arg(1))fail(87);
        else {memory.check(GuestAddress{arg(1)},copied*unit,Access::Write);
            if(copied){memory.store(GuestAddress{arg(1)},bits,process_heap);std::size_t i=1;for(const auto& [heap,limits]:runtime_.private_heaps){(void)limits;if(i==copied)break;memory.store(GuestAddress{arg(1)+i++*unit},bits,heap);}}
            returned=count;}
    } else if(symbol=="HeapWalk") {
        const auto heap=arg(0);const unsigned unit=x86_?4:8,size=x86_?28:40;const std::uint8_t bits=x86_?32:64;
        if(heap!=process_heap && !runtime_.private_heaps.contains(heap))fail(6);
        else if(!arg(1))fail(87);
        else {
            memory.check(GuestAddress{arg(1)},size,Access::Write);const auto previous=memory.load(GuestAddress{arg(1)},bits);
            const auto belongs=[&](std::uint64_t pointer){const auto owner=runtime_.heap_owners.find(pointer);return owner==runtime_.heap_owners.end()?heap==process_heap:owner->second==heap;};
            // x86 配置區的已配置區塊依 chunk 擁有者歸屬 heap，與一般區塊依位址合併列舉。
            static const std::vector<std::pair<std::uint64_t,std::uint64_t>> no_guest_blocks;
            const auto& guest=guest_heap_chunked_?guest_heap_allocated_blocks(memory,heap):no_guest_blocks;
            const auto guest_after=[&](std::uint64_t at){return std::upper_bound(guest.begin(),guest.end(),std::pair<std::uint64_t,std::uint64_t>{at,UINT64_MAX});};
            const bool previous_guest=previous && std::binary_search(guest.begin(),guest.end(),std::pair<std::uint64_t,std::uint64_t>{previous,0},
                [](const auto& a,const auto& b){return a.first<b.first;});
            if(previous && !previous_guest && (!allocations_.contains(previous) || !belongs(previous)))fail(87);
            else {auto block=previous?allocations_.upper_bound(previous):allocations_.begin();while(block!=allocations_.end() && !belongs(block->first))++block;
                const auto next_guest=previous?guest_after(previous):guest.begin();
                const bool use_guest=next_guest!=guest.end() && (block==allocations_.end() || next_guest->first<block->first);
                const auto entry_address=use_guest?next_guest->first:block!=allocations_.end()?block->first:0;
                const auto entry_size=use_guest?next_guest->second:block!=allocations_.end()?static_cast<std::uint64_t>(block->second):0;
                if(!use_guest && block==allocations_.end())fail(259);
                else if(entry_size>UINT32_MAX)fail(534);
                else {memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(size));memory.store(GuestAddress{arg(1)},bits,entry_address);memory.store(GuestAddress{arg(1)+unit},32,entry_size);memory.store(GuestAddress{arg(1)+unit+6},16,4);returned=1;}
            }
        }
    }
    else if(symbol=="HeapLock" || symbol=="HeapUnlock"){
        const auto heap=arg(0);
        if(heap!=process_heap && !runtime_.private_heaps.contains(heap))fail(6);
        else if(runtime_.unserialized_heaps.contains(heap))fail(87);
        else{
            auto& lock=runtime_.heap_locks[heap];
            if(symbol=="HeapLock"){
                if(lock.recursion==UINT32_MAX)fail(534);
                else {lock.owner=threads_.current;++lock.recursion;returned=1;}
            }else if(lock.owner!=threads_.current || !lock.recursion)fail(288);
            else {if(!--lock.recursion)lock.owner=0;returned=1;threads_.yield=true;}
        }
    }
    else if(symbol=="HeapCreate") {
        if(dword(0)&~1U)fail(50);
        else if(arg(1)>memory.limit() || arg(2)>UINT64_MAX-4095 || (arg(2) && arg(1)>arg(2)))fail(8);
        else {returned=runtime_.allocate_wait_handle();runtime_.private_heaps.emplace(returned,std::pair{arg(2)?(arg(2)+4095)&~UINT64_C(4095):0,UINT64_C(0)});if(dword(0)&1)runtime_.unserialized_heaps.insert(returned);
            // 可成長私有 heap（如靜態 CRT 的 _crtheap）可改用級距配置區；有上限者維持一般 heap 的容量契約。
            if(guest_heap_chunked_ && !arg(2) && guest_heap_private_requested())guest_private_heaps_.emplace(returned,GuestPrivateHeap{});}
    } else if(symbol=="HeapDestroy") {
        const auto heap=runtime_.private_heaps.find(arg(0));
        if(heap==runtime_.private_heaps.end())fail(6);
        else {std::vector<std::uint64_t> pointers;for(const auto& [pointer,owner]:runtime_.heap_owners)if(owner==arg(0))pointers.push_back(pointer);
            for(const auto pointer:pointers)(void)free_heap(memory,pointer,state);
            // 釋放時可能進入快速清單；移除 heap 前把它的區塊全部歸還，頁面與用量帳目隨之結清。
            flush_heap_quick(memory,arg(0));heap_quick_bins_.erase(arg(0));guest_heap_destroy(memory,arg(0));runtime_.private_heaps.erase(heap);runtime_.heap_locks.erase(arg(0));runtime_.unserialized_heaps.erase(arg(0));returned=1;}
    } else if(symbol=="HeapAlloc" || symbol=="HeapReAlloc" || symbol=="HeapFree" || symbol=="HeapSize" || symbol=="HeapValidate") {
        const auto heap=arg(0),pointer=arg(2);const bool valid=heap==process_heap || runtime_.private_heaps.contains(heap);
        struct HeapPathScope {
            bool active;std::uint64_t* calls{};std::uint64_t* nanoseconds{};std::chrono::steady_clock::time_point begin;
            ~HeapPathScope(){if(active && calls){++*calls;*nanoseconds+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count());}}
        } heap_path{trace_heap_paths(),nullptr,nullptr,trace_heap_paths()?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{}};
        if(heap_path.active){
            report_heap_paths(process_id(),allocations_.size());auto& s=heap_path_stats;
            const bool guest=guest_heap_contains(pointer) && symbol!="HeapAlloc";
            if(symbol=="HeapAlloc"){heap_path.calls=&s.guest_alloc;heap_path.nanoseconds=&s.guest_alloc_ns;}
            else if(symbol=="HeapFree" && guest){heap_path.calls=&s.guest_free;heap_path.nanoseconds=&s.guest_free_ns;}
            else if(symbol=="HeapFree"){heap_path.calls=&s.general_free;heap_path.nanoseconds=&s.general_free_ns;
                if(const auto found=allocations_.find(pointer);found!=allocations_.end() && found->second>4096)++s.general_large_free;}
            else {heap_path.calls=&s.other;heap_path.nanoseconds=&s.other_ns;}
        }
        const auto allocation=allocations_.find(pointer);const auto owner=runtime_.heap_owners.find(pointer);
        const bool owned=allocation!=allocations_.end() && (owner==runtime_.heap_owners.end()?heap==process_heap:owner->second==heap);
        const auto flags_value=dword(1);const auto allowed=symbol=="HeapAlloc"?9U:symbol=="HeapReAlloc"?0x19U:1U;
        if(valid && (symbol=="HeapAlloc" || symbol=="HeapReAlloc") && (flags_value&4))throw HostCallFault("尚未支援 HEAP_GENERATE_EXCEPTIONS 的記憶體不足例外");
        if(!valid || (flags_value&~allowed)){fail(87);if(symbol=="HeapSize")returned=x86_?UINT32_MAX:UINT64_MAX;}
        else if(symbol=="HeapAlloc"){
            // x86 process heap 與可成長私有 heap 的小型配置由 chunk 級距配置區提供（O(1) 串列，不經位址搜尋與
            // 存活物件樹）；HeapFree／HeapSize／HeapReAlloc／HeapValidate 依位址與 chunk 擁有者辨識這些區塊。
            // 不足時沿用一般 heap。HeapLock 期間（含持有者自己的配置）改走一般 heap，讓鎖定中的 HeapWalk 與既有登錄一致。
            const auto heap_lock=runtime_.heap_locks.find(heap);
            const bool heap_locked=heap_lock!=runtime_.heap_locks.end() && heap_lock->second.owner;
            if(guest_heap_chunked_ && !heap_locked && !(flags_value&~9U) && (heap==process_heap || guest_heap_private_ready(heap)))
                if(const auto block=guest_heap_allocate(memory,std::max<std::uint64_t>(UINT64_C(1),arg(2)),heap)){
                    if(flags_value&8U)memory.fill(GuestAddress{block},0,static_cast<std::size_t>(std::max<std::uint64_t>(UINT64_C(1),arg(2))));
                    returned=block;
                }
            if(!returned){
                if(heap_path.active){heap_path.calls=&heap_path_stats.general_alloc;heap_path.nanoseconds=&heap_path_stats.general_alloc_ns;
                    auto& st=heap_path_stats;
                    if(heap!=process_heap){++st.skip_heap;st.last_heap=heap;}else if(heap_locked)++st.skip_locked;else if(flags_value&~9U){++st.skip_flags;st.last_flags=flags_value;}else ++st.skip_full;}
                returned=allocate_heap(memory,arg(2),heap);
            }
        }
        else if(guest_heap_contains(pointer) && guest_heap_owner(memory,pointer)==heap){
            // malloc 配置區區塊同屬 process heap：與 UCRT 相同可經 Heap* API 查詢、重配或釋放；私有 heap 的區塊依 chunk 擁有者歸屬。
            const auto size=guest_heap_block_size(memory,pointer);
            if(symbol=="HeapFree"){guest_heap_free(memory,pointer);returned=1;}
            else if(symbol=="HeapSize")returned=size;
            else if(symbol=="HeapValidate")returned=1;
            else if(arg(3)<=size)returned=pointer;
            else if(!(flags_value&0x10)){
                returned=allocate_heap(memory,arg(3),heap);
                if(returned){std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));memory.read(GuestAddress{pointer},bytes);
                    memory.write(GuestAddress{returned},bytes);
                    if(flags_value&8)memory.write(GuestAddress{returned+size},std::vector<std::uint8_t>(static_cast<std::size_t>(arg(3)-size),0));
                    guest_heap_free(memory,pointer);}
            }
        }
        else if(symbol=="HeapValidate")returned=validate_heap(memory,heap,pointer);
        else if(!owned && (pointer || symbol!="HeapFree")){fail(87);if(symbol=="HeapSize")returned=x86_?UINT32_MAX:UINT64_MAX;}
        else if(symbol=="HeapFree")returned=free_heap(memory,pointer,state);
        else if(symbol=="HeapSize")returned=allocation->second;
        else if(arg(3)<=allocation->second)returned=pointer;
        else if(!(flags_value&0x10))returned=reallocate_heap(memory,pointer,arg(3),state);
    } else if (symbol == "CreateFileW" || symbol=="CreateFileA" || symbol=="CreateFile2") {
        return invoke_file_open(state,memory,{"kernel32.dll",symbol,symbol=="CreateFile2"?5U:7U});
    } else if (symbol == "CloseHandle") {
        const auto handle = arg(0);
        if (runtime_.handle_flags[handle]&2)fail(5);
        else if (handle >= 0x100 && handle <= 0x102 && standard_open_[handle - 0x100]) { standard_open_[handle - 0x100] = false; returned = 1; }
        else if (const auto file=files_.find(handle);file!=files_.end()) {
            close_transfer_handle(memory,handle);
            returned=1;
        }
        else if (kernel_device_handle(handle)) return close_kernel_device(state,memory,handle);
        else if (power_requests_.erase(handle)) returned = 1;
        else if (close_transaction(handle)) returned = 1;
        else if (close_bluetooth(handle)) returned = 1;
        else if (close_mapping(handle)) returned = 1;
        else if (close_job(handle)) returned = 1;
        else if (close_process(handle)) returned = 1;
        else if (runtime_.completion_ports.erase(handle)) returned = 1;
        else if (runtime_.process_snapshots.erase(handle)) returned = 1;
        else if (close_wait_object(handle)) returned=1;
        else if (runtime_.process_thread_handles.erase(handle)) {runtime_.thread_handle_access.erase(handle);returned = 1;}
        else if (runtime_.token_handles.erase(handle)) {runtime_.token_handle_objects.erase(handle);collect_tokens();returned = 1;}
        else fail(6);
        if(returned)runtime_.handle_flags.erase(handle);
    } else if (symbol == "GetFileSizeEx") {
        const auto file=file_handle_information(arg(0));
        if(file.error)fail(file.error);
        else if(!arg(1))fail(87);
        else {memory.store(GuestAddress{arg(1)},64,file.metadata.size);returned=1;}
    } else if (symbol == "SetFilePointerEx") {
        const auto found = files_.find(arg(0));
        if (found == files_.end()) fail(6);
        else if(found->second.serial || found->second.hid)fail(1);
        else {
            const auto position_guard=runtime_.win_file_positions.at(found->second.object).lock();
            const auto method = dword(3);
            const auto distance = arg(1);
            __int128 position = std::bit_cast<std::int64_t>(distance);
            if (method != 0) {
                if(found->second.native)found->second.metadata=found->second.native->information().metadata;
                position += method == 1 ? runtime_.win_file_positions.at(found->second.object) : found->second.metadata.size;
            }
            if (method > 2) fail(87);
            else if (position < 0) fail(131);
            else if (position > INT64_MAX) fail(87);
            else {
                if (arg(2)) memory.store(GuestAddress{arg(2)}, 64, static_cast<std::uint64_t>(position));
                runtime_.win_file_positions.at(found->second.object) = static_cast<std::uint64_t>(position); returned = 1;
            }
        }
    } else if(symbol=="CreatePipe"){
        return invoke_file_io(state,memory,{"kernel32.dll",symbol,4,0,0,8});
    } else if (symbol == "ReadFile" || symbol == "WriteFile" || symbol=="ReadFileEx" || symbol=="WriteFileEx") {
        const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
        const bool extended=symbol.ends_with("Ex"),writing=symbol.starts_with("Write");const auto handle=arg(0),overlapped=arg(extended?3:4),count_pointer=extended?0:arg(3);const auto count=dword(2);
        const auto found=files_.find(handle);const bool standard=handle>=0x100 && handle<=0x102 && standard_open_[handle-0x100];
        if(found!=files_.end() && (found->second.pipe || found->second.serial || found->second.hid))return invoke_file_io(state,memory,{"kernel32.dll",symbol,5});
        if(found!=files_.end() && found->second.usb){if(count_pointer)memory.store(GuestAddress{count_pointer},32,0);fail(1);} // WinUSB 裝置 handle 只經 WinUsb_* 傳輸。
        else if(extended && (!overlapped || !arg(4)))fail(87);
        else if(extended && (standard || (found!=files_.end() && ((found->second.mode&0x30) || found->second.completion_port))))fail(87);
        else if(extended && (threads_.items.at(threads_.current).apcs.size()+static_cast<std::size_t>(std::count_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& io){return io.thread==threads_.current && io.routine;}))>=1024))fail(8);
        else if (!count_pointer && !overlapped) fail(87);
        else if (!standard && found==files_.end()) {if(count_pointer)memory.store(GuestAddress{count_pointer},32,0);fail(6);}
        else if (standard && ((writing && handle==0x100) || (!writing && handle!=0x100))) fail(5);
        else if (!standard && ((found->second.metadata.attributes&0x10) || (writing?!(found->second.access&(0x40000000U|2U|4U)):!(found->second.access&(0x80000000U|1U))))) {if(count_pointer)memory.store(GuestAddress{count_pointer},32,0);fail(5);}
        else {
            // 同步客體記憶體 fault 必須在 count／signal／position 變更前派送。
            // OVERLAPPED 的搬移錯誤則保留後方 NTSTATUS／completion／998 契約。
            if(!overlapped){
                auto extent=std::uint64_t{count};
                if(!writing && !standard){const auto size=found->second.native?found->second.native->current_size():found->second.bytes->size();
                    const auto offset=runtime_.win_file_positions.at(found->second.object);extent=std::min<std::uint64_t>(count,size>offset?size-offset:0);}
                memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(extent),writing?Access::Read:Access::Write);
            }
            if(count_pointer){memory.check(GuestAddress{count_pointer},4,Access::Write);memory.store(GuestAddress{count_pointer},32,0);}
            const auto position_guard=standard?std::shared_ptr<void>{}:runtime_.win_file_positions.at(found->second.object).lock();
            // FilePosition 的隱含轉型若與 int 分支混用，會截成有號 32 位元；同步大檔 I/O 必須保留完整游標。
            std::uint64_t offset=standard?UINT64_C(0):static_cast<std::uint64_t>(runtime_.win_file_positions.at(found->second.object)),event=0;
            if(overlapped) {
                memory.check(GuestAddress{overlapped},x86_?20:32,Access::Write);offset=memory.load(GuestAddress{overlapped+2*unit},64);event=extended?0:memory.load(GuestAddress{overlapped+(x86_?16:24)},bits)&~UINT64_C(1);
                if(event && (!runtime_.wait_objects.contains(event) || runtime_.wait_objects.at(event).kind!=WaitObject::Kind::Event)) {fail(6);frame.return_to(state,returned);return std::nullopt;}
            }
            if(!standard)set_file_io_signaled(handle,false);
            if(standard && !writing && count) {
                memory.check(GuestAddress{arg(1)},count,Access::Write);
                if(!await_standard_input(state,memory,1))return std::nullopt;
            }
            auto extent=std::uint64_t{count};
            if(!writing) {
                const auto size=standard?runtime_.standard_input.bytes.size():found->second.native?found->second.native->current_size():found->second.bytes->size();
                const auto position=standard?runtime_.standard_input.position:offset;extent=std::min<std::uint64_t>(count,size>position?size-position:0);
            }
            FileIoResult io;
            if(!standard && found->second.native){
                auto& file=found->second;
                if(writing && (offset==UINT64_MAX || (!(file.access&(0x40000000U|2U)) && (file.access&4U))))offset=file.native->current_size();
                // 整體範圍先檢查，避免後段的鎖衝突造成先寫入前段。
                io.error=file.native->validate_io(offset,count,writing);
            }
            process_io::Batch io_request(writing?process_io::Kind::Write:process_io::Kind::Read);
            if(!io.error)try{
                memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(extent),writing?Access::Read:Access::Write);
                // DWORD 是單次 API 的長度；暫存區大小只限制分段搬移，不限制合法 I/O。
                std::vector<std::uint8_t> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(extent,1024U*1024U)));
                bool first=true;
                while(first || io.bytes<extent){
                    first=false;const auto length=static_cast<std::size_t>(std::min<std::uint64_t>(extent-io.bytes,buffer.size()));auto part=std::span(buffer).first(length);
                    if(writing)memory.read(GuestAddress{arg(1)+io.bytes},part);FileIoResult transferred;
                    if(standard && writing)transferred=write_standard_output(static_cast<unsigned>(handle-0x100),part);
                    else if(standard){auto& input=runtime_.standard_input;transferred.bytes=std::min<std::uint64_t>(length,input.bytes.size()-input.position);std::copy_n(input.bytes.begin()+static_cast<std::ptrdiff_t>(input.position),static_cast<std::size_t>(transferred.bytes),part.begin());input.position+=transferred.bytes;process_io::record(process_io::Kind::Read,transferred.bytes);}
                    else if(found->second.native)transferred=writing?found->second.native->write(part,offset+io.bytes):found->second.native->read(part,offset+io.bytes);
                    else if(writing)transferred.error=5;
                    else{const auto& bytes=*found->second.bytes;const auto position=static_cast<std::size_t>(std::min<std::uint64_t>(offset+io.bytes,bytes.size()));transferred.bytes=std::min<std::uint64_t>(length,bytes.size()-position);std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(position),static_cast<std::size_t>(transferred.bytes),part.begin());process_io::record(process_io::Kind::Read,transferred.bytes);}
                    if(!writing && transferred.bytes)memory.write(GuestAddress{arg(1)+io.bytes},part.first(static_cast<std::size_t>(transferred.bytes)));
                    io.bytes+=transferred.bytes;io.error=transferred.error;if(io.error || transferred.bytes<length || !length)break;
                }
                if(writing && !standard && found->second.native && io.bytes)found->second.metadata=found->second.native->information().metadata;
            }catch(const std::bad_alloc&){io.error=8;}catch(const MemoryFault&){io.error=998;}
            // 診斷：X64ARM_TRACE_FILE_READ=<路徑片段> 時，記錄符合檔案每次同步讀寫的位移、長度與宿主時間。
            if(static const char* const read_trace=std::getenv("X64ARM_TRACE_FILE_READ");read_trace && *read_trace && !standard &&
               found->second.path.find(read_trace)!=std::string::npos)
                std::fprintf(stderr,"file_read_trace pid=%u op=%.*s path=%s offset=%llu count=%u bytes=%llu overlapped=%d unix_us=%lld\n",process_id(),
                    static_cast<int>(symbol.size()),symbol.data(),found->second.path.c_str(),static_cast<unsigned long long>(offset),count,
                    static_cast<unsigned long long>(io.bytes),overlapped?1:0,static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
            if(io.error && std::getenv("X64ARM_TRACE_FILE_ERRORS"))std::fprintf(stderr,"[file_io] %.*s handle=%llu offset=%llu count=%u transferred=%llu error=%u\n",static_cast<int>(symbol.size()),symbol.data(),static_cast<unsigned long long>(handle),static_cast<unsigned long long>(offset),count,static_cast<unsigned long long>(io.bytes),io.error);
            if(count_pointer)memory.store(GuestAddress{count_pointer},32,io.bytes);
            if(!io.error && !standard && (!overlapped || (found->second.mode&0x30)))runtime_.win_file_positions.at(found->second.object)=offset+io.bytes;
            if(overlapped) {
                if(!writing && !io.error && !io.bytes && count)io.error=38;
                const auto status=io.error?(io.error==38?0xc0000011U:io.error==5?0xc0000022U:io.error==33?0xc0000054U:io.error==8?0xc0000017U:io.error==998?0xc0000005U:0xc000000dU):0U;
                memory.store(GuestAddress{overlapped},bits,status);memory.store(GuestAddress{overlapped+unit},bits,io.bytes);if(event)runtime_.wait_objects.at(event).set_signal(true);
                if(!extended && !io.error && !standard && ((found->second.mode&0x30) || !(found->second.completion_modes&1)) && !(memory.load(GuestAddress{overlapped+(x86_?16:24)},bits)&1)) {
                    const auto port=runtime_.completion_ports.find(found->second.completion_port);
                    if(port!=runtime_.completion_ports.end())port->second.packets.push_back({found->second.completion_key,overlapped,status,static_cast<std::uint32_t>(io.bytes)});
                }
            }
            if(!standard)set_file_io_signaled(handle,true);
            if(extended && (!io.error || io.error==38)){
                threads_.items.at(threads_.current).apcs.push_back({arg(4),{io.error,io.bytes,overlapped}});returned=1;threads_.yield=true;
            }else if(io.error)fail(io.error);else returned=1;
        }
    } else throw HostCallFault("服務名稱尚未實作");
    frame.return_to(state, returned);
    return std::nullopt;
}
}
