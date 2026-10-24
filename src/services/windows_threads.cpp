#include "x64arm/startup_profile.hpp"
#include <chrono>
#include <array>
#include <cerrno>
#include <cfenv>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#include "x64arm/windows_services.hpp"
#include "srw_trace.hpp"
#include "resident_service_policy.hpp"
#include "guest_crt_state.h"
#include "windows_loader_lock.hpp"
#include "x64arm/guest_address_policy.hpp"
#include "x64arm/windows_unwind.hpp"
#include "x64arm/interpreter.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include "x64arm/platform.hpp"
#include "x64arm/shared_wake.hpp"
#include "socket_database.hpp"
#include "guest_io_completion_code.hpp"
#include "io_wait_policy.hpp"
#include "monotonic_clock.hpp"
namespace x64arm {
namespace {

// 明確啟用的冷邊界診斷；每個 stream 最多 2 MiB，格式為固定八個 uint64。
// 前三筆固定為 header／計數／狀態；資料從 byte 192 開始。診斷失敗不得影響客體。
#if !defined(_WIN32)
class AdmissionTrace {
    using Record=std::array<std::uint64_t,8>;
    static_assert(sizeof(Record)==64);
    struct Environment {
        int error{errno};std::fenv_t fp{};bool saved{std::fegetenv(&fp)==0};
        ~Environment(){if(saved)std::fesetenv(&fp);errno=error;}
    };
    static std::uint64_t now() noexcept {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static std::uint64_t cpu() noexcept {
        timespec value{};return clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value)==0?
            std::uint64_t(value.tv_sec)*UINT64_C(1000000000)+std::uint64_t(value.tv_nsec):0;
    }
    struct State {
        static constexpr std::size_t capacity=(2*1024*1024-192)/64;
        std::mutex mutex;int fd{-1};bool failed{};
        std::array<Record,128> buffer{};
        // 各事件獨立限流／合併，避免 selected 緊接著的 release 記錄被餓掉。
        std::array<Record,32> previous{};std::array<std::uint64_t,32> next_sample{};
        std::array<bool,32> have_previous{};
        std::size_t buffered{},written{};
        std::uint64_t seen{},coalesced{},limited{},dropped{},errors{},cpu_ns{},last_flush{},last_seen{};
        bool opened{};
        bool write_at(const void* data,std::size_t size,std::size_t offset) noexcept {
            const auto* bytes=static_cast<const char*>(data);
            while(size){const auto n=::pwrite(fd,bytes,size,static_cast<off_t>(offset));
                if(n<0 && errno==EINTR)continue;
                if(n<=0){failed=true;++errors;return false;}
                bytes+=n;size-=static_cast<std::size_t>(n);offset+=static_cast<std::size_t>(n);
            }return true;
        }
        void open() noexcept {
            if(opened)return;opened=true;
            char path[1024];const auto* directory=std::getenv("X64ARM_TRACE_ADMISSION_DIRECTORY");
            const int size=std::snprintf(path,sizeof(path),"%s/admission-%d-provider.bin",directory?directory:"",int(::getpid()));
            if(size<0 || std::size_t(size)>=sizeof(path)){failed=true;++errors;return;}
            fd=::open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            if(fd<0){failed=true;++errors;return;}
            // 開檔完成後相鄰取樣，避免將磁碟延遲算入跨 stream 的時基偏差。
            const auto anchor=now();
            const auto wall=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            const Record header{anchor,0,1,std::uint64_t(::getpid()),wall,64,2*1024*1024,1};
            write_at(header.data(),sizeof(header),0);last_flush=anchor;
        }
        void flush(std::uint64_t stamp,bool terminal=false) noexcept {
            if(fd<0 || failed)return;
            if(buffered){
                if(!write_at(buffer.data(),buffered*64,192+written*64))return;
                written+=buffered;buffered=0;
            }
            const Record counts{stamp,254,seen,coalesced,limited,dropped,errors,cpu_ns};
            const Record status{stamp,255,written,buffered,last_seen,terminal?1U:0U,0,0};
            if(write_at(counts.data(),64,64))write_at(status.data(),64,128);
            last_flush=stamp;
        }
        ~State(){Environment environment;flush(now(),true);if(fd>=0)::close(fd);}
    };
    static State& storage(){static State state;return state;}
public:
    static bool enabled() noexcept {
        static const bool value=[] {const auto* path=std::getenv("X64ARM_TRACE_ADMISSION_DIRECTORY");return path && *path;}();
        return value;
    }
    template<class Build> static void capture(std::uint64_t interval,std::uint64_t event,
            std::uint64_t provider,std::uint64_t current,Build&& build) noexcept {
        if(!enabled())return;
        Environment environment;if(!environment.saved)return;
        try{
            auto& state=storage();std::lock_guard lock(state.mutex);
            const auto begin=cpu(),stamp=now();state.open();++state.seen;state.last_seen=stamp;
            if(state.failed)return;
            if(event>=state.next_sample.size())++state.errors;
            else if(stamp<state.next_sample[event])++state.limited;
            else{
                state.next_sample[event]=stamp+interval;
                try{
                    const auto values=build();const Record record{stamp,event,provider,current,values[0],values[1],values[2],values[3]};
                    if(state.have_previous[event] && std::equal(record.begin()+1,record.end(),state.previous[event].begin()+1))++state.coalesced;
                    else if(state.written+state.buffered>=State::capacity)++state.dropped;
                    else{
                        state.previous[event]=record;state.have_previous[event]=true;
                        if(state.buffered==state.buffer.size())state.flush(stamp);
                        if(!state.failed)state.buffer[state.buffered++]=record;
                    }
                }catch(...){++state.errors;}
            }
            if(stamp>=state.last_flush && stamp-state.last_flush>=UINT64_C(100000000))state.flush(stamp);
            const auto end=cpu();if(begin && end>=begin)state.cpu_ns+=end-begin;
        }catch(...){}
    }
};
#else
class AdmissionTrace {
public:
    static bool enabled() noexcept {return false;}
    template<class Build> static void capture(std::uint64_t,std::uint64_t,
            std::uint64_t,std::uint64_t,Build&&) noexcept {}
};
#endif
bool resident_recipe_eligible(const HostServices& provider,GuestAddress address) {
#ifdef X64ARM_HOST_SERVICES_RESIDENT_CONTEXT_SCALAR
    if(provider.resident_context_scalar_service(address))return true;
#endif
#ifdef X64ARM_HOST_SERVICES_SCALAR_RECIPE
    return provider.resident_service(address) && provider.resident_scalar_service(address) &&
        provider.resident_scalar_recipe(address)!=0;
#else
    (void)provider;(void)address;return false;
#endif
}

// 只讀診斷視圖使用原本的優先序、游標與 affinity 選擇；不寫 processor_id、
// 不解析等待、不執行服務。排除數是本次快照，不把缺失時段當作無可跑工作。
template<class Threads,class Pending>
void trace_provider_admission(const WindowsServices& provider,const Threads& threads,const Pending& pending,
        unsigned processors,std::uint32_t process_priority,bool background,std::uint64_t cursor,bool stopped,
        std::uint64_t event) noexcept {
    if(!AdmissionTrace::enabled())return;
    AdmissionTrace::capture(UINT64_C(5000000),event,
        reinterpret_cast<std::uintptr_t>(static_cast<const HostServices*>(&provider)),threads.current,[&]{
        std::array<std::uint64_t,8> excluded{};std::uint64_t raw=0,selected=0,flags=0;
        const auto pack=[&](unsigned begin){std::uint64_t value=0;for(unsigned n=0;n<4;++n){
            if(excluded[begin+n]>0xffffU)flags|=UINT64_C(0x8000);
            value|=std::min<std::uint64_t>(excluded[begin+n],0xffffU)<<(16*n);
        }return value;};
        const auto finish=[&]{
            const auto first=pack(0);const auto second=pack(4);
            return std::array<std::uint64_t,4>{raw,selected,first,(second&UINT64_C(0x0000ffffffffffff))|(flags<<48)};
        };
        for(const auto& [id,thread]:threads.items){(void)id;
            if(thread.status!=ThreadStatus::Runnable)++excluded[0];
            else if(thread.suspended)++excluded[1];else ++raw;
        }
        if(threads.items.empty()){flags|=1;return finish();}
        if(threads.yield){flags|=2;return finish();}
        if(stopped){flags|=4;return finish();}
        const auto found=threads.items.find(threads.current);
        if(found==threads.items.end() || found->second.status!=ThreadStatus::Runnable || found->second.suspended){flags|=8;return finish();}
        const auto& current=found->second;
        auto available=processors==64?UINT64_MAX:(UINT64_C(1)<<processors)-1;
        const auto allowed=available&current.affinity;if(!allowed){flags|=16;return finish();}
        const auto processor=(allowed&(UINT64_C(1)<<current.ideal_processor))?current.ideal_processor:static_cast<std::uint32_t>(std::countr_zero(allowed));
        available&=~(UINT64_C(1)<<processor);selected=1;
        struct Candidate {int priority;bool wrapped;std::uint64_t id;const GuestThread* thread;};
        std::vector<Candidate> candidates;
        for(const auto& [id,thread]:threads.items)
            if(id!=threads.current && thread.status==ThreadStatus::Runnable && !thread.suspended)
                candidates.push_back({thread.effective_priority(process_priority,background),id<=cursor,id,&thread});
        std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b){
            if(a.priority!=b.priority)return a.priority>b.priority;if(a.wrapped!=b.wrapped)return !a.wrapped;return a.id<b.id;
        });
        for(std::size_t n=0;n<candidates.size();++n){
            if(!available){excluded[6]+=candidates.size()-n;break;}
            const auto& candidate=candidates[n];const auto& thread=*candidate.thread;
            const auto request=pending.find(candidate.id);
            if(request!=pending.end() && request->second==thread.context.rip){++excluded[2];continue;}
            if(thread.context.pending_cpu_trap){++excluded[3];continue;}
            if(provider.handles(thread.context.rip)
#ifdef X64ARM_HOST_SERVICES_PARALLEL_SERVICE
                && !provider.parallel_service(thread.context.rip) && !resident_recipe_eligible(provider,thread.context.rip)
#ifdef X64ARM_HOST_SERVICES_COORDINATED_SERVICE
                && detail::resident_mapping_service(provider.name(thread.context.rip))==detail::ResidentMappingService::None
#endif
#endif
                ){++excluded[4];continue;}
            const auto eligible=available&thread.affinity;if(!eligible){++excluded[5];continue;}
            const auto cpu=(eligible&(UINT64_C(1)<<thread.ideal_processor))?thread.ideal_processor:static_cast<std::uint32_t>(std::countr_zero(eligible));
            available&=~(UINT64_C(1)<<cpu);++selected;
        }
        return finish();
    });
}
#if !defined(_WIN32)
// 診斷：X64ARM_TRACE_THREAD_CYCLES 啟用時，統計排程器閒置區間（沒有可執行客體執行緒）及其結束原因。
struct IdleTrace {
    std::uint64_t periods{}, nanoseconds{}, signalled{}, signalled_ns{}, timer{}, timer_ns{}, longest_ns{};
    std::uint64_t buckets[6]{}; // <100µs、<500µs、<1ms、<5ms、<20ms、>=20ms 的次數
};
IdleTrace idle_trace;
// 診斷：X64ARM_TRACE_IDLE_TIMELINE=<目錄> 時，逐段記錄閒置起訖（Unix 微秒），供跨程序幀節拍對照。
std::FILE* idle_timeline_file() {
    static std::FILE* const file = [] () -> std::FILE* {
        const auto* directory = std::getenv("X64ARM_TRACE_IDLE_TIMELINE");
        if (!directory || !*directory)
            return nullptr;
        if(std::string_view(directory)=="stderr")return stderr; // 可由觀測器的記憶體 pipe 接收，避免磁碟停頓。
        auto* opened = std::fopen((std::string(directory) + "/idle-" + std::to_string(::getpid()) + ".txt").c_str(), "a");
        if (opened)
            std::setvbuf(opened, nullptr, _IOLBF, 0); // 程序可能被強制結束，逐行寫出。
        return opened;
    }();
    return file;
}
// 同一時間線另記單一客體執行緒連續占用排程器 10 ms 以上的區段（換人或閒置時結算）。
struct RunTrace {std::uint64_t thread{},boundaries{},wake_zero{},wake_set{},last_wake{},interrupts{},arms{};std::chrono::steady_clock::time_point start{};};
RunTrace run_trace;
long long idle_timeline_unix_us(std::chrono::steady_clock::time_point point);
template<class Threads> void trace_run_switch(const Threads& threads, std::uint64_t next) {
    auto* timeline = idle_timeline_file();
    if (!timeline || run_trace.thread == next)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (run_trace.thread && now - run_trace.start >= std::chrono::milliseconds(10)) {
        const auto found = threads.find(run_trace.thread);
        const auto name = found == threads.end() ? std::string{} : utf16_to_utf8(found->second.description);
        // 區段內經過的服務／語義邊界次數，及結算時其他可執行的執行緒（分辨無法搶占與主執行緒在等待）。
        std::string runnable;
        for (const auto& [id, thread] : threads)
            if (id != run_trace.thread && thread.status == ThreadStatus::Runnable && !thread.suspended)
                runnable += (runnable.empty() ? "" : ",") + std::to_string(id);
        const auto prefix=timeline==stderr?"timeline pid="+std::to_string(::getpid())+" ":std::string{};
        std::fprintf(timeline, "%srun %llu %lld %lld 0x%llx boundaries=%llu/%llu/%llu runnable=%s wake=%llu/%llu/%lluus priority=%d multimedia=%d %s\n", prefix.c_str(),static_cast<unsigned long long>(run_trace.thread),
                     idle_timeline_unix_us(run_trace.start), idle_timeline_unix_us(now),
                     static_cast<unsigned long long>(found == threads.end() ? 0 : found->second.entry),
                     static_cast<unsigned long long>(run_trace.boundaries), static_cast<unsigned long long>(run_trace.interrupts),
                     static_cast<unsigned long long>(run_trace.arms), runnable.empty() ? "-" : runnable.c_str(),
                     static_cast<unsigned long long>(run_trace.wake_zero), static_cast<unsigned long long>(run_trace.wake_set),
                     static_cast<unsigned long long>(run_trace.last_wake / 1000),
                     found==threads.end()?0:found->second.priority,
                     found==threads.end() || !found->second.multimedia.handle?0:
                         std::clamp(found->second.multimedia.base+found->second.multimedia.relative,
                                    found->second.multimedia.minimum,found->second.multimedia.maximum),name.c_str());
    }
    run_trace = {next, 0, 0, 0, 0, 0, 0, now};
}
long long idle_timeline_unix_us(std::chrono::steady_clock::time_point point) {
    const auto offset = std::chrono::system_clock::now().time_since_epoch() - std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(point.time_since_epoch() + offset).count());
}
#endif
// 沿用明確啟用的排程時間線；一般執行不讀時鐘、不記錄輪詢細節。
class WakePollScope {
    startup_profile::Scope profile_;
#if !defined(_WIN32)
    std::FILE* file_{};
    const char* name_{};
    std::chrono::steady_clock::time_point start_{};
#endif
public:
    explicit WakePollScope(const char* name):profile_("wake_poll",name) {
#if !defined(_WIN32)
        file_=idle_timeline_file();name_=name;
        if(file_)start_=std::chrono::steady_clock::now();
#endif
    }
    ~WakePollScope() {
#if !defined(_WIN32)
        if(!file_)return;
        const auto end=std::chrono::steady_clock::now();
        if(end-start_>=std::chrono::milliseconds(5)){
            const auto prefix=file_==stderr?"timeline pid="+std::to_string(::getpid())+" ":std::string{};
            std::fprintf(file_,"%spoll %s %lld %lld\n",prefix.c_str(),name_,idle_timeline_unix_us(start_),idle_timeline_unix_us(end));
        }
#endif
    }
};
std::uint64_t external_wait_spin_ticks() {
    static const auto ticks = [] {
        const auto *raw = std::getenv("X64ARM_WAIT_SPIN_US");
        if (!raw)
            return UINT64_C(0);
        const auto *end = raw + std::strlen(raw);
        unsigned value = 0;
        const auto result = std::from_chars(raw, end, value);
        return result.ec == std::errc{} && result.ptr == end && value <= 1000 ? std::uint64_t{value} * 10 : UINT64_C(0);
    }();
    return ticks;
}
std::uint64_t external_wait_poll_limit() {
    static const auto ticks = [] {
        const auto *raw = std::getenv("X64ARM_WAIT_POLL_US");
        if (!raw)
            return UINT64_C(10000);
        const auto *end = raw + std::strlen(raw);
        unsigned value = 0;
        const auto result = std::from_chars(raw, end, value);
        return result.ec == std::errc{} && result.ptr == end && value >= 50 && value <= 1000 ? std::uint64_t{value} * 10
                                                                                             : UINT64_C(10000);
    }();
    return ticks;
}
// 有界排程量測開關；時間片仍乘上客體優先序，API yield 與到期等待照常處理。
unsigned scheduler_quantum() noexcept {
    static const unsigned value = [] {
        const char *raw = std::getenv("X64ARM_SCHEDULER_QUANTUM");
        if (!raw)
            return 4096U;
        unsigned parsed = 0;
        const auto end = raw + std::strlen(raw);
        const auto result = std::from_chars(raw, end, parsed);
        return result.ec == std::errc{} && result.ptr == end && parsed >= 512 && parsed <= 262144 &&
                       std::has_single_bit(parsed)
                   ? parsed
                   : 4096U;
    }();
    return value;
}
bool adaptive_scheduler_budget() noexcept {
    static const bool enabled=std::getenv("X64ARM_SCHEDULER_QUANTUM")==nullptr;
    return enabled;
}
bool clock_scheduled_slices() noexcept {
    // 觀察批次只限制回到協調者的間隔；宿主時鐘模式由時間片及明確 yield 排程。
    // 顯式指令 quantum 保留原來的精確配額，開關用於同成品實際工作負載對照。
    static const bool enabled=[] {
        const auto* setting=std::getenv("X64ARM_SCHEDULER_CLOCK");
        return !setting || std::strcmp(setting,"0")!=0;
    }();
    return enabled && adaptive_scheduler_budget();
}
} // namespace
std::uint32_t WindowsServices::thread_context_bytes(std::uint64_t id, std::vector<std::uint8_t> &bytes, bool setting,
                                                    bool remote) {
    const auto size = x86_ ? 716U : 1232U;
    if (bytes.size() != size)
        return 87;
    const auto found = threads_.items.find(id);
    if (found == threads_.items.end())
        return 6;
    auto &thread = found->second;
    if (thread.status == ThreadStatus::Exited || !thread.suspended || (!remote && id == threads_.current))
        return 87;
    const auto offset = x86_ ? 0U : 48U;
    std::uint32_t flags{};
    for (unsigned i = 0; i < 4; ++i)
        flags |= std::uint32_t{bytes[offset + i]} << (8 * i);
    const auto architecture = x86_ ? 0x10000U : 0x100000U;
    if (!(flags & architecture))
        return 87;
    if (flags & ~(architecture | (x86_ ? 0x2fU : 0xfU)))
        return 50; // Debug／XSTATE 尚無 CPU 狀態後端。
    try {
        GuestMemory input;
        input.set_execution_mode(x86_ ? ExecutionMode::X86 : ExecutionMode::X64);
        input.map(GuestAddress{0x10000}, size, read_write, bytes);
        if (setting) {
            thread.context = read_context(input, 0x10000, thread.context);
            return 0;
        }
        input.map(GuestAddress{0x20000}, size, read_write);
        write_context(input, 0x20000, thread.context);
        const auto copy = [&](unsigned begin, unsigned length) {
            std::vector<std::uint8_t> part(length);
            input.read(GuestAddress{0x20000 + begin}, part);
            std::copy(part.begin(), part.end(), bytes.begin() + begin);
        };
        if (x86_) {
            if (flags & 1U)
                copy(180, 24);
            if (flags & 2U)
                copy(156, 24);
            if (flags & 4U)
                copy(140, 16);
            if (flags & 8U)
                copy(28, 112);
            if (flags & 0x20U)
                copy(204, 512);
        } else {
            if (flags & 1U) {
                copy(56, 2);
                copy(66, 6);
                copy(152, 8);
                copy(248, 8);
            }
            if (flags & 2U) {
                copy(120, 32);
                copy(160, 88);
            }
            if (flags & 4U) {
                constexpr std::array<unsigned, 4> segments{3, 0, 4, 5};
                for (unsigned i = 0; i < segments.size(); ++i) {
                    const auto at = 58 + i * 2;
                    const auto value = thread.context.segments[segments[i]];
                    bytes[at] = static_cast<std::uint8_t>(value);
                    bytes[at + 1] = static_cast<std::uint8_t>(value >> 8);
                }
            }
            if (flags & 8U) {
                copy(52, 4);
                copy(256, 512);
            }
        }
        return 0;
    } catch (const MemoryFault &) {
        return 998;
    } catch (const HostCallFault &) {
        return 50;
    } catch (const std::bad_alloc &) {
        return 8;
    }
}
std::span<const WindowsServiceDescriptor> thread_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
                                                      {"avrt.dll", "AvSetMmThreadCharacteristicsA", 2},
                                                      {"avrt.dll", "AvSetMmThreadCharacteristicsW", 2},
                                                      {"avrt.dll", "AvSetMmThreadPriority", 2},
                                                      {"avrt.dll", "AvRevertMmThreadCharacteristics", 1},
                                                      {"kernel32.dll", "CreateIoCompletionPort", 4},
                                                      {"kernel32.dll", "PostQueuedCompletionStatus", 4},
                                                      {"kernel32.dll", "GetQueuedCompletionStatus", 5},
                                                      {"kernel32.dll", "GetQueuedCompletionStatusEx", 6},
                                                      {"kernel32.dll", "SetFileCompletionNotificationModes", 2},
                                                      {"kernel32.dll", "Module32First", 2},
                                                      {"kernel32.dll", "Module32Next", 2},
                                                      {"kernel32.dll", "Module32FirstW", 2},
                                                      {"kernel32.dll", "Module32NextW", 2},
                                                      {"kernel32.dll", "CreateToolhelp32Snapshot", 2},
                                                      {"kernel32.dll", "Process32FirstW", 2},
                                                      {"kernel32.dll", "Process32NextW", 2},
                                                      {"kernel32.dll", "Process32First", 2},
                                                      {"kernel32.dll", "Process32Next", 2},
                                                      {"kernel32.dll", "Thread32First", 2},
                                                      {"kernel32.dll", "Thread32Next", 2},
                                                      {"kernel32.dll", "QueryThreadCycleTime", 2},
                                                      {"kernel32.dll", "OpenThread", 3},
                                                      {"kernel32.dll", "CreateThread", 6},
                                                      {"kernel32.dll", "CreateRemoteThread", 7},
                                                      {"kernel32.dll", "CreateRemoteThreadEx", 8},
                                                      {"kernel32.dll", "TerminateThread", 2},
                                                      {"kernel32.dll", "ExitThread", 1},
                                                      {"kernel32.dll", "GetCurrentThreadId", 0},
                                                      {"kernel32.dll", "GetCurrentThreadStackLimits", 2},
                                                      {"kernel32.dll", "SetThreadStackGuarantee", 1},
                                                      {"kernel32.dll", "SetThreadAffinityMask", 2},
                                                      {"kernel32.dll", "SetThreadIdealProcessor", 2},
                                                      {"kernel32.dll", "GetExitCodeThread", 2},
                                                      {"kernel32.dll", "GetThreadId", 1},
                                                      {"kernel32.dll", "GetProcessIdOfThread", 1},
                                                      {"kernel32.dll", "FreeLibraryAndExitThread", 2},
                                                      {"kernel32.dll", "SetThreadDescription", 2},
                                                      {"kernel32.dll", "GetThreadDescription", 2},
                                                      {"kernel32.dll", "SetThreadInformation", 4},
                                                      {"kernel32.dll", "GetThreadInformation", 4},
                                                      {"kernel32.dll", "SuspendThread", 1},
                                                      {"kernel32.dll", "ResumeThread", 1},
                                                      {"kernel32.dll", "GetThreadContext", 2},
                                                      {"kernel32.dll", "SetThreadContext", 2},
                                                      {"kernel32.dll", "Wow64GetThreadContext", 2},
                                                      {"kernel32.dll", "Wow64SetThreadContext", 2},
                                                      {"kernel32.dll", "Wow64SuspendThread", 1},
                                                      {"kernel32.dll", "SwitchToThread", 0},
                                                      {"kernel32.dll", "Sleep", 1},
                                                      {"kernel32.dll", "Beep", 2},
                                                      {"kernel32.dll", "SleepEx", 2},
                                                      {"kernel32.dll", "QueueUserAPC", 3},
                                                      {"kernel32.dll", "SignalObjectAndWait", 4},
                                                      {"kernel32.dll", "WaitForSingleObject", 2},
                                                      {"kernel32.dll", "WaitForMultipleObjects", 4},
                                                      {"kernel32.dll", "WaitForSingleObjectEx", 3},
                                                      {"kernel32.dll", "WaitForMultipleObjectsEx", 5},
                                                      {"kernel32.dll", "SetWaitableTimer", 6},
                                                      {"kernel32.dll", "SetWaitableTimerEx", 7},
                                                      {"kernel32.dll", "CancelWaitableTimer", 1},
                                                      {"kernel32.dll", "InitializeCriticalSection", 1},
                                                      {"kernel32.dll", "InitializeCriticalSectionAndSpinCount", 2},
                                                      {"kernel32.dll", "InitializeCriticalSectionEx", 3},
                                                      {"crt", "__vcrt_InitializeCriticalSectionEx", 3},
                                                      {"kernel32.dll", "SetCriticalSectionSpinCount", 2},
                                                      {"kernel32.dll", "EnterCriticalSection", 1},
                                                      {"kernel32.dll", "TryEnterCriticalSection", 1},
                                                      {"kernel32.dll", "LeaveCriticalSection", 1},
                                                      {"kernel32.dll", "DeleteCriticalSection", 1},
                                                      {"kernel32.dll", "InitializeSRWLock", 1},
                                                      {"kernel32.dll", "AcquireSRWLockExclusive", 1},
                                                      {"kernel32.dll", "AcquireSRWLockShared", 1},
                                                      {"kernel32.dll", "TryAcquireSRWLockExclusive", 1},
                                                      {"kernel32.dll", "TryAcquireSRWLockShared", 1},
                                                      {"kernel32.dll", "ReleaseSRWLockExclusive", 1},
                                                      {"kernel32.dll", "ReleaseSRWLockShared", 1},
                                                      {"kernel32.dll", "InitializeConditionVariable", 1},
                                                      {"kernel32.dll", "SleepConditionVariableSRW", 4},
                                                      {"kernel32.dll", "SleepConditionVariableCS", 3},
                                                      {"kernel32.dll", "WakeConditionVariable", 1},
                                                      {"kernel32.dll", "WakeAllConditionVariable", 1},
                                                      {"kernel32.dll", "InitOnceExecuteOnce", 4},
                                                      {"kernel32.dll", "InitOnceInitialize", 1},
                                                      {"kernel32.dll", "InitOnceBeginInitialize", 4},
                                                      {"kernel32.dll", "InitOnceComplete", 3},
                                                      {"crt", "_beginthreadex", 6},
                                                      {"crt", "_resetstkoflw", 0},
                                                      {"crt", "_endthreadex", 1}};
    return table;
}
std::uint64_t WindowsServices::open_local_thread(std::uint64_t id, std::uint32_t rights, bool inherit) {
    // OpenThread 與 NtGetNextThread 共用：依執行緒物件的 DACL 計算授與權限，失敗丟出 SecurityDescriptorError。
    std::vector<std::uint8_t> *local{};
    int native = -1, lock = -1;
    bool thread = false;
    const auto handle = runtime_.allocate_wait_handle();
    runtime_.process_thread_handles.emplace(handle, std::pair{id, false});
    struct Provisional {
        RuntimeState &runtime;
        std::uint64_t handle;
        ~Provisional() { runtime.process_thread_handles.erase(handle); }
    } provisional{runtime_, handle};
    const auto error = process_security_location(handle, 0, local, native, lock, thread);
    if (error)
        throw SecurityDescriptorError{error};
    SharedEventLock guard(lock);
    const auto granted =
        kernel_security_access(native >= 0 ? shared_object_security(native, true) : *local, rights, true);
    const auto result = runtime_.allocate_wait_handle();
    runtime_.process_thread_handles.emplace(result, std::pair{id, inherit});
    runtime_.thread_handle_access[result] = granted;
    runtime_.handle_flags[result] = inherit ? 1U : 0U;
    return result;
}
std::uint64_t WindowsServices::thread_id(std::uint64_t handle) const {
    if (handle == (x86_ ? UINT32_MAX - 1 : UINT64_MAX - 1))
        return threads_.current;
    const auto found = runtime_.process_thread_handles.find(handle);
    return found != runtime_.process_thread_handles.end() && threads_.items.contains(found->second.first)
               ? found->second.first
               : 0;
}
bool WindowsServices::acquire_slim_word(GuestMemory &memory, std::uint64_t address, std::uint64_t id, bool shared) {
    // 依字組編碼取得：bit0 獨占、bit1 等待者、bit2 起共用數。最後一位等待者離開時清除 bit1；
    // 取不到時設 bit1，讓客體快速路徑的釋放改進宿主，再由等待解析授予。
    const std::uint8_t width = x86_ ? 32 : 64;
    for (;;) {
        const auto word = memory.load(GuestAddress{address}, width);
        std::uint64_t next;
        if (!(word & 1) && (shared || !(word >> 2))) {
            next = shared ? word + 4 : word | 1;
            const bool others = std::any_of(threads_.items.begin(), threads_.items.end(), [&](const auto &entry) {
                const auto &thread = entry.second;
                const auto &wait = thread.wait;
                return entry.first != id && thread.status == ThreadStatus::Waiting &&
                       ((wait.kind == ThreadWaitKind::Slim && wait.handles[0] == address) ||
                        (wait.kind == ThreadWaitKind::Condition && !wait.critical && wait.handles.size() > 1 &&
                         wait.handles[1] == address && (wait.signaled || wait.deadline <= runtime_.clock_ticks)));
            });
            if (!others) next &= ~UINT64_C(2);
            if (memory.atomic_rmw(GuestAddress{address}, width, GuestMemory::AtomicRmw::CompareExchange, next, word) == word) return true;
            continue;
        }
        if (word & 2) return false;
        next = word | 2;
        if (memory.atomic_rmw(GuestAddress{address}, width, GuestMemory::AtomicRmw::CompareExchange, next, word) == word) return false;
    }
}
std::uint32_t WindowsServices::thread_identifier(std::uint64_t id)const{
    const auto found=threads_.items.find(id);if(found==threads_.items.end())return 0;
    return found->second.public_id?found->second.public_id:static_cast<std::uint32_t>(id);
}
void WindowsServices::initialize_stack_guard(GuestMemory& memory,std::uint64_t base,std::uint64_t size,ThreadStackGuard& guard){
    // 既有後端預先提交整個 stack；底部保護頁仍攔截耗盡，保證區保留實際可寫 backing。
    if(size<8192 || (base&4095U) || !memory.query(GuestAddress{base}))return;
    auto protections=runtime_.virtual_page_protections;
    protections.insert_or_assign(base,RuntimeState::PageProtection{0x104U,4U});
    memory.protect(GuestAddress{base},4096,0);
    runtime_.virtual_page_protections=std::move(protections);guard.armed=true;
}
std::uint32_t WindowsServices::configure_stack_guarantee(GuestState& state,GuestMemory& memory,std::uint32_t requested,bool reset,std::uint32_t& previous){
    auto& thread=threads_.items.at(threads_.current);
    auto* guard=&thread.stack_guard;auto base=thread.stack_base,size=thread.stack_size;
    if(thread.fiber){auto& fiber=threads_.fibers.at(thread.fiber);guard=&fiber.stack_guard;base=fiber.stack_base;size=fiber.stack_size;}
    previous=guard->guarantee;
    const auto rounded=(static_cast<std::uint64_t>(requested)+4095)&~UINT64_C(4095);
    if(rounded>UINT32_MAX || size<8192 || rounded>size-8192)return 87;
    const auto guarantee=std::max(rounded,static_cast<std::uint64_t>(guard->guarantee));
    const auto old_protection=runtime_.virtual_page_protections.find(base+guard->guarantee);
    if(reset && guard->armed && !guard->overflow && guarantee==guard->guarantee && old_protection!=runtime_.virtual_page_protections.end() && (old_protection->second.mode&0x100U))return 0;
    if(!reset && (!requested || guarantee==guard->guarantee))return 0;
    const auto page=base+guarantee;
    // 恢復前必須已展開到保護頁上方；不能把目前 API 框架改成不可讀寫。
    if((reset || !guard->overflow) && state.reg(Gpr::Rsp)<page+8192)return 8;
    auto protections=runtime_.virtual_page_protections;
    if(guard->armed)protections.erase(base+guard->guarantee);
    if(reset || !guard->overflow)protections.insert_or_assign(page,RuntimeState::PageProtection{0x104U,4U});
    std::vector<GuestMemory::Mapping> ranges;
    if(guard->armed && base+guard->guarantee!=page)ranges.push_back({GuestAddress{base+guard->guarantee},4096,read_write});
    if(reset || !guard->overflow)ranges.push_back({GuestAddress{page},4096,0});
    memory.protect_ranges(ranges);runtime_.virtual_page_protections=std::move(protections);
    guard->guarantee=static_cast<std::uint32_t>(guarantee);
    if(reset){guard->overflow=false;guard->armed=true;}else guard->armed=!guard->overflow;
    memory.store(GuestAddress{thread.teb+(x86_?8U:16U)},x86_?32:64,guard->limit(base));
    memory.store(GuestAddress{thread.teb+(x86_?0xf78U:0x1748U)},32,guard->guarantee);
    return 0;
}
bool WindowsServices::release_stack_guard(GuestState& state,GuestMemory& memory,std::uint64_t address){
    if(threads_.items.empty())return false;
    auto& thread=threads_.items.at(threads_.current);
    auto* guard=&thread.stack_guard;auto base=thread.stack_base;
    if(thread.fiber){auto& fiber=threads_.fibers.at(thread.fiber);guard=&fiber.stack_guard;base=fiber.stack_base;}
    const auto page=base+guard->guarantee;
    const auto protection=runtime_.virtual_page_protections.find(page);
    if(!guard->armed || address<page || address-page>=4096 || protection==runtime_.virtual_page_protections.end() || !(protection->second.mode&0x100U))return false;
    memory.protect(GuestAddress{page},4096,read_write);runtime_.virtual_page_protections.erase(page);
    guard->armed=false;guard->overflow=true;
    memory.store(GuestAddress{thread.teb+(x86_?8U:16U)},x86_?32:64,base);
    (void)state;return true;
}
std::uint64_t WindowsServices::thread_from_identifier(std::uint32_t identifier)const{
    if(!identifier)return 0;
    for(const auto& [id,thread]:threads_.items){(void)thread;if(thread_identifier(id)==identifier)return id;}
    return 0;
}
void WindowsServices::cancel_waitable_timer_apcs(std::uint64_t identity){
    if(!identity)return;for(auto& [id,thread]:threads_.items){(void)id;std::erase_if(thread.apcs,[&](const auto& callback){return callback.waitable_timer==identity;});}
}
std::uint64_t WindowsServices::coalesce_waitable_timer(const WaitObject& timer)const{
    auto result=timer.timer_nominal_due;const auto delay=static_cast<std::uint64_t>(timer.timer_tolerance)*10000;
    const auto last=delay>UINT64_MAX-result?UINT64_MAX:result+delay;
    for(const auto& [id,other]:runtime_.wait_states){(void)id;if(&other==&timer || other.kind!=WaitObject::Kind::Timer || !other.active || other.due<timer.timer_nominal_due || other.due>last)continue;if(result==timer.timer_nominal_due || other.due<result)result=other.due;}
    return result;
}
ExecutionPolicy WindowsServices::execution_policy(const GuestState &state) const {
    // TEB 是 context 與 guest thread 的穩定身分；平行批次啟動前由協調執行緒查詢。
    if(runtime_.process_background || runtime_.process_priority_class==0x40)return ExecutionPolicy::Efficient;
    auto control=runtime_.process_power_control,state_mask=runtime_.process_power_state;
    const auto teb = x86_ ? state.fs_base : state.gs_base;
    // 重新綁定 worker 時頻繁查詢：先以 context 位址找到執行緒並核對 TEB，對不上才依 TEB 走訪。
    const GuestThread *owner = nullptr;
    {
        std::unique_lock<std::mutex> lock;
        const auto &index = resident_thread_index(lock);
        if (const auto found = index.contexts.find(&state); found != index.contexts.end())
            if (const auto thread = threads_.items.find(found->second); thread != threads_.items.end() && thread->second.teb == teb)
                owner = &thread->second;
    }
    // 協調執行緒以獨立的 GuestState 執行目前執行緒，索引裡只有各執行緒保存的 context：先核對目前執行緒。
    if (!owner)
        if (const auto current = threads_.items.find(threads_.current); current != threads_.items.end() && current->second.teb == teb)
            owner = &current->second;
    if (!owner)
        for (const auto &[id, thread] : threads_.items) {
            (void)id;
            if (thread.teb == teb) {
                owner = &thread;
                break;
            }
        }
    if (owner && (owner->power_control & 1U)) {
        control = owner->power_control;
        state_mask = owner->power_state;
    }
    if(control&1U)return state_mask&1U?ExecutionPolicy::Efficient:ExecutionPolicy::Responsive;
    // MMCSS 的即時工作也映射宿主 QoS；只改實際執行該客體的 worker，明確電源設定優先。
    if(owner && owner->multimedia.handle &&
       owner->effective_priority(runtime_.process_priority_class,runtime_.process_background)>=16)
        return ExecutionPolicy::Interactive;
    return runtime_.process_priority_class==0x4000?ExecutionPolicy::Utility:
        runtime_.process_priority_class==0x8000?ExecutionPolicy::Responsive:
        runtime_.process_priority_class==0x80?ExecutionPolicy::Interactive:ExecutionPolicy::Automatic;
}
void WindowsServices::set_processor_count(GuestMemory &memory, unsigned count) {
    if (!count || count > (x86_ ? 32U : 64U) || count > host_physical_processors())
        throw std::invalid_argument("平行客體核心數超過宿主實體核心或目前架構限制");
    runtime_.processor_count = count;
    runtime_.process_affinity = count == 64 ? UINT64_MAX : (UINT64_C(1) << count) - 1;
    for (auto &[id, thread] : threads_.items) {
        (void)id;
        thread.affinity = runtime_.process_affinity;
    }
    memory.set_processor_count(count);
    notify_concurrency_changed();
}
bool WindowsServices::set_process_affinity(GuestState &state, std::uint64_t mask) {
    const auto system = runtime_.processor_count == 64 ? UINT64_MAX : (UINT64_C(1) << runtime_.processor_count) - 1;
    if (!mask || (mask & ~system))
        return false;
    runtime_.process_affinity = mask;
    for (auto &[id, thread] : threads_.items) {
        thread.affinity = mask;
        auto &context = id == threads_.current ? state : thread.context;
        if (!(mask & (UINT64_C(1) << context.processor_id)))
            context.processor_id = static_cast<std::uint32_t>(std::countr_zero(mask));
    }
    notify_concurrency_changed();
    return true;
}
WindowsServices::ResidentThreadIndex& WindowsServices::resident_thread_index(std::unique_lock<std::mutex>& lock) const {
    auto& index = resident_threads_;
    lock = std::unique_lock(index.mutex);
    // 執行緒只在建立時加入表中、從不移除，std::map 節點位址在程序期間固定。
    if (index.stale || index.size != threads_.items.size()) {
        index.live.clear();
        index.contexts.clear();
        for (const auto &[id, thread] : threads_.items) {
            index.contexts.emplace(&thread.context, id);
            if (thread.status != ThreadStatus::Exited)
                index.live.emplace_back(id, &thread);
        }
        index.size = threads_.items.size();
        index.stale = false;
        static const bool trace = std::getenv("X64ARM_TRACE_THREAD_INDEX") != nullptr;
        if (trace)
            std::fprintf(stderr, "[thread_index] pid=%d threads=%zu live=%zu\n", ::getpid(), index.size, index.live.size());
    }
    return index;
}
std::vector<std::pair<std::uint64_t, GuestThread *>> WindowsServices::live_threads() {
    // 迴圈本體可能再查詢索引，只在鎖內複製快照；表中節點不移除，指標在程序期間有效。
    std::vector<std::pair<std::uint64_t, GuestThread *>> result;
    std::unique_lock<std::mutex> lock;
    const auto &index = resident_thread_index(lock);
    result.reserve(index.live.size());
    for (const auto &[id, thread] : index.live)
        result.emplace_back(id, const_cast<GuestThread *>(thread));
    return result;
}
bool WindowsServices::concurrent_guest_threads() const noexcept {
    if (debugger_stopped())
        return false;
    unsigned runnable = 0, exited = 0;
    std::unique_lock<std::mutex> lock;
    auto &index = resident_thread_index(lock);
    for (const auto &[id, thread] : index.live) {
        (void)id;
        if (thread->status == ThreadStatus::Exited)
            ++exited;
        else if (thread->status == ThreadStatus::Runnable && !thread->suspended && ++runnable >= 2)
            return true;
    }
    if (exited * 2 > index.live.size())
        index.stale = true;
    return false;
}
std::vector<GuestState *> WindowsServices::parallel_contexts(GuestState &state) {
    if(AdmissionTrace::enabled())trace_provider_admission(*this,threads_,resident_service_requests_,runtime_.processor_count,
        runtime_.process_priority_class,runtime_.process_background,parallel_cursor_,debugger_stopped(),1);
    parallel_thread_count_ = 0;
    if (threads_.items.empty() || threads_.yield || debugger_stopped())
        return {};
    const auto mask = runtime_.processor_count == 64 ? UINT64_MAX : (UINT64_C(1) << runtime_.processor_count) - 1;
    auto &current = threads_.items.at(threads_.current);
    if (current.status != ThreadStatus::Runnable || current.suspended)
        return {};
    auto available = mask;
    const auto allowed = available & current.affinity;
    if (!allowed)
        return {};
    state.processor_id = static_cast<std::uint32_t>(((allowed & (UINT64_C(1) << current.ideal_processor))
                                                         ? current.ideal_processor
                                                         : static_cast<std::uint32_t>(std::countr_zero(allowed))));
    available &= ~(UINT64_C(1) << state.processor_id);
    std::vector<GuestState *> result{&state};
    parallel_thread_ids_[parallel_thread_count_++] = threads_.current;
    // 等待協調者處理 API／例外的 context 不占 worker。依有效優先序分派，
    // 同優先序從游標之後輪替；MMCSS 喚醒後尚未完成的工作仍保有優先權。
    // 先篩出可執行的候選（遊戲常有數十條執行緒，同時 Runnable 的只有少數），每條只算一次有效優先序；
    // 排序鍵＝（優先序高者先，游標之後的 ID 先輪替），與原本「依游標旋轉後穩定排序」的順序相同。
    struct Candidate {std::int32_t priority;bool wrapped;std::uint64_t id;GuestThread* thread;};
    std::vector<Candidate> candidates;
    for (const auto& [id,entry] : live_threads())
        if (id != threads_.current && entry->status == ThreadStatus::Runnable && !entry->suspended)
            candidates.push_back({entry->effective_priority(runtime_.process_priority_class,runtime_.process_background),id<=parallel_cursor_,id,entry});
    std::sort(candidates.begin(),candidates.end(),[](const Candidate& left,const Candidate& right){
        if(left.priority!=right.priority)return left.priority>right.priority;
        if(left.wrapped!=right.wrapped)return !left.wrapped;
        return left.id<right.id;
    });
    for (const auto& candidate : candidates) {
        if(!available)break;
        const auto id=candidate.id;auto &thread = *candidate.thread;
        const auto pending_service=resident_service_requests_.find(id);
        if (id == threads_.current || thread.status != ThreadStatus::Runnable || thread.suspended ||
            (pending_service!=resident_service_requests_.end() && pending_service->second==thread.context.rip) ||
            thread.context.pending_cpu_trap || (handles(thread.context.rip)
#ifdef X64ARM_HOST_SERVICES_PARALLEL_SERVICE
            && !parallel_service(thread.context.rip)
            && !resident_recipe_eligible(*this,thread.context.rip)
#ifdef X64ARM_HOST_SERVICES_COORDINATED_SERVICE
            && detail::resident_mapping_service(name(thread.context.rip))==detail::ResidentMappingService::None
#endif
#endif
            ))
            continue;
        const auto eligible = available & thread.affinity;
        if (!eligible)
            continue;
        thread.context.processor_id =
            static_cast<std::uint32_t>(((eligible & (UINT64_C(1) << thread.ideal_processor))
                                            ? thread.ideal_processor
                                            : static_cast<std::uint32_t>(std::countr_zero(eligible))));
        available &= ~(UINT64_C(1) << thread.context.processor_id);
        result.push_back(&thread.context);
        parallel_thread_ids_[parallel_thread_count_++] = id;
        parallel_cursor_ = id;
    }
    return result;
}
void WindowsServices::after_parallel_instructions(GuestState &state, GuestMemory &memory,
                                                  std::span<const std::uint64_t> counts) {
    if (counts.size() != parallel_thread_count_)
        throw std::logic_error("原生批次計數與已選執行緒不一致");
    std::uint64_t others = 0;
    for (std::size_t n = 1; n < counts.size(); ++n) {
        others += counts[n];
        threads_.items.at(parallel_thread_ids_[n]).cycles += counts[n];
    }
    parallel_thread_count_ = 0;
    if (others)
        retired(memory, others);
    // 其他 worker 可以自旋到時間片到期；零進度的協調 context 仍須先完成慢路徑，
    // 否則跨頁原子操作／未接納指令會在每次集合後被搶占而永久飢餓。
    if (!counts.empty() && counts[0])
        after_instructions(state, memory, counts[0]);
}
#ifdef X64ARM_HOST_SERVICES_RESIDENT_PARALLEL
std::uint64_t WindowsServices::resident_context_key(const GuestState& state) const noexcept {
    std::unique_lock<std::mutex> lock;
    const auto& index=resident_thread_index(lock);
    const auto found=index.contexts.find(&state);
    return found!=index.contexts.end()?found->second:threads_.current;
}
#ifdef X64ARM_HOST_SERVICES_RESIDENT_SERVICE_REQUEST
void WindowsServices::resident_service_request(std::uint64_t key,GuestAddress rip) {
    if(threads_.items.contains(key))resident_service_requests_[key]=rip;
}
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_CONTROL_REQUEST
void WindowsServices::resident_control_request(std::uint64_t key,GuestAddress rip,bool pending) {
    if(pending){
        if(threads_.items.contains(key))resident_service_requests_[key]=rip;
    }else{
        const auto found=resident_service_requests_.find(key);
        if(found!=resident_service_requests_.end() && found->second==rip)resident_service_requests_.erase(found);
    }
}
#endif
#ifdef X64ARM_HOST_SERVICES_RESIDENT_CONTROL_DRAIN
bool WindowsServices::resident_select_control_request(GuestState& state,GuestMemory& memory) {
    if(!resident_execution_depth_ || parallel_workers()<2 || debugger_stopped())return false;
    for(const auto& [id,entry]:live_threads()){
        const auto& thread=*entry;
        if(id==threads_.current || thread.status!=ThreadStatus::Runnable || thread.suspended)continue;
        const auto pending=resident_service_requests_.find(id);
        if((pending==resident_service_requests_.end() || pending->second!=thread.context.rip) &&
           (!handles(thread.context.rip) || parallel_service(thread.context.rip) ||
            resident_recipe_eligible(*this,thread.context.rip)))continue;
        // schedule 已按輪替順序優先選取相同類別的控制待辦，並保存目前
        // GuestState、TLS／FLS、回呼及例外；不另設省略狀態交換的捷徑。
        const auto previous=threads_.current;
        // 這是客體已交出完整狀態的排程邊界，並非 API 內尚未提交的結果。
        // 原生 LastError／TLS 常式可能剛寫過 TEB；先接手最新值，避免
        // schedule 把協調者的舊 last_error_ 寫回客體。Win32／Win64 共用契約。
        if(runtime_.image.base)
            last_error_=static_cast<std::uint32_t>(memory.load(
                GuestAddress{x86_?state.fs_base+0x34:state.gs_base+0x68},32));
        schedule(state,memory);
        return threads_.current!=previous;
    }
    return false;
}
#endif
namespace {
// 時間片絕對期限可獨立比較；關閉時沿用相對 5 ms 期限。
// 跨程序通知與等待期限屬正確性契約，不受此時間片策略開關影響。
bool resident_preempt_enabled() noexcept {
    // 2026-10-24 起預設開啟：DQ3 遊戲內幀率穩定（不再掉到 8～9 fps）、Steam 啟動在變動範圍內；=0 恢復相對 5 ms 期限。
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_RESIDENT_PREEMPT");return !setting || std::string_view(setting)!="0";}();
    return enabled;
}
bool resident_external_wake_enabled() noexcept {
    // X64ARM_RESIDENT_EXTERNAL_WAKE=0 僅供診斷對照；預設保留跨程序事件中斷。
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_RESIDENT_EXTERNAL_WAKE");return !setting || std::string_view(setting)!="0";}();
    return enabled;
}
// 跨程序 IPC、管道與共用物件的通知都推進同一個 session 喚醒世代。
class SharedWakeSource final : public ResidentExternalWake {
    std::shared_ptr<SharedWake> wake_;
public:
    explicit SharedWakeSource(std::shared_ptr<SharedWake> wake):wake_(std::move(wake)){}
    bool wait_changed(std::uint64_t observed,std::uint64_t nanoseconds) const noexcept override {
        if(wake_->snapshot()!=observed)return true;
        if(!wake_->wait_for(observed,nanoseconds))std::this_thread::sleep_for(std::chrono::nanoseconds(nanoseconds));
        return wake_->snapshot()!=observed;
    }
    bool changed(std::uint64_t observed) const noexcept override {return wake_->snapshot()!=observed;}
};
}
std::shared_ptr<const ResidentExternalWake> WindowsServices::resident_external_wake(std::uint64_t& observed) const {
    // 只有一個客體執行緒時沒有可被外部事件喚醒的等待者。
#if !defined(_WIN32)
    if(idle_timeline_file())++run_trace.arms; // 核心查詢外部喚醒來源的次數，與絕對時間片計時器分開。
#endif
    if(!resident_external_wake_enabled() || !runtime_.host_clock || threads_.items.size()<2)return {};
    if(!resident_external_source_){
        auto wake=mapping_wake();if(!wake)return {};
        resident_external_source_=std::make_shared<SharedWakeSource>(std::move(wake));
    }
    observed=resident_external_polled_;
    return resident_external_source_;
}
#ifdef X64ARM_HOST_SERVICES_RESIDENT_IDLE_WAKE
// 協調者閒置等待時的外部喚醒：與搶占無關。renderer／browser／GPU 程序只靠 Mojo 管道與跨程序事件互通，
// 所有客體執行緒都在等待時，對方送來的資料只會推進 session 喚醒世代，不會產生信箱請求；只依期限等待時，
// Steam 熱啟動曾因此整個程序樹閒置 5～20 秒，直到某個客體計時器到期。X64ARM_RESIDENT_IDLE_WAKE=0 關閉。
std::shared_ptr<const ResidentExternalWake> WindowsServices::resident_idle_wake(std::uint64_t& observed) const {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_RESIDENT_IDLE_WAKE");return !setting || std::string_view(setting)!="0";}();
    if(!enabled || !runtime_.host_clock || threads_.items.size()<2)return {};
    if(!resident_external_source_){
        auto wake=mapping_wake();if(!wake)return {};
        resident_external_source_=std::make_shared<SharedWakeSource>(std::move(wake));
    }
    observed=resident_external_polled_;
    return resident_external_source_;
}
void WindowsServices::request_resident_poll() {threads_.yield=true;threads_.interrupt_poll=true;}
#endif
void WindowsServices::after_resident_parallel(GuestState& state,GuestMemory& memory) {
    parallel_thread_count_=0;
    // D3D11 裝置內容方法不喚醒其他執行緒、不釋放鎖，其後不跑完整收尾（其他服務照常每次收尾）：等待解析與排程由其他服務、常駐中斷計時器（時間片、等待期限、
    // 外部事件）承接。DQ3 實測：完全略過時遊戲內最快、讀檔時段音訊欠載最少；每 100 µs（讀時鐘的 ISB 約占
    // 主執行緒 4%）、每 16 次或依近似時鐘收尾，讀檔時段都出現較長的停格與音訊欠載。
    if(std::exchange(graphics_service_epilogue_,false))return;
    after_instructions(state,memory,0);
}
void WindowsServices::after_resident_interrupt(GuestState& state,GuestMemory& memory) {
    // 中斷只為其他 runnable、等待期限或外部事件而武裝：直接做完整排程（輪詢外部事件、解析逾時、
    // 輪替），不等時間片到期；否則期限已過的下一次武裝會立刻再中斷，直到時間片結束。
    threads_.yield=true;threads_.interrupt_poll=true;graphics_service_epilogue_=false;
#if !defined(_WIN32)
    if(idle_timeline_file())++run_trace.interrupts;
#endif
    after_resident_parallel(state,memory);
}
std::uint64_t WindowsServices::resident_parallel_wakeup_ns(std::span<GuestState* const> contexts) const {
    std::size_t runnable=0,exited=0;std::uint64_t deadline=UINT64_MAX;bool socket_select=false;
    {
        std::unique_lock<std::mutex> lock;
        auto& index=resident_thread_index(lock);
        for(const auto& [id,thread]:index.live){
            (void)id;
            if(thread->status==ThreadStatus::Runnable && !thread->suspended)++runnable;
            else if(thread->status==ThreadStatus::Waiting){
                deadline=std::min(deadline,thread->wait.deadline);
                socket_select=socket_select || thread->wait.kind==ThreadWaitKind::SocketSelect;
            }
            else if(thread->status==ThreadStatus::Exited)++exited;
        }
        if(exited*2>index.live.size())index.stale=true;
    }
    // 全部客體執行緒都在等待、但仍有網路就緒狀態要輪詢（WSAEventSelect／WSAAsyncSelect 監看、重疊 socket 收發、
    // accept、名稱解析、select）時，比照一般排程迴圈的外部等待，以輪詢上限（預設 1 ms）喚醒協調者。
    // 否則資料抵達後沒有任何事件喚醒，要等到下一個客體計時器才被發現：Steam 熱啟動的 webhelper 網路程序
    // 等 Steam.exe 的 loopback websocket 回覆，曾因此整個程序樹閒置 3～10 秒。只在完全閒置時縮短，
    // 有客體運算時不增加協調者中斷。
    // X64ARM_RESIDENT_NETWORK_POLL=0 關閉，供同引擎對照。
    static const bool network_poll=[] {const auto* value=std::getenv("X64ARM_RESIDENT_NETWORK_POLL");return !value || std::string_view(value)!="0";}();
    const auto& network=runtime_.network;
    const bool network_wait=network_poll && !runnable && (socket_select || !gui_.socket_watches.empty() || !network.pending_socket_io.empty() ||
        !network.pending_accepts.empty() || !network.resolvers.empty() || !network.pending_lookups.empty());
    // 尚未取得 worker 的 runnable 必須獲得排程；全部正在運算時不啟用時間片。單一常駐框架依目前
    // 執行緒時間片的絕對期限（排程時設定），不可每次進入本體都重新起算：頻繁冷出口會讓相對期限
    // 不斷順延而永不觸發。多 worker 仍以 5 ms 讓尚未取得 worker 的 runnable 獲得排程。
    std::uint64_t wake=0;
    if(runnable>contexts.size()){
        if(contexts.size()==1 && threads_.slice_deadline!=UINT64_MAX && resident_preempt_enabled()){
            const auto now=detail::monotonic_ticks();
            wake=threads_.slice_deadline>now?std::min<std::uint64_t>(threads_.slice_deadline-now,UINT64_MAX/100)*100:1;
        }else wake=5000000;
    }
    resident_wait_deadline_=deadline;
    if(deadline!=UINT64_MAX){
        const auto now=detail::monotonic_ticks();
        const auto remaining=deadline>now?std::min<std::uint64_t>(deadline-now,UINT64_MAX/100)*100:1;
        wake=wake?std::min(wake,remaining):remaining;
    }
    // 音訊已有實際播放進度與下一個事件期限；沿同一獨立中斷計時器喚醒，
    // 不等待其他客體計時器，也不在正常產碼加入配額、時鐘讀取或裝置輪詢。
    if(const auto audio=wasapi_wakeup_ns())wake=wake?std::min(wake,audio):audio;
    if(network_wait){
        const auto poll=external_wait_poll_limit()*100;
        wake=wake?std::min(wake,poll):poll;
    }
    // 診斷：X64ARM_TRACE_PARALLEL_WAKE=1 統計短期限（<1 ms）由哪個來源決定，每 2 秒輸出一次。
    static const bool trace_wake=std::getenv("X64ARM_TRACE_PARALLEL_WAKE")!=nullptr;
    if(trace_wake){
        static std::atomic<std::uint64_t> calls{},slice{},waits{},overdue{},audio_short{},network_short{},last_report{};
        ++calls;
        if(wake && wake<1000000){
            if(deadline!=UINT64_MAX && detail::monotonic_ticks()>=deadline)++overdue;
            else if(deadline!=UINT64_MAX && wake==std::min<std::uint64_t>(wake,(deadline-detail::monotonic_ticks())*100+1))++waits;
            else if(network_wait)++network_short;
            else if(runnable>contexts.size())++slice;
            else ++audio_short;
        }
        const auto now=detail::monotonic_ticks();auto previous=last_report.load();
        if(now-previous>=20000000 && last_report.compare_exchange_strong(previous,now))
            std::fprintf(stderr,"[parallel_wake] calls=%llu short_overdue=%llu short_wait=%llu short_slice=%llu short_audio=%llu short_network=%llu runnable=%zu contexts=%zu\n",
                static_cast<unsigned long long>(calls.load()),static_cast<unsigned long long>(overdue.load()),static_cast<unsigned long long>(waits.load()),
                static_cast<unsigned long long>(slice.load()),static_cast<unsigned long long>(audio_short.load()),static_cast<unsigned long long>(network_short.load()),runnable,contexts.size());
    }
#if !defined(_WIN32)
    if (idle_timeline_file()) {
        ++(wake ? run_trace.wake_set : run_trace.wake_zero);
        run_trace.last_wake = wake;
    }
    // 診斷：X64ARM_TRACE_IDLE_DUMP=<目錄> 時，常駐平行模式沒有 runnable 且下一次喚醒超過 20 ms（或無期限）
    // 就傾印各客體執行緒的等待（每程序最多每 3 秒一次）。協調者此時直接休眠，不經排程迴圈的閒置傾印。
    // 有 socket 監看的程序喚醒期限會被網路輪詢夾短，永遠達不到 20 ms；X64ARM_TRACE_IDLE_DUMP_MIN_WAKE_US 可改門檻（0 表示任何閒置）。
    static const char* const idle_dump_directory=std::getenv("X64ARM_TRACE_IDLE_DUMP");
    static const std::uint64_t idle_dump_min_wake=[] {
        const auto* value=std::getenv("X64ARM_TRACE_IDLE_DUMP_MIN_WAKE_US");
        return value && *value?std::strtoull(value,nullptr,10)*1000:UINT64_C(20000000);
    }();
    if(idle_dump_directory && !runnable && (!wake || wake>=idle_dump_min_wake)){
        static std::atomic<long long> last_dump{};
        const auto now_ms=static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        auto previous=last_dump.load();
        if(now_ms-previous>=3000 && last_dump.compare_exchange_strong(previous,now_ms)){
            const auto path=std::string(idle_dump_directory)+"/resident-idle-"+std::to_string(::getpid())+".log";
            if(auto* out=std::fopen(path.c_str(),"a")){
                const auto ticks=detail::monotonic_ticks();
                std::fprintf(out,"resident_idle unix_ms=%lld wake_ns=%llu threads=%zu\n",now_ms,static_cast<unsigned long long>(wake),threads_.items.size());
                for(const auto& [id,thread]:threads_.items){
                    if(thread.status==ThreadStatus::Exited)continue;
                    std::fprintf(out,"  id=%llu status=%d suspended=%d kind=%d remaining_ms=%lld name=%s rip=0x%llx api=%s\n",
                        static_cast<unsigned long long>(id),static_cast<int>(thread.status),thread.suspended?1:0,static_cast<int>(thread.wait.kind),
                        thread.wait.deadline==UINT64_MAX?-1LL:static_cast<long long>((thread.wait.deadline>ticks?thread.wait.deadline-ticks:0)/10000),
                        utf16_to_utf8(thread.description).c_str(),static_cast<unsigned long long>(thread.context.rip.value),
                        handles(thread.context.rip)?std::string(name(thread.context.rip)).c_str():"");
                    for(const auto handle:thread.wait.handles){
                        const auto object=runtime_.wait_objects.find(handle);
                        if(object==runtime_.wait_objects.end()){std::fprintf(out,"    handle=0x%llx (非等待物件)\n",static_cast<unsigned long long>(handle));continue;}
                        const auto& state_object=runtime_.wait_state(object->second);
                        std::fprintf(out,"    handle=0x%llx kind=%d signaled=%d manual=%d count=%u shared_fd=%d name=%s\n",
                            static_cast<unsigned long long>(handle),static_cast<int>(object->second.kind),state_object.signaled()?1:0,
                            state_object.manual_reset?1:0,state_object.count,runtime_.wait_lock_descriptor(object->second),
                            utf16_to_utf8(object->second.name).c_str());
                    }
                }
                std::fclose(out);
            }
        }
    }
#endif
    return wake;
}
#endif
std::uint64_t WindowsServices::native_chain_limit() const noexcept {
    if(resident_execution_depth_)return UINT64_MAX;
    if(threads_.yield)return 0;
    if(runtime_.host_clock && adaptive_scheduler_budget())return threads_.host_instruction_budget.limit;
    return 8U * (runtime_.host_clock ? scheduler_quantum() : 512U);
}
#ifdef X64ARM_HOST_SERVICES_NATIVE_TIME_SLICE
std::uint64_t WindowsServices::native_chain_time_slice_ns() const noexcept {
    if(resident_execution_depth_)return 0;
    if(threads_.yield || !runtime_.host_clock || !clock_scheduled_slices())return 0;
    // 100 us 在原有 50..250 us 觀察間隔內；仍於每次 API／故障返回立即更新服務。
    // 有排程中的執行緒時，不把續行時限延到該執行緒的時間片之外。
    if(threads_.items.empty())return 100000;
    const auto now=detail::monotonic_ticks();
    return now<threads_.slice_deadline?std::min<std::uint64_t>(1000,threads_.slice_deadline-now)*100:0;
}
#endif
bool WindowsServices::allows_multiblock() const noexcept { return !threads_.yield; }
std::uint64_t WindowsServices::instruction_slice() const noexcept {
    if(resident_execution_depth_)return UINT64_MAX;
    return threads_.items.empty() || (runtime_.host_clock && clock_scheduled_slices()) ? UINT64_MAX : threads_.remaining;
}
void WindowsServices::after_instructions(GuestState &state, GuestMemory &memory, std::uint64_t count) {
    retired(memory, count);
#if !defined(_WIN32)
    if (idle_timeline_file())
        ++run_trace.boundaries;
#endif
    // 跨程序 handle 請求（DuplicateHandle、IsProcessInJob 等）由請求方同步等待回覆。常駐模式改以時間片排程後，
    // 忙碌中的目標要等下一次 wake_threads 才處理，請求方每次約等 1.5 ms。監看執行緒在轉移通道可讀時設旗標；
    // 這裡只多一次原子讀取，有請求才立即排空，不做完整排程或其他來源的輪詢。
    if (handle_transfer_requested())
        poll_handle_transfers(memory);
    if(!resident_execution_depth_ && runtime_.host_clock && adaptive_scheduler_budget())
        threads_.host_instruction_budget.observe(count,runtime_.clock_ticks);
    if (threads_.items.empty())
        return;
    threads_.items.at(threads_.current).cycles += count;
#if !defined(_WIN32)
    // 診斷：X64ARM_TRACE_THREAD_CYCLES=<目錄> 時，約每 100M 條客體指令記錄各執行緒的累計指令。
    // 只讀取排程狀態，不改變執行；未設定時只多一次靜態旗標檢查。
    static const char *const trace_directory = std::getenv("X64ARM_TRACE_THREAD_CYCLES");
    if (trace_directory) {
        static std::uint64_t traced = 0, next_report = 100'000'000;
        static std::FILE *trace_file = nullptr;
        static const auto trace_started = std::chrono::steady_clock::now();
        traced += count;
        if (traced >= next_report) {
            next_report = traced + 100'000'000;
            if (!trace_file) {
                const auto path = std::string(trace_directory) + "/threads-" + std::to_string(::getpid()) + ".log";
                trace_file = std::fopen(path.c_str(), "a");
            }
            if (trace_file) {
                const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - trace_started).count();
                static const bool trace_stacks = std::getenv("X64ARM_TRACE_THREAD_STACKS") != nullptr;
                static double next_stack_report = 0;
                const bool capture_stacks = trace_stacks && seconds >= next_stack_report;
                if (capture_stacks) next_stack_report = seconds + 10;
                std::fprintf(trace_file, "t=%.3f total=%llu current=%llu\n", seconds, static_cast<unsigned long long>(traced),
                             static_cast<unsigned long long>(threads_.current));
                std::fprintf(trace_file,
                             "  idle periods=%llu total_ms=%.1f signalled=%llu (%.1f ms) timer=%llu (%.1f ms) longest_ms=%.2f "
                             "hist(<0.1ms,<0.5,<1,<5,<20,>=20ms)=%llu,%llu,%llu,%llu,%llu,%llu\n",
                             static_cast<unsigned long long>(idle_trace.periods), static_cast<double>(idle_trace.nanoseconds) / 1e6,
                             static_cast<unsigned long long>(idle_trace.signalled), static_cast<double>(idle_trace.signalled_ns) / 1e6,
                             static_cast<unsigned long long>(idle_trace.timer), static_cast<double>(idle_trace.timer_ns) / 1e6,
                             static_cast<double>(idle_trace.longest_ns) / 1e6, static_cast<unsigned long long>(idle_trace.buckets[0]),
                             static_cast<unsigned long long>(idle_trace.buckets[1]),
                             static_cast<unsigned long long>(idle_trace.buckets[2]),
                             static_cast<unsigned long long>(idle_trace.buckets[3]),
                             static_cast<unsigned long long>(idle_trace.buckets[4]),
                             static_cast<unsigned long long>(idle_trace.buckets[5]));
                static std::map<std::uint64_t, std::uint64_t> last_cycles;
                for (const auto &[id, thread] : threads_.items)
                    if (thread.cycles) {
                        const auto delta = thread.cycles - last_cycles[id];
                        last_cycles[id] = thread.cycles;
                        std::fprintf(trace_file, "  id=%llu cycles=%llu delta=%llu status=%d entry=0x%llx name=%s\n",
                                     static_cast<unsigned long long>(id), static_cast<unsigned long long>(thread.cycles),
                                     static_cast<unsigned long long>(delta), static_cast<int>(thread.status),
                                     static_cast<unsigned long long>(thread.entry),
                                     utf16_to_utf8(thread.description).c_str());
                        // 有界、唯讀的等待診斷；x64 使用原始 PE unwind 資料，不要求保留 RBP。
                        if (capture_stacks && thread.status != ThreadStatus::Exited) {
                            auto walk = id == threads_.current ? state : thread.context;
                            std::fprintf(trace_file, "    wait kind=%d deadline=%llu clock=%llu handles=",
                                         static_cast<int>(thread.wait.kind),
                                         static_cast<unsigned long long>(thread.wait.deadline),
                                         static_cast<unsigned long long>(runtime_.clock_ticks));
                            for (const auto handle : thread.wait.handles)
                                std::fprintf(trace_file, "0x%llx,", static_cast<unsigned long long>(handle));
                            std::fputc('\n', trace_file);
                            for (unsigned depth = 0; depth < 24 && walk.rip.value; ++depth) {
                                const auto pc = walk.rip.value, sp = walk.reg(Gpr::Rsp);
                                const auto api = handles(walk.rip) ? name(walk.rip) : std::string_view{};
                                std::fprintf(trace_file, "    frame=%u pc=0x%llx sp=0x%llx api=%.*s\n", depth,
                                             static_cast<unsigned long long>(pc), static_cast<unsigned long long>(sp),
                                             static_cast<int>(api.size()), api.empty() ? "" : api.data());
                                try {
                                    const RuntimeImage leaf{};
                                    const auto* image = module_image(pc, true);
                                    if (x86_ || handles(walk.rip)) {
                                        walk.rip = GuestAddress{memory.load(GuestAddress{sp}, x86_ ? 32 : 64)};
                                        walk.reg(Gpr::Rsp) += x86_ ? 4 : 8;
                                        if (x86_) break; // 不把 x86 任意堆疊資料誤認為返回位址。
                                    } else walk = virtual_unwind(memory, image ? *image : leaf, pc, walk, 0).caller;
                                    if (walk.reg(Gpr::Rsp) <= sp) break;
                                } catch (const std::exception&) { break; }
                            }
                        }
                        // 本段有明顯進度的執行緒：記錄目前 RIP 與 frame pointer 鏈上的返回位址（供事後依模組匯出歸類）。
                        if (delta >= 2'000'000) {
                            GuestState context = id == threads_.current ? state : thread.context;
                            std::fprintf(trace_file, "    rip=0x%llx", static_cast<unsigned long long>(context.rip.value));
                            auto frame = context.reg(Gpr::Rbp);
                            const unsigned width = x86_ ? 4U : 8U;
                            for (unsigned depth = 0; depth < 12 && frame; ++depth) {
                                try {
                                    const auto next = memory.load(GuestAddress{frame}, static_cast<std::uint8_t>(width * 8));
                                    const auto caller = memory.load(GuestAddress{frame + width}, static_cast<std::uint8_t>(width * 8));
                                    std::fprintf(trace_file, " 0x%llx", static_cast<unsigned long long>(caller));
                                    if (next <= frame)
                                        break;
                                    frame = next;
                                } catch (const MemoryFault &) {
                                    break;
                                }
                            }
                            std::fputc('\n', trace_file);
                        }
                    }
                std::fflush(trace_file);
            }
        }
    }
#endif
    const bool resident=resident_execution_depth_!=0;
    const bool clock_slice=!resident && runtime_.host_clock && clock_scheduled_slices();
    if(!resident && !clock_slice)threads_.remaining = count >= threads_.remaining ? 0 : threads_.remaining - count;
    // 正常 AOT 的排程只在已存在的服務／語義邊界輪替；純原生迴圈不呼叫這裡，熱迴圈不檢查配額或時間。
    // 多執行緒時不在每個邊界都做完整排程與外部事件輪詢，改在宿主時鐘時間片（與計量模式相同的
    // 2 ms×優先序）到期或明確 yield 時輪替。retired() 對小批次延後同步時鐘，這裡直接讀單調時鐘；
    // 尚未設定截止點（UINT64_MAX）視為到期，由 schedule 建立。精確觀察與舊 JIT 保留原計量排程。
    // X64ARM_RESIDENT_SCHEDULE_EVERY_BOUNDARY=1 恢復舊行為（每個邊界都排程），只供同成品對照。
    static const bool every_boundary=[] {const auto* setting=std::getenv("X64ARM_RESIDENT_SCHEDULE_EVERY_BOUNDARY");
        return setting && std::string_view(setting)=="1";}();
    std::uint64_t now=0; // 需要時才讀單調時鐘（100 ns），同一邊界共用。
    // 等待期限已過的客體也要完整排程解析逾時：否則平行批次的喚醒期限持續為「立即」，協調者每次逾時後
    // 重新排程卻不解析，形成連續靜止（DQ3 強制平行時約占短期限的 45%）。
    const bool resident_due=resident && threads_.items.size()>1 && (every_boundary ||
        threads_.slice_deadline==UINT64_MAX || (now=detail::monotonic_ticks())>=threads_.slice_deadline ||
        now>=resident_wait_deadline_);
    if (threads_.yield || (resident?resident_due:
        ((!clock_slice && !threads_.remaining) ||
         (runtime_.host_clock && runtime_.clock_ticks >= threads_.slice_deadline)))) {
        // 指令退休後 TEB 才是客體的最新值；直接寫入不能被 schedule 的
        // provider 快取覆蓋。API 內主動排程仍保留尚未發布的服務結果。
        if (state.gs_base || state.fs_base)
            last_error_ = static_cast<std::uint32_t>(memory.load(
                GuestAddress{x86_ ? state.fs_base + 0x34 : state.gs_base + 0x68}, 32));
        schedule(state, memory);
    } else if(resident && parallel_workers()>1 && !debugger_stopped()) {
        // 服務／語義邊界已交回完整狀態；沒有完整排程時，先將已滿足的
        // 物件等待轉為 Runnable，讓保留中的 worker 可立即接手。共用解析
        // 保留 WaitAll、訊號消耗與返回值，不輪詢外部裝置或強迫客體切換。
        // 遊戲每幀有數千次 API 邊界，每次都走訪全部等待者會吃掉協調執行緒大半時間；同一時間片內最多每
        // X64ARM_RESIDENT_OBJECT_WAKE_US（預設 100 µs，0＝每個邊界）解析一次。只延後閒置 worker 接手已就緒
        // 執行緒的時機：目前執行緒進入等待、讓出或時間片到期仍走完整排程，不會漏掉喚醒；通知政策不變。
        static const std::uint64_t object_wake_interval=[] {const auto* value=std::getenv("X64ARM_RESIDENT_OBJECT_WAKE_US");
            return value && *value?std::strtoull(value,nullptr,10)*10U:UINT64_C(1000);}();
        bool changed=false;
        if(object_wake_interval){if(!now)now=detail::monotonic_ticks();}
        if(!object_wake_interval || now>=resident_object_wake_after_){
            resident_object_wake_after_=now+object_wake_interval;
            changed=resolve_thread_waits(memory,ThreadWakeScope::Objects);
        }
        // 精確通知候選只在等待／排程狀態改變時撤銷續行資格；仍待效能驗收，
        // 預設沿用舊通知政策，明確設定 X64ARM_RESIDENT_PRECISE_WAKE 非 0 才啟用。
        // 時間片、外部通知及完整 API 前置保持原契約。
        // 2026-10-24 起預設開啟：DQ3 讀檔載入約 50→30 秒、Steam 啟動交錯量測在變動範圍內；=0 恢復每邊界通知。
        static const bool precise_wake=[] {const auto* value=std::getenv("X64ARM_RESIDENT_PRECISE_WAKE");
            return !value || !*value || std::string_view(value)!="0";}();
        const bool notify=changed || !precise_wake;
        if(notify)notify_concurrency_changed();
        static const bool trace_wake=std::getenv("X64ARM_TRACE_RESIDENT_WAKE")!=nullptr;
        if(trace_wake){
            struct Counts {std::uint64_t calls{},changed{},notifications{};};
            static thread_local Counts counts;
            ++counts.calls;counts.changed+=changed;counts.notifications+=notify;
            // 只在診斷時累計，沒有逐 API 讀時鐘；低頻輸出不改變等待／喚醒語義。
            if((counts.calls&16383U)==0)std::fprintf(stderr,
                "resident_wake calls=%llu changed=%llu notifications=%llu precise=%d\n",
                static_cast<unsigned long long>(counts.calls),static_cast<unsigned long long>(counts.changed),
                static_cast<unsigned long long>(counts.notifications),precise_wake?1:0);
        }
    }
    if(AdmissionTrace::enabled())trace_provider_admission(*this,threads_,resident_service_requests_,runtime_.processor_count,
        runtime_.process_priority_class,runtime_.process_background,parallel_cursor_,debugger_stopped(),2);
    // 跨程序記憶體操作或排程期間套用的映射變更也在此邊界讓 CFG 快速路徑失效。
    refresh_guard_table(memory);
}
WindowsServices::CompletionResult WindowsServices::poll_completion(GuestState& state, GuestMemory& memory,
                                                                 std::uint64_t handle, bool extended, bool expired) {
    const auto port = runtime_.completion_ports.find(handle);
    if (port == runtime_.completion_ports.end())
        return {0,735,true,false};
    const unsigned unit = x86_ ? 4 : 8;
    const std::uint8_t bits = x86_ ? 32 : 64;
    const WindowsCall arg(state,memory,extended ? 6 : 5);
    const auto lane = [&](unsigned index, unsigned offset, std::uint64_t value, unsigned bytes) {
        std::memcpy(state.xmm[index].data()+offset,&value,bytes);
    };
    auto& packets = port->second.packets;
    if (!packets.empty()) {
        if (extended) {
            const auto count = std::min<std::size_t>(static_cast<std::uint32_t>(arg(2)),packets.size());
            memory.check(GuestAddress{arg(1)},count*4*unit,Access::Write);
            memory.check(GuestAddress{arg(3)},4,Access::Write);
            for (std::size_t i=0;i<count;++i) {
                const auto& packet = packets[i];
                const auto at = arg(1)+i*4*unit;
                memory.store(GuestAddress{at},bits,packet.key);
                memory.store(GuestAddress{at+unit},bits,packet.overlapped);
                memory.store(GuestAddress{at+2*unit},bits,packet.status);
                memory.store(GuestAddress{at+3*unit},32,packet.bytes);
            }
            memory.store(GuestAddress{arg(3)},32,count);
            for (std::size_t i=0;i<count;++i)packets.pop_front();
            return {1,0,true,false};
        }
        const auto packet = packets.front();
        const auto bytes = arg(1), key = arg(2), overlapped = arg(3);
        memory.check(GuestAddress{bytes},4,Access::Write);
        memory.check(GuestAddress{key},unit,Access::Write);
        memory.check(GuestAddress{overlapped},unit,Access::Write);
        const bool guest_return = guest_io_completion_.value != 0;
        if (guest_return) {
            // 所有指標先經原 memory provider 驗證；續跑仍用正常客體 store，保留 COW／別名與撤銷檢查。
            for (unsigned n=0;n<(x86_?2U:4U);++n)state.xmm[n].fill(0);
            if (x86_) {
                lane(0,0,bytes,4);lane(0,4,key,4);lane(0,8,overlapped,4);
                lane(1,0,packet.bytes,4);lane(1,4,packet.key,4);lane(1,8,packet.overlapped,4);
            } else {
                lane(0,0,bytes,8);lane(0,8,key,8);lane(1,0,overlapped,8);
                lane(2,0,packet.bytes,4);lane(2,8,packet.key,8);lane(3,0,packet.overlapped,8);
            }
            state.reg(Gpr::Rax)=packet.status==0;
            state.rip=GuestAddress{guest_io_completion_.value+(x86_?detail::guest_io_packet_x86:detail::guest_io_packet_x64)};
            ++io_wait_stats_.packets;
        } else {
            memory.store(GuestAddress{bytes},32,packet.bytes);
            memory.store(GuestAddress{key},bits,packet.key);
            memory.store(GuestAddress{overlapped},bits,packet.overlapped);
        }
        packets.pop_front();
        const std::uint32_t error = !packet.status ? 0
            : packet.status==0xc0000017 ? 8 : packet.status==0xc0000005 ? 998
            : packet.status==0xc0000022 ? 5 : packet.status==0xc0000054 ? 33
            : packet.status==0xc0000011 ? 38 : packet.status==0x80000005 ? 234
            : packet.status==0xc000014b ? 109 : packet.status==0xc00000b0 ? 233
            : packet.status==0xc000020d ? 64 : packet.status==0xc0000241 ? 1236
            : packet.status==0xc000009a ? 1450 : packet.status==0xc00000b5 ? 121
            : packet.status==0x10c ? 1022 : packet.status==0xc0000236 ? 1225
            : packet.status==0xc000023c ? 1231 : packet.status==0xc000023d ? 1232
            : packet.status==0xc000020c ? 1226 : packet.status==0xc000000d ? 87 : 995;
        return {static_cast<std::uint32_t>(packet.status==0),error,true,guest_return};
    }
    if (!expired)return {};
    const auto output = arg(3);
    memory.check(GuestAddress{output},extended?4:unit,Access::Write);
    const bool guest_return = !extended && guest_io_completion_.value != 0;
    if (guest_return) {
        state.xmm[0].fill(0);lane(0,0,output,unit);
        state.reg(Gpr::Rax)=0;
        state.rip=GuestAddress{guest_io_completion_.value+(x86_?detail::guest_io_timeout_x86:detail::guest_io_timeout_x64)};
        ++io_wait_stats_.timeouts;
    } else memory.store(GuestAddress{output},extended?32:bits,0);
    return {0,258,true,guest_return};
}
void WindowsServices::wake_threads(GuestMemory &memory) {
    startup_profile::Scope measurement("scheduler", "wake_threads");
    WakePollScope wake_measurement("wake_threads");
    process_io::Scope io_scope(runtime_.io_counters, io_publisher_.get());
    {
        WakePollScope poll_measurement("poll_http_io");
        poll_http_io();
    }
    {
        WakePollScope poll_measurement("poll_autoproxy");
        poll_autoproxy(memory);
    }
    {
        WakePollScope poll_measurement("poll_handle_transfers");
        poll_handle_transfers(memory);
    }
    {
        WakePollScope poll_measurement("poll_job_termination");
        poll_job_termination(memory);
    }
    poll_machine_shutdown();
    if (runtime_.forced_exit_code() || debugger_stopped())
        return;
    {
        WakePollScope poll_measurement("poll_directory_notifications");
        poll_directory_notifications();
    }
    {
        WakePollScope poll_measurement("poll_shell_notifications");
        poll_shell_notifications(memory);
    }
    {
        WakePollScope poll_measurement("poll_file_io");
        poll_file_io(memory, true);
    }
    {
        WakePollScope poll_measurement("poll_network_io");
        poll_network_io(memory);
    }
    {
        WakePollScope poll_measurement("poll_midi_input");
        poll_midi_input(memory);
    }
    {
        WakePollScope poll_measurement("poll_media_foundation");
        poll_media_foundation(memory);
    }
    {
        WakePollScope poll_measurement("poll_wave_input");
        poll_wave_input(memory);
    }
    {
        WakePollScope poll_measurement("poll_wave_output");
        poll_wave_output(memory);
    }
    {
        WakePollScope poll_measurement("poll_winusb");
        poll_winusb(memory);
    }
    {
        WakePollScope poll_measurement("poll_dsound");
        poll_dsound();
    }
    {
        WakePollScope poll_measurement("poll_wasapi");
        poll_wasapi();
    }
    {
        WakePollScope poll_measurement("poll_directmusic");
        poll_directmusic();
    }
    {
        WakePollScope poll_measurement("poll_dinput");
        poll_dinput(true);
    }
    {
        WakePollScope poll_measurement("poll_address_notifications");
        poll_address_notifications(memory);
    }
    {
        WakePollScope poll_measurement("poll_desktop_notifications");
        poll_desktop_notifications();
    }
    {
        WakePollScope poll_measurement("poll_device_notifications");
        poll_device_notifications(memory);
    }
    {
        WakePollScope poll_measurement("poll_bluetooth");
        poll_bluetooth(memory);
    }
    {
        WakePollScope poll_measurement("poll_registry_notifications");
        poll_registry_notifications();
        poll_guest_services(memory);
    }
    {
        WakePollScope poll_measurement("poll_power_notifications");
        poll_power_notifications(memory);
    }
    {
        WakePollScope poll_measurement("poll_timer_queues");
        poll_timer_queues(memory);
    }
    {
        {WakePollScope poll_measurement("refresh_memory_resource_notifications");(void)refresh_memory_resource_notifications();}
        WakePollScope poll_measurement("poll_registered_waits");
        poll_registered_waits(memory);
    }
    {
        WakePollScope poll_measurement("poll_threadpool");
        poll_threadpool(memory);
    }
    {
        WakePollScope poll_measurement("poll_etw");
        poll_etw(memory);
    }
    {WakePollScope poll_measurement("poll_perflib");poll_perflib(memory);}
    // 執行中的客體每個指令時間片仍檢查等待物件；AppKit 外部事件最多每毫秒輪詢一次。
    // 明確的訊息 API 與所有執行緒等待時仍立即 pump；已到期的 GUI 計時器也不延後。
    const bool gui_due = !runtime_.host_clock || !native_gui_ || runtime_.clock_ticks >= gui_.scheduler_poll_deadline ||
                         std::any_of(gui_.timers.begin(), gui_.timers.end(),
                                     [&](const auto &timer) { return timer.second.deadline <= runtime_.clock_ticks; });
    if (gui_due && std::any_of(threads_.items.begin(), threads_.items.end(), [](const auto &entry) {
            return entry.second.status == ThreadStatus::Waiting && entry.second.wait.kind == ThreadWaitKind::GuiInput;
        })) {
        gui_.scheduler_poll_deadline = runtime_.clock_ticks + 10000;
        WakePollScope poll_measurement("pump_gui");
        pump_gui(memory, false);
    }
    // 等待完成、取消或執行緒結束後，釋放不再使用的內部物件參照。
    for(auto it=threads_.retained_wait_handles.begin();it!=threads_.retained_wait_handles.end();){
        const auto handle=*it;
        const bool pending=std::any_of(threads_.items.begin(),threads_.items.end(),[&](const auto& entry){
            const auto& thread=entry.second;
            return thread.status==ThreadStatus::Waiting &&
                (thread.wait.kind==ThreadWaitKind::Objects || thread.wait.kind==ThreadWaitKind::GuiInput) &&
                std::find(thread.wait.handles.begin(),thread.wait.handles.end(),handle)!=thread.wait.handles.end();
        });
        if(pending){++it;continue;}
        it=threads_.retained_wait_handles.erase(it);
        (void)close_wait_object(handle);
    }
    // 只走訪計時器索引；先複製 handle 清單，迴圈內的變動不影響走訪。
    const std::vector<std::uint64_t> timer_handles(runtime_.wait_objects.timers().begin(), runtime_.wait_objects.timers().end());
    for (const auto handle : timer_handles) {
        const auto found_timer = runtime_.wait_objects.find(handle);
        if (found_timer == runtime_.wait_objects.end())
            continue;
        auto &entry = found_timer->second;
        auto &object = runtime_.wait_state(entry);
        if(object.kind==WaitObject::Kind::Timer && object.timer_routine){const auto owner=threads_.items.find(object.timer_owner);if(owner==threads_.items.end() || owner->second.status==ThreadStatus::Exited){object.active=false;cancel_waitable_timer_apcs(entry.identity);}}
        if(object.kind==WaitObject::Kind::Timer && object.active && object.absolute_due){object.timer_nominal_due=runtime_.system_deadline(object.absolute_due);object.due=coalesce_waitable_timer(object);}
        if (object.kind != WaitObject::Kind::Timer || !object.active || object.due > runtime_.clock_ticks)
            continue;
        object.count = 1;
        object.absolute_due=0;
        if(object.timer_routine){auto& callbacks=threads_.items.at(object.timer_owner).apcs;
            if(callbacks.size()<1024 && std::none_of(callbacks.begin(),callbacks.end(),[&](const auto& callback){return callback.waitable_timer==entry.identity;})){
                const auto time=runtime_.system_filetime();GuestCallback callback{object.timer_routine,{object.timer_argument,static_cast<std::uint32_t>(time),static_cast<std::uint32_t>(time>>32)}};
                callback.waitable_timer=entry.identity;callbacks.push_back(std::move(callback));}
        }
        if (object.period){const auto periods=1+(runtime_.clock_ticks-object.timer_nominal_due)/object.period;
            object.timer_nominal_due=periods>(UINT64_MAX-object.timer_nominal_due)/object.period?UINT64_MAX:object.timer_nominal_due+periods*object.period;object.due=coalesce_waitable_timer(object);}
        else
            object.active = false;
    }
    {WakePollScope poll_measurement("resolve_thread_waits");resolve_thread_waits(memory);}
    // 脈衝只屬於這次完整事件輪詢；局部鎖通知不消耗其他事件。
    for (const auto handle : multimedia_pulses_) {
        if (const auto event = runtime_.wait_objects.find(handle); event != runtime_.wait_objects.end())
            event->second.set_signal(false);
    }
    multimedia_pulses_.clear();
}
bool WindowsServices::resolve_thread_waits(GuestMemory& memory,ThreadWakeScope scope,std::uint64_t wake_address) {
    // 完整排程與同步通知共用取鎖、逾時、返回值及優先序處理。局部通知
    // 只更新相符等待者，不輪詢外部事件、不切換目前客體；worker 重綁時
    // 就能看到新的 Runnable 集合，無須等下一個時間片。
    bool changed=false;
    // 不取共用命名空間鎖，先判斷這組 handle 是否「可能」就緒；可能就緒時才在鎖下重查／消耗，保留 auto-reset 與
    // WaitAll 的原子性。共用 event 讀原子訊號；本地物件、執行緒、子程序與檔案 I/O 的狀態不需共用鎖；共用 mutex 的
    // 擁有權（含 abandoned 探查）只能在鎖下判斷，視為可能就緒。schedule 在 wake_threads 之前保存喚醒 epoch，
    // 檢查後才發生的 SetEvent 不會遺失。
    const auto may_be_ready=[&](const std::vector<std::uint64_t>& handles,bool all,std::uint64_t thread_id){
        bool any=false,every=true;
        for(const auto handle:handles){
            bool possible=true;
            if(const auto object=runtime_.wait_objects.find(handle);object!=runtime_.wait_objects.end()){
                if(object->second.shared_event)possible=object->second.signaled();
                else if(runtime_.wait_lock_descriptor(object->second)<0)possible=runtime_.wait_ready(object->second,handles,all,thread_id);
            }else if(const auto target=this->thread_id(handle))possible=threads_.items.at(target).status==ThreadStatus::Exited;
            else if(const auto child=process_signaled(handle))possible=*child;
            else if(const auto file=file_io_signaled(handle))possible=*file;
            any|=possible;every&=possible;
            if(all?!every:any)break;
        }
        return all?every:any;
    };
    for (const auto &[id, entry] : live_threads()) {
        auto &thread = *entry;
        if (thread.status != ThreadStatus::Waiting)
            continue;
        auto &wait = thread.wait;
        if(scope!=ThreadWakeScope::All){
            const bool condition=wait.kind==ThreadWaitKind::Condition && wait.handles.size()>1;
            bool selected=false;
            if(scope==ThreadWakeScope::Condition)
                selected=condition && wait.handles[0]==wake_address;
            else if(scope==ThreadWakeScope::Slim)
                selected=(wait.kind==ThreadWaitKind::Slim && !wait.handles.empty() && wait.handles[0]==wake_address) ||
                    (condition && !wait.critical && wait.handles[1]==wake_address);
            else if(scope==ThreadWakeScope::Critical)
                selected=((wait.kind==ThreadWaitKind::Critical || wait.kind==ThreadWaitKind::CriticalRetry ||
                           (wait.kind==ThreadWaitKind::CrtLockRetry && wait.critical)) &&
                          !wait.handles.empty() && wait.handles[0]==wake_address) ||
                    (condition && wait.critical && wait.handles[1]==wake_address);
            else if(scope==ThreadWakeScope::Objects)
                // 別名 handle 仍由鎖內的實際物件與 WaitAll 契約決定資格。
                selected=wait.kind==ThreadWaitKind::Objects;
            if(!selected)continue;
        }
        // 部分等待直接重新執行原 API，會在下方提早 continue；統一在
        // 離開這個等待者時回報狀態變化，不只涵蓋尾端 complete_call。
        struct WaitChange {
            GuestThread& thread;
            bool& changed;
            ThreadStatus status;
            ThreadWaitKind kind;
            std::uint64_t deadline;
            bool signaled;
            ~WaitChange(){
                if(status==ThreadStatus::Waiting && thread.status==ThreadStatus::Runnable)thread.wake_pending=true;
                changed=changed || thread.status!=status || thread.wait.kind!=kind ||
                    thread.wait.deadline!=deadline || thread.wait.signaled!=signaled;
            }
        } change{thread,changed,thread.status,wait.kind,wait.deadline,wait.signaled};
        bool ready = false;
        bool guest_return = false;
        std::uint64_t result = 258;
        if(wait.kind==ThreadWaitKind::RegistryIo){
            // owner 的原生呼叫框架仍在外層；其他依賴登錄的 API 尚未執行任何副作用。
            if(id==registry_io_owner_ ? registry_io_ready() : !registry_io_busy_){
                if(thread.registry_deferred_wait){
                    thread.wait=std::move(*thread.registry_deferred_wait);
                    thread.registry_deferred_wait.reset();
                }
                thread.status=ThreadStatus::Runnable;
            }
            continue;
        }
        if(wait.kind==ThreadWaitKind::HandleTransfer){
            if(finish_duplicate_remote_handle(thread.context,memory,id)){
                thread.status=ThreadStatus::Runnable;thread.wait={};
            }
            continue;
        }
        if(wait.kind==ThreadWaitKind::ProcessCreation){
            if(finish_guest_process_creation(thread.context,memory,id)){
                thread.status=ThreadStatus::Runnable;thread.wait={};
            }
            continue;
        }
        if(wait.kind==ThreadWaitKind::Sleep && !thread.callbacks.empty() && thread.callbacks.back().continuation==Continuation::PolicyRetry){
            const auto pending=policy_sections_.find(thread.callbacks.back().value);
            const auto wake=mapping_wake();
            if(pending==policy_sections_.end() || (wake && wake->snapshot()!=pending->second.wake_epoch)){
                ready=true;result=0;
                if(pending!=policy_sections_.end())startup_profile::mark("policy_notified");
            }
        }
        if (wait.kind == ThreadWaitKind::DebugEvent) {
            if (finish_debug_wait(thread.context, memory, id, wait.deadline))
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::CrtProcess) {
            const auto signaled=process_signaled(wait.handles.at(0));
            if(!signaled || *signaled)thread.status=ThreadStatus::Runnable;
            continue; // 保留 CRT 呼叫框架，醒來後取得 DWORD 狀態並關閉 handle。
        }
        if (wait.alertable && !thread.apcs.empty()) {
            thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::RegistryNotify) {
            if (!registry_notifications_.contains(wait.handles.at(0))) {
                wait.signaled = true;
                thread.status = ThreadStatus::Runnable;
            }
            continue;
        }
        if (wait.kind == ThreadWaitKind::EtwTrace) {
            if (std::any_of(wait.handles.begin(), wait.handles.end(),
                            [&](auto handle) { return runtime_.traces.ready(handle); }))
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::EtwEnable) {
            const auto found = etw_enable_waits_.find(id);
            bool pending = false;
            if (found != etw_enable_waits_.end())
                for (auto notification : found->second)
                    pending |= runtime_.traces.notification_pending(notification);
            if (!pending || wait.deadline <= runtime_.clock_ticks)
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::RegisteredWait || wait.kind == ThreadWaitKind::ServiceControl)
            continue;
        if (wait.kind == ThreadWaitKind::MediaFoundation) {
            if (media_foundation_ready(wait.handles.at(0), id))
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::PowerNotificationCompletion) {
            if (!power_notification_pending(wait.handles.at(0)))
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::ThreadpoolCompletion) {
            if (!threadpool_callbacks_pending(wait.handles.at(0)))
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::RegisteredWaitCompletion) {
            if (!registered_waits_.contains(wait.handles.at(0)))
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::TimerQueueCompletion) {
            const bool pending = wait.handles.at(1) ? timer_queues_.contains(wait.handles.at(0))
                                                    : queue_timers_.contains(wait.handles.at(0));
            if (!pending)
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::Http) {
            if (http_ready(wait.handles.at(0))) {
                thread.status = ThreadStatus::Runnable;
                thread.wait = {};
            }
            continue;
        }
        if (wait.kind == ThreadWaitKind::NativeDevice) {
            if (bluetooth_ready(wait.handles.at(0))) {
                thread.status = ThreadStatus::Runnable;
                thread.wait = {};
            }
            continue;
        }
        if (wait.kind == ThreadWaitKind::Perflib) {
            if (perflib_consumer_ready(wait.handles.at(0))) {
                thread.status = ThreadStatus::Runnable;
                thread.wait = {};
            }
            continue;
        }
        if (wait.kind == ThreadWaitKind::Resolver) {
            const auto job=runtime_.network.resolvers.find(id);
            if(job==runtime_.network.resolvers.end() || job->second->ready.load(std::memory_order_acquire)){
                thread.status=ThreadStatus::Runnable;thread.wait={};
            }
            continue;
        }
        if (wait.kind == ThreadWaitKind::SocketSelect) {
            if (socket_select_ready(wait))
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::FileIo) {
            if(wait.handles.size()==3) {
                const auto handle=wait.handles[2];bool signaled=false;
                if(!handle)signaled=memory.load(GuestAddress{wait.handles[1]},x86_?32:64)!=0x103;
                else if(const auto event=runtime_.wait_objects.find(handle);event!=runtime_.wait_objects.end()){
                    // 共用 event 尚未有訊號時不取命名空間鎖（仍照常判定逾時）；有訊號才在鎖下重查並消耗。
                    if(!event->second.shared_event || event->second.signaled()){
                        SharedEventLock event_lock(runtime_.wait_lock_descriptor(event->second));
                        auto& signal=runtime_.wait_state(event->second);signaled=signal.signaled();if(signaled)signal.consume_signal();
                    }
                }else if(const auto file=file_io_signaled(handle))signaled=*file;
                else signaled=true; // handle 在等待期間被關閉，由共用查詢流程回報錯誤。
                wait.signaled=signaled;
                if(signaled || wait.deadline<=runtime_.clock_ticks){wait.timed_out=!signaled;thread.status=ThreadStatus::Runnable;}
                continue;
            }
            bool pending = false;
            if (wait.handles.size() == 2)
                pending = memory.load(GuestAddress{wait.handles[1]}, x86_ ? 32 : 64) == 0x103;
            else if (!wait.handles.empty() && !wait.handles[0]) {
                const WindowsCall args(thread.context, memory, 2);
                std::uint32_t timeout = 0;
                const auto pipe_name = name(thread.context.rip).ends_with('W')
                                           ? read_utf16(memory, GuestAddress{args(0)}, 256)
                                           : utf8_to_utf16(read_ansi(memory, args(0), 256));
                pending = NamedPipe::available(mapping_session(), pipe_name, timeout) == 231;
                if (wait.deadline <= runtime_.clock_ticks) {
                    wait.timed_out = true;
                    pending = false;
                }
            } else
                pending =
                    std::any_of(pending_file_io_.begin(), pending_file_io_.end(),
                                [&](const auto &io) {
                                    return (!io.overlapped || io.blocking) && io.thread == id && !io.done;
                                }) ||
                    std::any_of(runtime_.network.pending_socket_io.begin(), runtime_.network.pending_socket_io.end(),
                                [&](const auto &io) { return !io.overlapped && io.thread == id && !io.done; });
            if (!pending)
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::GuiSend) {
            const auto request =
                wait.handles.empty() ? gui_.sent_messages.end() : gui_.sent_messages.find(wait.handles[0]);
            if (request != gui_.sent_messages.end())
                wait.deadline = gui_send_deadline(request->second);
            if (request == gui_.sent_messages.end() || request->second.done || wait.deadline <= runtime_.clock_ticks ||
                (!(request->second.timeout_flags & 1U) && pending_gui_send(id, false)))
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::GuiInput) {
            const bool invalid_window =
                wait.gui_window && wait.gui_window != UINT64_MAX && !gui_.windows.contains(wait.gui_window);
            bool ended_modal = false;
            if (!thread.callbacks.empty() && thread.callbacks.back().continuation == Continuation::GuiModalDispatch) {
                const auto modal = gui_.windows.find(thread.callbacks.back().value);
                ended_modal = modal == gui_.windows.end() || modal->second.ended;
            }
            ready = std::any_of(gui_.queue.begin(), gui_.queue.end(), [&](const auto &message) {
                return gui_message_matches(message, id, wait.gui_window, wait.gui_min, wait.gui_max) &&
                       (gui_queue_category(message) & wait.gui_mask) && (!wait.gui_new_only || !message.queue_seen);
            });
            int lock = -1;
            const bool possible = wait.handles.empty() || may_be_ready(wait.handles, wait.all, id);
            for (const auto handle : possible ? wait.handles : std::vector<std::uint64_t>{}) {
                const auto object = runtime_.wait_objects.find(handle);
                if (object != runtime_.wait_objects.end() && runtime_.wait_lock_descriptor(object->second) >= 0) {
                    lock = runtime_.wait_lock_descriptor(object->second);
                    break;
                }
            }
            SharedEventLock event_lock(lock);
            if (!possible && wait.all)
                ready = false;
            for (const auto handle : possible ? wait.handles : std::vector<std::uint64_t>{}) {
                const auto object = runtime_.wait_objects.find(handle);
                const auto target = thread_id(handle);
                const auto process = process_signaled(handle);
                const bool signal = object != runtime_.wait_objects.end()
                                        ? runtime_.wait_ready(object->second, wait.handles, wait.all, id)
                                    : target ? threads_.items.at(target).status == ThreadStatus::Exited
                                             : process.value_or(true);
                ready = wait.all ? ready && signal : ready || signal;
            }
            ready |= invalid_window || ended_modal || pending_gui_send(id, wait.gui_callbacks) ||
                     (wait.gui_drag_detection && gui_drag_interrupted(wait.gui_drag_detection)) ||
                     (wait.gui_move_size && gui_move_size_interrupted(wait.gui_move_size));
            if (ready || wait.deadline <= runtime_.clock_ticks)
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::ConsoleInput) {
            if (wait.handles.empty() || console_input_ready(wait.handles[0]))
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::StandardInput) {
            if (standard_input_ready(wait.handles[0], wait.handles[1] != 0, wait.handles[2] != 0)) {
                // 保留 API 的 RIP 與參數，重新呼叫後才提交實際讀取結果。
                thread.status = ThreadStatus::Runnable;
                thread.wait = {};
            }
            continue;
        }
        if (wait.kind == ThreadWaitKind::Objects) {
            // 只有共用 event、且尚未到期時，先用原子訊號排除不可能成功的等待。
            // 正面結果仍在原命名空間鎖下重查／消耗，保留 auto-reset 與 WaitAll 的原子性。
            // schedule 在 wake_threads 之前保存喚醒 epoch；檢查後的 SetEvent 不會遺失。
            if (wait.deadline > runtime_.clock_ticks && !wait.handles.empty() && !may_be_ready(wait.handles, wait.all, id))
                continue;
            int shared_lock = -1;
            for (const auto handle : wait.handles) {
                const auto object = runtime_.wait_objects.find(handle);
                if (object != runtime_.wait_objects.end() && runtime_.wait_lock_descriptor(object->second) >= 0) {
                    shared_lock = runtime_.wait_lock_descriptor(object->second);
                    break;
                }
            }
            SharedEventLock event_lock(shared_lock);
            std::size_t first = wait.handles.size(), available = 0;
            bool valid = true;
            for (std::size_t i = 0; i < wait.handles.size(); ++i) {
                const auto object = runtime_.wait_objects.find(wait.handles[i]);
                const auto target = thread_id(wait.handles[i]);
                bool signaled = false;
                if (object != runtime_.wait_objects.end()) {
                    signaled = runtime_.wait_ready(object->second, wait.handles, wait.all, id);
                } else if (target)
                    signaled = threads_.items.at(target).status == ThreadStatus::Exited;
                else if (const auto child = process_signaled(wait.handles[i]))
                    signaled = *child;
                else if (const auto file = file_io_signaled(wait.handles[i]))
                    signaled = *file;
                else
                    valid = false;
                if (signaled) {
                    ++available;
                    if (first == wait.handles.size())
                        first = i;
                }
            }
            if (!valid) {
                ready = true;
                result = UINT32_MAX;
                memory.store(GuestAddress{thread.teb + (x86_ ? 0x34 : 0x68)}, 32, 6);
            } else if (wait.all ? available == wait.handles.size() : available != 0) {
                ready = true;
                result = wait.all ? 0 : first;
                for (std::size_t i = 0; i < wait.handles.size(); ++i)
                    if (wait.all || i == first) {
                        const auto object = runtime_.wait_objects.find(wait.handles[i]);
                        if (object != runtime_.wait_objects.end()) {
                            if (runtime_.acquire_wait_object(object->second, id))
                                result = 0x80U + (wait.all ? 0 : i);
                        }
                    }
            }
        }
        if (wait.kind == ThreadWaitKind::ProcessInputIdle) {
            const auto idle = process_signaled(wait.handles.front(), true);
            if (!idle) {
                ready = true;
                result = UINT32_MAX;
                memory.store(GuestAddress{thread.teb + (x86_ ? 0x34 : 0x68)}, 32, 6);
            } else if (*idle) {
                ready = true;
                result = 0;
            }
        }
        if (wait.kind == ThreadWaitKind::Completion) {
            const auto completion = poll_completion(thread.context,memory,wait.handles[0],wait.all,
                                                    wait.deadline<=runtime_.clock_ticks);
            ready=completion.ready;result=completion.value;guest_return=completion.guest_return;
            if (completion.error)
                memory.store(GuestAddress{thread.teb+(x86_?0x34:0x68)},32,completion.error);
        }
        if(wait.kind==ThreadWaitKind::CrtFileRetry){
            if(wait.deadline<=runtime_.clock_ticks)thread.status=ThreadStatus::Runnable;
            continue; // 重新執行原始 _locking 呼叫，保留每秒一次的嘗試次數與起始位置。
        }
        if (wait.kind == ThreadWaitKind::HeapRetry) {
            const auto lock = runtime_.heap_locks.find(wait.handles[0]);
            if (lock == runtime_.heap_locks.end() || !lock->second.owner || lock->second.owner == id) {
                thread.status = ThreadStatus::Runnable;
                thread.wait = {};
            }
            continue; // 重試原始 Heap／配置 API，保留其 ABI 參數與返回框架。
        }
        if (wait.kind == ThreadWaitKind::CriticalRetry) {
            const auto &section = runtime_.critical_sections.at(wait.handles[0]);
            if (!section.owner || section.owner == id) {
                thread.status = ThreadStatus::Runnable;
                thread.wait = {};
            }
            continue; // 保留 API RIP，取得鎖後重新執行原呼叫。
        }
        if(wait.kind==ThreadWaitKind::CrtLockRetry){
            const auto& locks=wait.critical?runtime_.critical_sections:runtime_.file_locks;
            const auto section=locks.find(wait.handles[0]);
            if(section==locks.end() || !section->second.owner || section->second.owner==id){thread.status=ThreadStatus::Runnable;thread.wait={};}
            continue; // 重試完整 CRT 呼叫，不能當成 _lock_file 已成功返回。
        }
        if (wait.kind == ThreadWaitKind::LoaderRetry) {
            if (!runtime_.loader_owner || runtime_.loader_owner == id)
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::Critical) {
            auto &section = runtime_.critical_sections.at(wait.handles[0]);
            if (!section.owner) {
                section.owner = id;
                section.recursion = 1;
                ready = true;
                result = 0;
                memory.store(GuestAddress{wait.handles[0] + (x86_ ? 4 : 8)}, 32, 0);
                memory.store(GuestAddress{wait.handles[0] + (x86_ ? 8 : 12)}, 32, 1);
                memory.store(GuestAddress{wait.handles[0] + (x86_ ? 12 : 16)}, x86_ ? 32 : 64, thread_identifier(id));
            }
        }
        if (wait.kind == ThreadWaitKind::CrtLock && wait.handles[0] == UINT64_MAX && guest_locale_lock_state_.value) {
            // locale 鎖的事實在客體狀態格：無主時由等待者取得並扣除等待數。
            const GuestAddress owner_cell = guest_locale_lock_state_, waiter_cell{owner_cell.value + 8};
            if (!memory.load(owner_cell, 32)) {
                memory.store(owner_cell, 32, thread_identifier(id));
                memory.store(GuestAddress{owner_cell.value + 4}, 32, 1);
                const auto waiters = memory.load(waiter_cell, 32);
                if (waiters) memory.store(waiter_cell, 32, waiters - 1);
                ready = true;
                result = 0;
            }
        } else if (wait.kind == ThreadWaitKind::CrtLock) {
            auto &lock = runtime_.file_locks.at(wait.handles[0]);
            if (!lock.owner) {
                lock.owner = id;
                lock.recursion = 1;
                ready = true;
                result = 0;
            }
        }
        if (wait.kind == ThreadWaitKind::Once) {
            const auto &once = runtime_.once_initializations.at(wait.handles[0]);
            // 初始化失敗可由另一個等待者重試；保留原 API 參數重新進入。
            if (once.complete || !once.owner)
                thread.status = ThreadStatus::Runnable;
            continue;
        }
        if (wait.kind == ThreadWaitKind::Condition) {
            if (!wait.signaled && wait.deadline <= runtime_.clock_ticks) {
                wait.signaled = true;
                wait.timed_out = true;
                wait.deadline = UINT64_MAX;
            }
            if (!wait.signaled)
                continue;
            const auto address = wait.handles[1];
            if (wait.critical) {
                auto &section = runtime_.critical_sections.at(address);
                if (section.owner)
                    continue;
                section.owner = id;
                section.recursion = 1;
                memory.store(GuestAddress{address + (x86_ ? 4 : 8)}, 32, 0);
                memory.store(GuestAddress{address + (x86_ ? 8 : 12)}, 32, 1);
                memory.store(GuestAddress{address + (x86_ ? 12 : 16)}, x86_ ? 32 : 64, thread_identifier(id));
            } else {
                // 依字組重新取得；取不到時已設等待位，持有者釋放會進宿主再解析。
                if (!acquire_slim_word(memory, address, id, wait.shared))
                    continue;
            }
            ready = true;
            result = wait.timed_out ? 0 : 1;
            if (wait.timed_out)
                memory.store(GuestAddress{thread.teb + (x86_ ? 0x34 : 0x68)}, 32, 1460);
        }
        if (wait.kind == ThreadWaitKind::Slim) {
            if (acquire_slim_word(memory, wait.handles[0], id, wait.shared)) {
                ready = true;
                result = 0;
            }
        }
        if (!ready && wait.deadline <= runtime_.clock_ticks) {
            ready = true;
            result = wait.kind == ThreadWaitKind::Sleep ? 0 : 258;
        }
        if (ready) {
            if (!runtime_.priority_boost_disabled && !thread.priority_boost_disabled &&
                wait.kind != ThreadWaitKind::Sleep && result != 258 && thread.priority > -15 && thread.priority < 15)
                thread.dynamic_priority_boost = 1;
            if (!guest_return)complete_call(thread.context, memory, result);
            thread.status = ThreadStatus::Runnable;
            thread.wait = {};
        }
    }
    return changed;
}
// FLS 鏡像：TEB+0x1840 放 128 個值，+0x400 放程序共用的使用中位元圖，TEB FlsData 指向它。
// 每個執行緒的鏡像永遠等於該執行緒目前（或其目前 fiber）的 FLS 值，所以執行緒切換不必同步；
// 只在值或使用中狀態改變時寫入。寫入失敗（TEB 已回收）時略過，客體常式不會再讀到它。
void WindowsServices::sync_fls_mirror(GuestMemory &memory, std::uint64_t teb, const std::array<std::uint64_t, 128> &values) noexcept {
    if (!fls_mirror_ || !teb)
        return;
    std::array<std::uint8_t, 128 * 8 + 16> bytes{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto value = runtime_.fls_used[i] ? values[i] : 0;
        for (unsigned n = 0; n < 8; ++n)
            bytes[i * 8 + n] = static_cast<std::uint8_t>(value >> (8 * n));
        if (runtime_.fls_used[i])
            bytes[128 * 8 + i / 8] |= static_cast<std::uint8_t>(1U << (i % 8));
    }
    try {
        memory.write(GuestAddress{teb + 0x1840}, bytes);
        memory.store(GuestAddress{teb + 0x17c8}, 64, teb + 0x1840);
    } catch (const std::exception &) {
    }
}
void WindowsServices::sync_current_fls_mirror(GuestMemory &memory) noexcept {
    if (!fls_mirror_)
        return;
    const auto current = threads_.items.find(threads_.current);
    if (current != threads_.items.end())
        sync_fls_mirror(memory, current->second.teb, runtime_.fls_values);
}
void WindowsServices::sync_fls_mirrors(GuestMemory &memory) noexcept {
    if (!fls_mirror_)
        return;
    for (const auto &[id, thread] : threads_.items)
        if (thread.status != ThreadStatus::Exited && thread.teb)
            sync_fls_mirror(memory, thread.teb, id == threads_.current ? runtime_.fls_values : thread.fls_values);
}
void WindowsServices::schedule(GuestState &state, GuestMemory &memory) {
    startup_profile::Scope measurement("scheduler", "schedule");
    resident_wait_deadline_=UINT64_MAX; // 排程會解析逾時；下次計算喚醒期限時重新記錄。
    notify_concurrency_changed(); // 等待完成與 runnable 集合只在協調執行緒更新。
    if (threads_.items.empty())
        return;
    struct SavedContextScope {
        bool &flag;
        bool previous;
        ~SavedContextScope() { flag = previous; }
    } scope{threads_.context_saved, threads_.context_saved};
    threads_.context_saved = true;
    auto &source = threads_.items.at(threads_.current);
    const auto tsc = state.tsc;
    source.context = state;
    source.callbacks = std::move(runtime_.callbacks);
    source.sorts = std::move(runtime_.sorts);
    source.exceptions = std::move(exceptions_.stack);
    if (state.gs_base || state.fs_base)
        memory.store(GuestAddress{x86_ ? state.fs_base + 0x34 : state.gs_base + 0x68}, 32, last_error_);
    // IPC／裝置完成沒有共同的宿主 wait handle；先縮短一次交接的延遲，
    // 持續閒置則退至 1 ms，避免長等待保持高頻輪詢。固定時鐘沿用原步長。
    std::uint64_t external_poll_ticks = runtime_.host_clock ? 500 : 10000;
    std::uint64_t spin_deadline = 0;
    const auto wake = runtime_.host_clock ? mapping_wake() : nullptr;
    auto handled_wake = wake ? wake->snapshot() : 0;
#if !defined(_WIN32)
    static const bool idle_tracing = std::getenv("X64ARM_TRACE_THREAD_CYCLES") != nullptr || idle_timeline_file() != nullptr;
    bool idle_active = false;
    std::chrono::steady_clock::time_point idle_start{};
    // 閒置傾印以本次 schedule 呼叫內的連續閒置計時；有 runnable 即返回，下次呼叫重新起算。
    std::chrono::steady_clock::time_point idle_dump_since{};
    bool idle_dumped = false;
    std::uint64_t idle_epoch = 0;
#endif
    for (;;) {
        const auto wake_epoch = wake ? wake->snapshot() : 0;
        resident_external_polled_ = wake_epoch; // 常駐中斷以此為基準；之後的外部通知一定會被看見。
        if (runtime_.host_clock){
            WakePollScope poll_measurement("retired");retired(memory, 0);
        }
        wait_debugger(memory);
        if (wake_epoch != handled_wake) {
            poll_handle_transfers(memory, true);
            handled_wake = wake_epoch;
        }
        wake_threads(memory);
        wait_debugger(memory);
        // 時間關鍵（優先序 15 以上：TIME_CRITICAL、MMCSS 多媒體／音訊）的 runnable 依 Windows 語義優先搶占，
        // 同優先序從目前執行緒之後輪流；一般優先序之間維持全體輪流——單核模擬下對一般優先序嚴格排序會讓
        // 低優先序的載入執行緒長期得不到 CPU（DQ3 首次可聽 90→200 秒），真實多核上它們本是並行。
        // 目前執行緒仍可執行而主動讓出時沿用全體輪流；可執行卻超過 50 ms 未被排到者暫視為優先序 15。
        // X64ARM_SCHED_STRICT_PRIORITY=0 恢復全體輪流。
        static const bool strict_priority=[] {const auto* value=std::getenv("X64ARM_SCHED_STRICT_PRIORITY");return !value || std::string_view(value)!="0";}();
        const auto current_thread = threads_.items.find(threads_.current);
        const bool voluntary_yield = threads_.yield && current_thread != threads_.items.end() &&
            current_thread->second.status == ThreadStatus::Runnable && !current_thread->second.suspended;
        auto chosen = threads_.items.upper_bound(threads_.current);
        if (strict_priority && !voluntary_yield) {
            const auto now_ticks = detail::monotonic_ticks();
            auto candidate = chosen;
            chosen = threads_.items.end();
            int best = -1;
            auto first_runnable = threads_.items.end();
            for (std::size_t n = 0; n < threads_.items.size(); ++n, ++candidate) {
                if (candidate == threads_.items.end())
                    candidate = threads_.items.begin();
                const auto &thread = candidate->second;
                if (thread.status != ThreadStatus::Runnable || thread.suspended)
                    continue;
                if (first_runnable == threads_.items.end())
                    first_runnable = candidate;
                auto priority = thread.effective_priority(runtime_.process_priority_class, runtime_.process_background);
                if (now_ticks - thread.last_dispatch_ticks >= UINT64_C(500000))
                    priority = std::max(priority, 15);
                if (priority >= 15 && priority > best) {
                    best = priority;
                    chosen = candidate;
                }
            }
            if (chosen == threads_.items.end())
                chosen = first_runnable;
        } else
            for (std::size_t n = 0; n < threads_.items.size(); ++n) {
                if (chosen == threads_.items.end())
                    chosen = threads_.items.begin();
                if (chosen->second.status == ThreadStatus::Runnable && !chosen->second.suspended)
                    break;
                ++chosen;
            }

#ifdef X64ARM_HOST_SERVICES_RESIDENT_PARALLEL
        if (resident_execution_depth_ && parallel_workers() > 1) {
            // 完整控制待辦（API／語義／故障）由協調者先承接；不能先啟動
            // 少數可運算客體，再讓其餘客體空等下一個排程期限。
            // 待辦之間依有效優先序選取，同優先序保留輪替，沿下方唯一的 TLS／回呼／例外切換流程；
            // worker 可完成的服務保持平行，不在這裡搶回協調者。
            auto request = threads_.items.upper_bound(threads_.current);
            int request_priority=0;
            for (std::size_t n = 0; n < threads_.items.size(); ++n) {
                if (request == threads_.items.end())request = threads_.items.begin();
                const auto& thread = request->second;
                const auto pending=resident_service_requests_.find(request->first);
                if (thread.status == ThreadStatus::Runnable && !thread.suspended &&
                    ((pending!=resident_service_requests_.end() && pending->second==thread.context.rip) ||
                     (handles(thread.context.rip) && !parallel_service(thread.context.rip) &&
                      !resident_recipe_eligible(*this,thread.context.rip)))) {
                    const auto priority=thread.effective_priority(runtime_.process_priority_class,runtime_.process_background);
                    if(priority>request_priority){chosen=request;request_priority=priority;}
                }
                ++request;
            }
        }
#endif
        // 物件／I/O 剛喚醒的高優先序工作須先取得一次執行機會；不能讓
        // MMCSS 音訊通知排在所有普通 runnable 後面。等優先序仍沿原輪替順序，
        // dispatch 後消耗 wake_pending；不能借用可被關閉、且不適用即時工作的 boost。
        auto awakened = threads_.items.end();
        int awakened_priority = 0;
        auto candidate = threads_.items.upper_bound(threads_.current);
        for(std::size_t n=0;n<threads_.items.size();++n){
            if(candidate==threads_.items.end())candidate=threads_.items.begin();
            const auto& thread=candidate->second;
            if(thread.status==ThreadStatus::Runnable && !thread.suspended && thread.wake_pending){
                const auto priority=thread.effective_priority(runtime_.process_priority_class,runtime_.process_background);
                if(priority>awakened_priority){awakened=candidate;awakened_priority=priority;}
            }
            ++candidate;
        }
        if(awakened!=threads_.items.end() && (chosen==threads_.items.end() ||
           awakened_priority>chosen->second.effective_priority(runtime_.process_priority_class,runtime_.process_background)))
            chosen=awakened;
        // 只投遞事件而未喚醒更高優先序工作時，沿用目前執行緒尚未到期的時間片。
        std::uint64_t kept_slice = UINT64_MAX;
        if (std::exchange(threads_.interrupt_poll, false)) {
            const auto current = threads_.items.find(threads_.current);
            if (current != threads_.items.end() && current->second.status == ThreadStatus::Runnable &&
                !current->second.suspended && threads_.slice_deadline != UINT64_MAX &&
                detail::monotonic_ticks() < threads_.slice_deadline &&
                (awakened==threads_.items.end() || awakened_priority<=
                 current->second.effective_priority(runtime_.process_priority_class,runtime_.process_background))) {
                chosen = current;
                kept_slice = threads_.slice_deadline;
            }
        }
        if (chosen != threads_.items.end() && chosen->second.status == ThreadStatus::Runnable &&
            !chosen->second.suspended) {
            // 同一執行緒續跑時，runtime 仍是有效的 TLS／FLS；只在真正換人前保存。
            // wake_threads 與時間／I/O 輪詢照常執行，不能以單一 runnable 提早略過。
#if !defined(_WIN32)
            if (idle_tracing && idle_active) {
                const auto elapsed = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - idle_start).count());
                ++idle_trace.periods;
                idle_trace.nanoseconds += elapsed;
                idle_trace.longest_ns = std::max(idle_trace.longest_ns, elapsed);
                if (wake_epoch != idle_epoch) {
                    ++idle_trace.signalled;
                    idle_trace.signalled_ns += elapsed;
                } else {
                    ++idle_trace.timer;
                    idle_trace.timer_ns += elapsed;
                }
                ++idle_trace.buckets[elapsed < 100'000 ? 0 : elapsed < 500'000 ? 1 : elapsed < 1'000'000 ? 2 : elapsed < 5'000'000 ? 3 : elapsed < 20'000'000 ? 4 : 5];
                if (auto* timeline = idle_timeline_file(); timeline && elapsed >= 50'000) {
                    const auto begin = idle_timeline_unix_us(idle_start);
                    const auto prefix=timeline==stderr?"timeline pid="+std::to_string(::getpid())+" ":std::string{};
                    std::fprintf(timeline, "%s%lld %lld\n", prefix.c_str(),begin, begin + static_cast<long long>(elapsed / 1000));
                }
                idle_active = false;
            }
#endif
#if !defined(_WIN32)
            trace_run_switch(threads_.items, chosen->first);
#endif
            const bool switching = chosen->first != threads_.current;
            if (switching) {
                source.fls_values = runtime_.fls_values;
                source.tls_values = runtime_.tls_values;
            }
            threads_.current = chosen->first;
            auto &target = chosen->second;
            state = target.context;
            state.tsc = tsc;
            const auto mask =
                target.affinity &
                (runtime_.processor_count == 64 ? UINT64_MAX : (UINT64_C(1) << runtime_.processor_count) - 1);
            if (!(mask & (UINT64_C(1) << state.processor_id)))
                state.processor_id = static_cast<std::uint32_t>(std::countr_zero(mask));
            runtime_.callbacks = std::move(target.callbacks);
            runtime_.sorts = std::move(target.sorts);
            exceptions_.stack = std::move(target.exceptions);
            if (switching) {
                runtime_.fls_values = target.fls_values;
                runtime_.tls_values = target.tls_values;
            }
            runtime_.errno_cell = target.errno_cell;
            last_error_ = static_cast<std::uint32_t>(memory.load(GuestAddress{target.teb + (x86_ ? 0x34 : 0x68)}, 32));
            // 客體執行緒以基底優先序分配時間片；保留輪替，避免原生 callback 被忙碌客體餓死。
            const auto priority=target.effective_priority(runtime_.process_priority_class,runtime_.process_background);
            threads_.remaining =
                resident_execution_depth_?0:
                runtime_.host_clock && clock_scheduled_slices()?UINT64_MAX:
                (runtime_.host_clock && adaptive_scheduler_budget()?threads_.host_instruction_budget.limit/8:
                    std::uint64_t{runtime_.host_clock ? scheduler_quantum() : 512U}) * static_cast<unsigned>(priority);
            // 一般優先序以 2 ms 作為到期門檻；仍按優先序加權。retired 及 API 入口共用單調時鐘，
            // 大原生批次返回後即讓出到期切片，慢直譯也不能只等長指令預算耗盡。
            const auto span = UINT64_C(2500) * static_cast<unsigned>(priority);
            // 常駐執行也使用同一時間片：只在服務／語義邊界比較，不進入生成碼。常駐模式的
            // clock_ticks 可能尚未同步，直接以單調時鐘起算；溢位時保留 UINT64_MAX（下次邊界即到期）。
            const auto slice_start = resident_execution_depth_ ? detail::monotonic_ticks() : runtime_.clock_ticks;
            threads_.slice_deadline = runtime_.host_clock && slice_start <= UINT64_MAX - span - 1
                                          ? slice_start + span
                                          : UINT64_MAX;
            if (kept_slice != UINT64_MAX)
                threads_.slice_deadline = kept_slice;
            target.wake_pending=false;
            target.dynamic_priority_boost = 0;
            target.last_dispatch_ticks = detail::monotonic_ticks();
            threads_.yield = false;
            prepare_debug_execution(state, memory);
            if(AdmissionTrace::enabled())trace_provider_admission(*this,threads_,resident_service_requests_,runtime_.processor_count,
                runtime_.process_priority_class,runtime_.process_background,parallel_cursor_,debugger_stopped(),3);
            return;
        }
#if !defined(_WIN32)
        if (idle_tracing && !idle_active) {
            trace_run_switch(threads_.items, 0);
            idle_active = true;
            idle_start = std::chrono::steady_clock::now();
            idle_epoch = wake_epoch;
        }
#endif
#if !defined(_WIN32)
        // 診斷：X64ARM_TRACE_IDLE_DUMP=<目錄> 時，程序內所有客體執行緒連續閒置超過 2 秒就傾印一次
        // 各執行緒的等待種類、handle 物件狀態、期限與客體返回位址；只讀排程狀態，未設定時只多一次旗標檢查。
        // X64ARM_TRACE_IDLE_DUMP_INTERVAL_MS=<N> 時改為每次進入閒置、距上次傾印至少 N ms 就傾印，不要求連續閒置
        // （有短週期計時器的程序永遠不會連續閒置 2 秒，卻仍可能在等待中錯過通知）。
        static const char* const idle_dump_directory=std::getenv("X64ARM_TRACE_IDLE_DUMP");
        if(idle_dump_directory){
            static const long long idle_dump_interval=[] {
                const auto* value=std::getenv("X64ARM_TRACE_IDLE_DUMP_INTERVAL_MS");return value && *value?std::atoll(value):0LL;}();
            static std::chrono::steady_clock::time_point idle_dump_last{};
            const auto now=std::chrono::steady_clock::now();
            if(idle_dump_since==std::chrono::steady_clock::time_point{})idle_dump_since=now;
            const bool periodic=idle_dump_interval>0 && now-idle_dump_last>=std::chrono::milliseconds(idle_dump_interval);
            if(periodic || (!idle_dumped && now-idle_dump_since>std::chrono::seconds(2))){
                idle_dumped=true;idle_dump_last=now;
                const auto path=std::string(idle_dump_directory)+"/idle-"+std::to_string(::getpid())+".log";
                if(auto* out=std::fopen(path.c_str(),"a")){
                    std::fprintf(out,"idle_dump unix_ms=%lld clock=%llu current=%llu threads=%zu wake_epoch=%llu\n",
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()),
                        static_cast<unsigned long long>(runtime_.clock_ticks),static_cast<unsigned long long>(threads_.current),
                        threads_.items.size(),static_cast<unsigned long long>(wake_epoch));
                    for(const auto& [id,thread]:threads_.items){
                        std::fprintf(out,"  id=%llu status=%d suspended=%d kind=%d deadline=%llu entry=0x%llx name=%s\n",
                            static_cast<unsigned long long>(id),static_cast<int>(thread.status),thread.suspended?1:0,
                            static_cast<int>(thread.wait.kind),static_cast<unsigned long long>(thread.wait.deadline),
                            static_cast<unsigned long long>(thread.entry),utf16_to_utf8(thread.description).c_str());
                        if(thread.status==ThreadStatus::Waiting && thread.wait.kind==ThreadWaitKind::GuiInput){
                            // 訊息等待的篩選條件與此執行緒佇列中的訊息（是否符合、是否已被 Peek 看過）。
                            const auto& w=thread.wait;std::size_t queued=0,matching=0,seen=0;
                            for(const auto& message:gui_.queue){
                                if(message.thread!=id && !(message.window && gui_.windows.contains(message.window) && gui_.windows.at(message.window).thread==id))continue;
                                ++queued;if(message.queue_seen)++seen;
                                if(gui_message_matches(message,id,w.gui_window,w.gui_min,w.gui_max) && (gui_queue_category(message)&w.gui_mask))++matching;
                            }
                            std::fprintf(out,"    gui window=0x%llx min=0x%x max=0x%x mask=0x%x new_only=%d queued=%zu matching=%zu seen=%zu total=%zu\n",
                                static_cast<unsigned long long>(w.gui_window),w.gui_min,w.gui_max,w.gui_mask,w.gui_new_only?1:0,queued,matching,seen,gui_.queue.size());
                        }
                        for(const auto handle:thread.wait.handles){
                            const auto object=runtime_.wait_objects.find(handle);
                            if(object==runtime_.wait_objects.end()){std::fprintf(out,"    handle=0x%llx (非等待物件)\n",static_cast<unsigned long long>(handle));continue;}
                            const auto& state_object=runtime_.wait_state(object->second);
                            std::fprintf(out,"    handle=0x%llx kind=%d signaled=%d manual=%d count=%u shared_fd=%d name=%s\n",
                                static_cast<unsigned long long>(handle),static_cast<int>(object->second.kind),state_object.signaled()?1:0,
                                state_object.manual_reset?1:0,state_object.count,runtime_.wait_lock_descriptor(object->second),
                                utf16_to_utf8(object->second.name).c_str());
                        }
                        if(thread.status!=ThreadStatus::Exited){
                            const auto& walk=id==threads_.current?state:thread.context;
                            std::fprintf(out,"    rip=0x%llx api=%s",static_cast<unsigned long long>(walk.rip.value),
                                handles(walk.rip)?std::string(name(walk.rip)).c_str():"");
                            const auto sp=walk.reg(Gpr::Rsp);const unsigned width=x86_?4U:8U;
                            for(unsigned n=0;n<6;++n)try{std::fprintf(out," [sp+%u]=0x%llx",n*width,
                                static_cast<unsigned long long>(memory.load(GuestAddress{sp+n*width},static_cast<std::uint8_t>(width*8))));}catch(const MemoryFault&){break;}
                            std::fputc('\n',out);
                        }
                    }
                    std::fclose(out);
                }
            }
        }
#endif
        auto next = UINT64_MAX;
        next = std::min(next, threadpool_next_deadline());
        next = std::min(next, memory_resource_poll_deadline_);
        next = std::min(next, etw_next_deadline());
        next = std::min(next, machine_shutdown_deadline());
        for (const auto &[id, track] : gui_.mouse_tracking) {
            const auto thread = threads_.items.find(id);
            if ((track.flags & 1) && thread != threads_.items.end() && thread->second.status == ThreadStatus::Waiting &&
                thread->second.wait.kind == ThreadWaitKind::GuiInput)
                next = std::min(next, track.deadline);
        }
        for (const auto &[handle, timer] : queue_timers_) {
            (void)handle;
            if (!timer_workers_exhausted_ && !timer.closing && !timer.expired)
                next = std::min(next, timer.deadline);
        }
        for (const auto &[id, thread] : threads_.items)
            if (thread.status == ThreadStatus::Waiting && !thread.suspended) {
                (void)id;
                next = std::min(next, thread.wait.deadline);
                for (const auto handle : thread.wait.handles) {
                    const auto timer = runtime_.wait_objects.find(handle);
                    if (timer != runtime_.wait_objects.end() && (timer->second.kind == WaitObject::Kind::Timer || timer->second.kind == WaitObject::Kind::MemoryResource) &&
                        runtime_.wait_state(timer->second).active)
                        next = std::min(next, runtime_.wait_state(timer->second).due);
                }
            }
        const bool console_wait = std::any_of(threads_.items.begin(), threads_.items.end(), [](const auto &entry) {
            return entry.second.status == ThreadStatus::Waiting && !entry.second.suspended &&
                   entry.second.wait.kind == ThreadWaitKind::ConsoleInput;
        });
        if (console_wait || std::any_of(threads_.items.begin(), threads_.items.end(), [](const auto &entry) {
                return entry.second.status == ThreadStatus::Waiting && !entry.second.suspended &&
                       entry.second.wait.kind == ThreadWaitKind::StandardInput;
            })) {
            const auto timeout =
                next == UINT64_MAX ? 50
                : next <= runtime_.clock_ticks
                    ? 0
                    : static_cast<int>(std::min<std::uint64_t>(50, (next - runtime_.clock_ticks + 9999) / 10000));
            const auto began = std::chrono::steady_clock::now();
            if (console_wait)
                (void)console_input_ready(0x100, static_cast<unsigned>(timeout));
            else
                (void)standard_input_ready(UINT64_MAX, false, false, timeout);
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count();
            // host clock 在下一輪開頭同步；中間沒有客體執行，不重複發布相同時間頁。
            if (!runtime_.host_clock) {
                runtime_.clock_ticks += static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed)) / 100;
                retired(memory, 0);
            }
            continue;
        }
        const bool gui_wait = std::any_of(threads_.items.begin(), threads_.items.end(), [](const auto &entry) {
            return entry.second.status == ThreadStatus::Waiting && !entry.second.suspended &&
                   entry.second.wait.kind == ThreadWaitKind::GuiInput;
        });
        const bool shared_wait = std::any_of(threads_.items.begin(), threads_.items.end(), [&](const auto &entry) {
            return entry.second.status == ThreadStatus::Waiting && !entry.second.suspended &&
                   std::any_of(entry.second.wait.handles.begin(), entry.second.wait.handles.end(), [&](auto handle) {
                       const auto object = runtime_.wait_objects.find(handle);
                       return object != runtime_.wait_objects.end() &&
                              runtime_.wait_lock_descriptor(object->second) >= 0;
                   });
        });
        const bool policy_wait=std::any_of(threads_.items.begin(),threads_.items.end(),[](const auto& entry){
            return entry.second.status==ThreadStatus::Waiting && !entry.second.suspended &&
                entry.second.wait.kind==ThreadWaitKind::Sleep && !entry.second.callbacks.empty() &&
                entry.second.callbacks.back().continuation==Continuation::PolicyRetry;
        });
        // 全部客體執行緒暫停時仍須輪詢 IPC，讓另一程序可以恢復它們。
        // 僅在無 runnable 執行緒的慢路徑檢查，正常執行的排程路徑不增加成本。
        const bool suspended_wait=runtime_.network.external_io && handle_transfers_ &&
            std::any_of(threads_.items.begin(),threads_.items.end(),[](const auto& entry){
                return entry.second.status!=ThreadStatus::Exited && !entry.second.terminating && entry.second.suspended;
            });
        const bool gui_polling =
            (gui_wait || !gui_.status_icons.values().empty() || !gui_.attention.values().empty()) && native_gui_;
        // 管道的 read/write/connect/close 已通知同一 SharedWake；等待到既有 50 ms 存活複查。
        // 原生 GUI 事件等待沒有接入 SharedWake，必須保留短輪詢；其他無通知 I/O 也沿用原路徑。
        const auto file_deadline=file_io_poll_deadline(gui_polling ? nullptr : wake.get());
        if(file_deadline)next=std::min(next,*file_deadline);
        const bool external_wait =
            debugger_attached() || suspended_wait || policy_wait || media_foundation_waiting() || midi_input_waiting() || wave_output_waiting() || dsound_waiting() ||
            wasapi_waiting() || directmusic_waiting() || dinput_waiting() || processes_waiting() || http_waiting() || bluetooth_waiting() || winusb_waiting() ||
            shared_wait || remote_family_waiting() || perflib_remote_waiting() ||
            std::any_of(threads_.items.begin(), threads_.items.end(),
                        [](const auto &item) {
                            return item.second.status == ThreadStatus::Waiting &&
                                   (item.second.wait.kind == ThreadWaitKind::SocketSelect ||
                                    item.second.wait.kind == ThreadWaitKind::HandleTransfer ||
                                    item.second.wait.kind == ThreadWaitKind::ProcessCreation ||
                                    item.second.wait.kind == ThreadWaitKind::RegistryIo ||
                                    item.second.wait.kind == ThreadWaitKind::EtwTrace ||
                                    item.second.wait.kind == ThreadWaitKind::DebugEvent ||
                                    item.second.wait.kind == ThreadWaitKind::CrtProcess);
                        }) ||
            !runtime_.network.resolvers.empty() || !runtime_.network.pending_lookups.empty() || !runtime_.network.pending_accepts.empty() ||
            !runtime_.network.pending_socket_io.empty() || !gui_.socket_watches.empty() || !file_deadline ||
            !change_notifications_.empty() || address_notification_waiting() ||
            (!registry_file_.empty() && !registry_notifications_.empty()) || !guest_service_waits_.empty();
        if (runtime_.host_clock && external_wait && external_wait_spin_ticks()) {
            if (!spin_deadline)
                spin_deadline = runtime_.clock_ticks + external_wait_spin_ticks();
            if (runtime_.clock_ticks < spin_deadline) {
                // 只縮短一次交接的休眠延遲；wake_threads／期限檢查仍每輪執行。
                // 長等待耗盡有界輪詢後仍沿用原本的退讓，不改固定時鐘。
                if (gui_wait && native_gui_)
                    pump_gui(memory, false, 0);
                std::this_thread::yield();
                continue;
            }
        }
        if (gui_polling) {
            next = std::min(next, gui_.attention.deadline());
            const auto until_deadline = next > runtime_.clock_ticks ? next - runtime_.clock_ticks : 0;
            const auto ticks = std::min(until_deadline, external_wait ? external_poll_ticks : UINT64_C(500000));
            if (wake && external_wait) {
                WakePollScope poll_measurement("pump_gui");
        pump_gui(memory, false);
                if (ticks && !wake->wait_for(wake_epoch, ticks * 100))
                    std::this_thread::sleep_for(std::chrono::nanoseconds(ticks * 100));
            } else
                pump_gui(memory, true, ticks);
            external_poll_ticks =
                std::min(runtime_.host_clock ? external_wait_poll_limit() : UINT64_C(10000), external_poll_ticks * 2);
            continue;
        }
        const bool console_object_wait =
            console_handle(0x100) && std::any_of(threads_.items.begin(), threads_.items.end(), [](const auto &entry) {
                return entry.second.status == ThreadStatus::Waiting &&
                       entry.second.wait.kind == ThreadWaitKind::Objects &&
                       std::find(entry.second.wait.handles.begin(), entry.second.wait.handles.end(), 0x100) !=
                           entry.second.wait.handles.end();
            });
        if (console_object_wait)
            (void)console_input_ready(0x100, 1);
        if (external_wait || console_object_wait || !power_notifications_.empty()) {
            const auto until_deadline = next > runtime_.clock_ticks ? next - runtime_.clock_ticks : 0;
            const auto ticks = runtime_.host_clock ? std::min(until_deadline, external_poll_ticks) : UINT64_C(10000);
            if (ticks && (!wake || !wake->wait_for(wake_epoch, ticks * 100)))
                std::this_thread::sleep_for(std::chrono::nanoseconds(ticks * 100));
            external_poll_ticks =
                std::min(runtime_.host_clock ? external_wait_poll_limit() : UINT64_C(10000), external_poll_ticks * 2);
            if (!runtime_.host_clock) {
                runtime_.clock_ticks += 10000;
                retired(memory, 0);
            }
            continue;
        }
        if (next == UINT64_MAX) {
            // 通道有監看執行緒時，其他程序仍可經 handle 傳輸喚醒、恢復或結束本程序：
            // 沒有本地期限就在共用喚醒上等待，不輪詢也不視為死結。
            if (!wake || !handle_transfers_ || handle_transfer_polling())
                throw HostCallFault("客體執行緒全部等待，且沒有可觸發的計時器：排程死結");
            if (!wake->wait_for(wake_epoch, UINT64_C(50'000'000)))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (runtime_.host_clock) {
            // 真實時間不能跳至 deadline；在共用喚醒上等待到期限（上限 50 ms），
            // 通道或映射事件會提前喚醒，沒有共用喚醒時才純休眠。
            if (next > runtime_.clock_ticks) {
                const auto ticks = std::min<std::uint64_t>(next - runtime_.clock_ticks, 500000);
                if (!wake || !wake->wait_for(wake_epoch, ticks * 100))
                    std::this_thread::sleep_for(std::chrono::nanoseconds(ticks * 100));
            }
        } else {
            runtime_.clock_ticks = std::max(runtime_.clock_ticks, next);
            retired(memory, 0);
        }
    }
}
std::optional<std::uint32_t> WindowsServices::force_finish_thread(GuestState &state, GuestMemory &memory) {
    auto &thread = threads_.items.at(threads_.current);
    if (!thread.forced_stop)
        throw HostCallFault("強制結束入口缺少終止要求");
    const auto id = threads_.current;
    // 協調執行緒已停止所有 worker；取消此客體執行緒的 I/O 並交付 OVERLAPPED／IOCP 完成。
    for (auto &io : pending_file_io_)
        if (io.thread == id && !io.done)
            io.result = {0, 995};
    poll_file_io(memory);
    std::erase_if(pending_file_io_, [&](const auto &io) { return io.thread == id; });
    runtime_.fls_values.fill(0);
    sync_current_fls_mirror(memory);
    runtime_.tls_values.fill(0);
    thread.apcs.clear();
    thread.wait = {};
    exceptions_.stack.clear();
    (void)revoke_com_classes(memory, id, false);
    (void)retire_com_context(state, memory, id, false);
    (void)retire_com_error(state, memory, id, false);
    (void)retire_ole_clipboard(state, memory, id, false);
    gui_.ole_initializations.erase(id);
    gui_.com_initializations.erase(id);
    if(gui_.com_main_sta==id)gui_.com_main_sta=0;
    state.pending_cpu_trap = 0;
    return finish_thread(state, memory, thread.exit_code);
}
std::optional<std::uint32_t> WindowsServices::finish_thread(GuestState &state, GuestMemory &memory,
                                                            std::uint32_t code) {
    // 正常 ThreadExit continuation 已執行並移除 FLS/TLS callback；此時才清除
    // 被 ExitThread 跨越的外層 callback，歸還混合 A/W hook 等暫存與 loader ownership。
    cancel_network_io(memory, 0, threads_.current);
    cancel_duplicate_remote_handle(memory,threads_.current);
    cancel_guest_process_creation(threads_.current);
    discard_file_transfers(state,memory,threads_.current);
    if(const auto job=runtime_.network.resolvers.find(threads_.current);job!=runtime_.network.resolvers.end()){
        job->second->cancelled.store(true);runtime_.network.resolvers.erase(job);
    }
    autoproxy_.erase(UINT64_MAX - threads_.current);
    // 舊式 Winsock 查詢結果由同一客體執行緒持有，結束時一起回收。
    for (const auto slot :
         {threads_.current, threads_.current + UINT64_C(0x200000000), threads_.current + UINT64_C(0x300000000)})
        if (const auto found = runtime_.network.service_buffers.find(slot);
            found != runtime_.network.service_buffers.end()) {
            if (found->second)
                (void)free_heap(memory, found->second, state);
            runtime_.network.service_buffers.erase(found);
        }
    for (auto &io : pending_file_io_)
        if (io.thread == threads_.current && !io.done)
            io.result = {0, 995};
    poll_file_io(memory);
    std::erase_if(pending_file_io_, [&](const auto &io) { return io.thread == threads_.current; });
    discard_callbacks(0, state, memory);
    discard_crt_io_locks(memory,threads_.current);
    if(const auto locale=runtime_.crt_thread_locales.find(threads_.current);locale!=runtime_.crt_thread_locales.end()){
        const auto address=locale->second;runtime_.crt_thread_locales.erase(locale);
        runtime_.crt_locales.erase(address);(void)free_heap(memory,address,state);
    }
    if(const auto query=runtime_.crt_locale_query_buffers.find(threads_.current);query!=runtime_.crt_locale_query_buffers.end()){
        const auto address=query->second;runtime_.crt_locale_query_buffers.erase(query);(void)free_heap(memory,address,state);
    }
    cleanup_etw_thread(state, memory, threads_.current);
    cleanup_perflib_thread(state,memory,threads_.current);
    cancel_http_connection_probe(threads_.current);
    cancel_bluetooth_thread(state,memory,threads_.current);
    discard_policy_thread(threads_.current);
    thread_power_requests_.erase(threads_.current);
    (void)retire_com_error(state, memory, threads_.current, false);
    (void)retire_ole_drag_thread(state, memory, threads_.current, false);
    (void)retire_com_proxy_thread(state, memory, threads_.current, false);
    (void)retire_com_marshal(memory, threads_.current, false);
    auto &thread = threads_.items.at(threads_.current);
    thread.status = ThreadStatus::Exited;
    if(thread.msvc_exception_cells){(void)free_heap(memory,thread.msvc_exception_cells,state);thread.msvc_exception_cells=0;}
    if(thread.msvc_exception_area){(void)free_heap(memory,thread.msvc_exception_area,state);thread.msvc_exception_area=0;}
    thread.exit_code = code;
    publish_thread_lifetime(threads_.current,code);
    thread.impersonation_token = 0;
    collect_tokens();
    for (auto &[identity, mutex] : runtime_.mutex_objects) {
        (void)identity;
        if (mutex.shared)
            mutex.shared->abandon(threads_.current);
        else if (mutex.owner == threads_.current) {
            mutex.owner = 0;
            mutex.recursion = 0;
            mutex.abandoned = true;
        }
    }
    std::erase_if(runtime_.mutex_objects, [&](const auto &entry) {
        return std::none_of(runtime_.wait_objects.begin(), runtime_.wait_objects.end(), [&](const auto &object) {
            return object.second.kind == WaitObject::Kind::Mutex && object.second.mutex == entry.first;
        });
    });
    if (const auto worker = wait_workers_.find(threads_.current); worker != wait_workers_.end()) {
        if (worker->second)
            release_registered_wait(worker->second);
        wait_workers_.erase(threads_.current);
    }
    cancel_gui_thread(threads_.current, memory, state);
    if (const auto tm = runtime_.tm_buffers.find(threads_.current); tm != runtime_.tm_buffers.end()) {
        (void)free_heap(memory, tm->second, state);
        runtime_.tm_buffers.erase(tm);
    }
    const bool last = std::all_of(threads_.items.begin(), threads_.items.end(),
                                  [](const auto &entry) { return entry.second.status == ThreadStatus::Exited; });
    if (last) {
        record_job_exit(code);
        (void)emit_debug_event(state, memory, debugger::ExitProcess{code});
        return code;
    }
    (void)emit_debug_event(state, memory, debugger::ExitThread{code});
    const auto retired_id=threads_.current,retired_stack=thread.stack_base,retired_size=thread.stack_size;
    const auto retired_teb=thread.teb,retired_exceptions=thread.exception_area;
    const auto fiber = thread.fiber, stack = thread.fiber_stack_allocation;
    thread.fiber = 0;
    thread.fiber_stack_allocation = 0;
    if (fiber)
        threads_.fibers.at(fiber).active_thread = 0;
    schedule(state, memory);
    if (fiber)
        destroy_fiber(state, memory, fiber);
    if (stack)
        (void)free_heap(memory, stack, state);
    // thread object／退出碼仍由 handle 保留；執行用記憶體在切離原堆疊後回收。
    // 正常返回、ExitThread 與 TerminateThread 共用此流程，避免累積短命 worker。
    if(retired_id!=1){
        if(retired_size && memory.mapped(GuestAddress{retired_stack}))memory.unmap(GuestAddress{retired_stack},static_cast<std::size_t>(retired_size));
        runtime_.virtual_page_protections.erase(runtime_.virtual_page_protections.lower_bound(retired_stack),runtime_.virtual_page_protections.lower_bound(retired_stack+retired_size));
        if(retired_teb && memory.mapped(GuestAddress{retired_teb}))memory.unmap(GuestAddress{retired_teb},x86_?0x4000:GUEST_CRT_TEB_SIZE);
        if(retired_exceptions && memory.mapped(GuestAddress{retired_exceptions}))memory.unmap(GuestAddress{retired_exceptions},0x20000);
        thread.stack_size=0;thread.teb=0;thread.exception_area=0;thread.errno_cell=0;
    }
    return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::exit_thread(GuestState &state, GuestMemory &memory, std::uint32_t code) {
    // 保留原服務／thread_return_ 狀態，等持有者完成後才能建立 detach 清單。
    auto &exiting = threads_.items.at(threads_.current);
    exiting.terminating = true;
    exiting.apcs.clear();
    if (await_loader(state, memory))
        return std::nullopt;
    std::vector<GuestCallback> callbacks;
    for (const auto &[handle, window] : gui_.windows)
        if (window.thread == threads_.current) {
            auto releases = revoke_gui_drop_targets(memory, handle);
            callbacks.insert(callbacks.end(), releases.begin(), releases.end());
        }
    auto message_filter = revoke_com_message_filter(memory, threads_.current);
    callbacks.insert(callbacks.end(), message_filter.begin(), message_filter.end());
    auto clipboard = retire_ole_clipboard(state, memory, threads_.current);
    callbacks.insert(callbacks.end(), clipboard.begin(), clipboard.end());
    auto classes = revoke_com_classes(memory, threads_.current);
    callbacks.insert(callbacks.end(), classes.begin(), classes.end());
    auto context = retire_com_context(state, memory, threads_.current);
    callbacks.insert(callbacks.end(), context.begin(), context.end());
    auto drag_releases = retire_ole_drag_thread(state, memory, threads_.current, true);
    callbacks.insert(callbacks.end(), drag_releases.begin(), drag_releases.end());
    auto proxies = retire_com_proxy_thread(state, memory, threads_.current, true);
    callbacks.insert(callbacks.end(), proxies.begin(), proxies.end());
    auto marshaling = retire_com_marshal(memory, threads_.current, true);
    callbacks.insert(callbacks.end(), marshaling.begin(), marshaling.end());
    gui_.ole_initializations.erase(threads_.current);
    gui_.com_initializations.erase(threads_.current);
    if(gui_.com_main_sta==threads_.current)gui_.com_main_sta=0;
    for (std::size_t i = 0; i < runtime_.fls_used.size(); ++i) {
        if (runtime_.fls_used[i] && runtime_.fls_callbacks[i] && runtime_.fls_values[i])
            callbacks.push_back({runtime_.fls_callbacks[i], {runtime_.fls_values[i]}});
        runtime_.fls_values[i] = 0;
    }
    sync_current_fls_mirror(memory);
    for (const auto callback : runtime_.image.tls_callbacks)
        callbacks.push_back({callback, {runtime_.image.base, 3, 0}, true});
    auto module_callbacks = module_thread_callbacks(3);
    callbacks.insert(callbacks.end(), module_callbacks.begin(), module_callbacks.end());
    callbacks.push_back({resolve("internal-com-error", "CleanupThread").value, {}});
    if (!callbacks.empty()) {
        start_loader_callbacks(state, memory, std::move(callbacks), Continuation::ThreadExit, code);
        return std::nullopt;
    }
    return finish_thread(state, memory, code);
}
std::uint64_t WindowsServices::create_guest_thread(const GuestState &state, GuestMemory &memory, std::uint64_t entry,
                                                   std::uint64_t parameter, std::uint64_t requested,
                                                   std::uint32_t flags_value) {
    // 建立數量由可用記憶體限制，不能把仍有 handle 的已退出物件當成執行名額。
    if (threads_.next_id>UINT32_MAX) {
        last_error_ = 8;
        return 0;
    }
    const auto unaligned_size=std::max(UINT64_C(65536), requested ? requested : runtime_.image.stack_size);
    if(unaligned_size>UINT64_MAX-65535){last_error_=8;return 0;}
    const auto stack_size = (unaligned_size + 65535) & ~UINT64_C(65535);
    if (stack_size > memory.remaining_capacity() || (x86_?0x24000U:0x20000U+GUEST_CRT_TEB_SIZE) > memory.remaining_capacity()-stack_size) {
        last_error_ = 8;
        return 0;
    } else {
        const auto id = threads_.next_id;
        auto base = UINT64_C(0x3010000000) + id * 0x1000000; // x64 TEB／堆疊區塊位於 MMU 視窗內。
        if (x86_) {
            base = 0x60000000;
            while (base + 0x100000 + stack_size <= 0x70000000 &&
                   !memory.range_available(GuestAddress{base}, 0x100000 + stack_size))
                base += 0x10000;
            if (base + 0x100000 + stack_size > 0x70000000) {
                last_error_ = 8;
                return 0;
            }
        }else if(prefer_low_guest_stack()){
            const auto reserved_end=[&](std::uint64_t first,std::uint64_t end){
                const auto& reservations=runtime_.virtual_regions;
                const auto next=reservations.lower_bound(first);auto high=first;
                if(next!=reservations.begin()){
                    const auto previous=std::prev(next);
                    high=std::max(high,previous->first+previous->second.size);
                }
                if(next!=reservations.end() && next->first<end)high=std::max(high,next->first+next->second.size);
                return high;
            };
            if(const auto low=find_guest_address_gap(memory,UINT64_C(0x60000000),0x100000+stack_size,
                                                    UINT64_C(0x100000000),reserved_end))base=low->value;
        }
        GuestThread thread;
        thread.desktop_handle = threads_.items.at(threads_.current).desktop_handle;
        thread.affinity = runtime_.process_affinity;
        thread.context.segments = state.segments;
        thread.teb = base;
        thread.errno_cell = base + (x86_?0x3ff0:GUEST_CRT_ERRNO_OFFSET);
        thread.exception_area = base + 0x10000;
        thread.security_descriptor = synchronization_descriptor(GuestMemory{}, 0).bytes;
        thread.pseudo_access = created_thread_pseudo_access(thread.security_descriptor);
        thread.public_id=allocate_thread_identifier(id);
        thread.stack_base = base + 0x100000;
        thread.stack_size = stack_size;
        thread.entry = entry;
        thread.suspended = (flags_value & 4) ? 1 : 0;
        memory.map(GuestAddress{base}, x86_?0x4000:GUEST_CRT_TEB_SIZE, read_write);
        memory.map(GuestAddress{thread.exception_area}, 0x20000, read_write);
        memory.map(GuestAddress{thread.stack_base}, static_cast<std::size_t>(stack_size), read_write);
        initialize_stack_guard(memory,thread.stack_base,stack_size,thread.stack_guard);
        const auto stack_top = thread.stack_base + stack_size;
        thread.context.rip = thread_start_;
        const std::uint8_t width = x86_ ? 32 : 64;
        const unsigned unit = x86_ ? 4 : 8;
        if (x86_) {
            thread.context.fs_base = base;
            thread.context.reg(Gpr::Rsp) = stack_top - 8;
            memory.store(GuestAddress{stack_top - 8}, 32, thread_return_.value);
            memory.store(GuestAddress{stack_top - 4}, 32, parameter);
            memory.store(GuestAddress{base}, 32, UINT32_MAX);
        } else {
            thread.context.gs_base = base;
            thread.context.reg(Gpr::Rsp) = stack_top - 40;
            thread.context.reg(Gpr::Rcx) = parameter;
            memory.store(GuestAddress{stack_top - 40}, 64, thread_return_.value);
        }
        memory.store(GuestAddress{base + unit}, width, stack_top);
        memory.store(GuestAddress{base + 2 * unit}, width, thread.stack_guard.limit(thread.stack_base));
        memory.store(GuestAddress{base + (x86_ ? 0x18 : 0x30)}, width, base);
        memory.store(GuestAddress{base + (x86_ ? 0x20 : 0x40)}, width, process_id());
        memory.store(GuestAddress{base + (x86_ ? 0x24 : 0x48)}, width, thread.public_id);
        memory.store(GuestAddress{base + (x86_ ? 0x30 : 0x60)}, width, peb_base_.value);
        memory.store(GuestAddress{base + (x86_ ? 0xf94 : 0x1780)}, width, base + 0x2000);
        const auto tls_vector = runtime_allocate(memory, {}, 64 * unit);
        memory.store(GuestAddress{base + (x86_ ? 0x2c : 0x58)}, width, tls_vector);
        if (runtime_.image.tls_index) {
            const auto tls =
                runtime_allocate(memory, runtime_.image.tls_data,
                                 std::max<std::size_t>(1, runtime_.image.tls_data.size() + runtime_.image.tls_zero),
                                 runtime_.image.tls_alignment);
            memory.store(GuestAddress{tls_vector}, width, tls);
        }
        initialize_thread_module_tls(memory, tls_vector);
        threads_.items.emplace(id, std::move(thread));
        sync_crt_thread_state(memory,id);
        sync_fls_mirror(memory, base, threads_.items.at(id).fls_values); // 新 TEB：全零值與目前的使用中位元圖。
        ++threads_.next_id;
        configure_thread_record(id);
        const auto result = runtime_.allocate_wait_handle();
        runtime_.process_thread_handles.emplace(result, std::pair{id, false});
        queue_debug_thread(id);
        // 新增可執行執行緒後建立排程邊界，讓它有機會在下一個耗時宿主呼叫前啟動。
        // CREATE_SUSPENDED 仍維持暫停，不要求切換。
        if (!(flags_value & 4))
            threads_.yield = true;
        return result;
    }
}
bool WindowsServices::dispatch_user_apc(GuestState &state, GuestMemory &memory, std::uint64_t completion) {
    auto &queue = threads_.items.at(threads_.current).apcs;
    if (queue.empty())
        return false;
    // 一次取一個，讓回呼內的可警示等待也能依序派送尚在佇列中的 APC。
    auto callback = std::move(queue.front());
    queue.erase(queue.begin());
    start_callbacks(state, memory, {std::move(callback)}, Continuation::UserApc, completion);
    return true;
}
std::optional<std::uint32_t> WindowsServices::invoke_thread(GuestState &state, GuestMemory &memory,
                                                            const WindowsServiceDescriptor &service) {
    if(service.symbol=="Beep")return invoke_wave_output(state,memory,service);
    if (service.symbol == "FreeLibraryAndExitThread")
        return invoke_module(state, memory, service);
    const WindowsCall arg(state, memory, service);
    if(service.family=="avrt.dll"){
        const auto done=[&](std::uint64_t value,std::uint32_t error=0){if(error)last_error_=error;arg.return_to(state,value);return std::optional<std::uint32_t>{};};
        auto& task=threads_.items.at(threads_.current).multimedia;
        try{
            if(service.symbol=="AvRevertMmThreadCharacteristics" || service.symbol=="AvSetMmThreadPriority"){
                if(!arg(0) || task.handle!=arg(0))return done(0,6);
                if(service.symbol=="AvRevertMmThreadCharacteristics")task={};
                else {const auto value=static_cast<std::int32_t>(arg(1));if(value < -2 || value > 2)return done(0,87);task.relative=value;}
                threads_.yield=true;return done(1);
            }
            if(!arg(0) || !arg(1))return done(0,87);
            memory.check(GuestAddress{arg(1)},4,Access::Write);
            auto name=service.symbol.ends_with('W')?read_utf16(memory,GuestAddress{arg(0)},256):utf8_to_utf16_lossy(read_ansi(memory,arg(0),256));
            for(auto& c:name)c=unicode_case_unit(c,false);
            if(name.empty() || name.find(u'\\')!=name.npos || name.find(u'/')!=name.npos)return done(0,1550);
            if(task.handle)return done(0,1552);
            const auto requested=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));
            const auto known=threads_.multimedia_task_indices.find(name);
            if(requested && (known==threads_.multimedia_task_indices.end() || known->second!=requested))return done(0,1551);
            std::u16string category=name==u"pro audio"?u"high":name==u"distribution"?u"low":u"medium";
            bool supported=false;
            for(auto standard:{u"audio",u"capture",u"distribution",u"games",u"playback",u"pro audio",u"window manager"})if(name==standard)supported=true;
            RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
            if(transaction.error)return done(0,transaction.error);
            const auto system=std::u16string(1,2)+u"\\software\\microsoft\\windows nt\\currentversion\\multimedia\\systemprofile";
            const auto number=[](const RegistryValue& value)->std::optional<std::uint32_t>{
                if(value.type!=4 || value.bytes.size()!=4)return {};
                std::uint32_t out=0;for(unsigned i=0;i<4;++i)out|=static_cast<std::uint32_t>(value.bytes[i])<<(i*8);return out;
            };
            if(const auto settings=runtime_.registry.keys.find(system);settings!=runtime_.registry.keys.end()){
                if(const auto value=settings->second.find(u"systemresponsiveness");value!=settings->second.end() && number(value->second)==100U)return done(0,1058);
            }
            int priority=2;
            if(const auto settings=runtime_.registry.keys.find(system+u"\\tasks\\"+name);settings!=runtime_.registry.keys.end()){
                supported=true;
                if(const auto value=settings->second.find(u"priority");value!=settings->second.end()){
                    const auto amount=number(value->second);if(!amount || *amount<1 || *amount>8)return done(0,87);priority=static_cast<int>(*amount);
                }
                if(const auto value=settings->second.find(u"scheduling category");value!=settings->second.end()){
                    const auto& data=value->second;if(data.type!=1 || data.bytes.size()%2)return done(0,87);category.clear();
                    for(std::size_t i=0;i<data.bytes.size();i+=2){const auto c=static_cast<char16_t>(data.bytes[i]|(static_cast<unsigned>(data.bytes[i+1])<<8));if(!c)break;category+=unicode_case_unit(c,false);}
                }
            }
            if(!supported)return done(0,1550);
            const int minimum=category==u"high"?23:category==u"medium"?16:category==u"low"?8:0;
            if(!minimum)return done(0,87);
            const int maximum=category==u"high"?26:category==u"medium"?22:15;
            if(category==u"high")priority=2;
            auto index=known==threads_.multimedia_task_indices.end()?0:known->second;
            if(!index){if(threads_.multimedia_task_indices.size()>=4096)return done(0,8);index=static_cast<std::uint32_t>(threads_.multimedia_task_indices.size()+1);threads_.multimedia_task_indices.emplace(name,index);}
            task={runtime_.allocate_wait_handle(),std::clamp(minimum+priority-1,minimum,maximum),minimum,maximum,0};
            memory.store(GuestAddress{arg(1)},32,index);threads_.yield=true;return done(task.handle);
        }catch(const MemoryFault&){return done(0,998);}catch(const std::bad_alloc&){return done(0,8);}catch(const std::invalid_argument&){return done(0,87);}
    }
    if (service.symbol == "SetThreadDescription" || service.symbol == "GetThreadDescription") {
        const bool query = service.symbol == "GetThreadDescription";
        const auto id = thread_id(arg(0));
        const auto finish = [&](std::uint32_t status) {
            arg.return_to(state, status);
            return std::optional<std::uint32_t>{};
        };
        if (!id)
            return finish(0x80070006U);
        if (!(thread_access(arg(0)) & (query ? 0x840U : 0x420U)))
            return finish(0x80070005U);
        if (!arg(1))
            return finish(0x80070057U);
        if (!query) {
            try {
                threads_.items.at(id).description = read_utf16(memory, GuestAddress{arg(1)});
            } catch (const std::invalid_argument &) {
                return finish(0x80070057U);
            } catch (const std::bad_alloc &) {
                return finish(0x8007000eU);
            }
        } else {
            memory.check(GuestAddress{arg(1)}, x86_ ? 4 : 8, Access::Write);
            const auto bytes = utf16_bytes(threads_.items.at(id).description);
            const auto buffer = allocate_heap(memory, bytes.size());
            if (!buffer)
                return finish(0x8007000eU);
            try {
                memory.write(GuestAddress{buffer}, bytes);
                runtime_.local_allocations.insert(buffer);
                memory.store(GuestAddress{arg(1)}, x86_ ? 32 : 64, buffer);
            } catch (...) {
                runtime_.local_allocations.erase(buffer);
                (void)free_heap(memory, buffer, state);
                throw;
            }
        }
        return finish(0);
    }
    if (service.symbol == "GetThreadContext" || service.symbol == "Wow64GetThreadContext" ||
        service.symbol == "SetThreadContext" || service.symbol == "Wow64SetThreadContext" ||
        service.symbol == "SuspendThread" || service.symbol == "Wow64SuspendThread" || service.symbol == "ResumeThread")
        return invoke_thread_control(state, memory, service);
    if ((service.symbol == "GetExitCodeThread" || service.symbol == "GetThreadId") &&
        process_signaled(arg(0)).has_value())
        return invoke_process(state, memory, service);
    // CRT 包裝保留 cdecl 框架，初始化、錯誤與鎖狀態共用 Win32 流程。
    const auto symbol = service.symbol=="__vcrt_InitializeCriticalSectionEx"?std::string_view{"InitializeCriticalSectionEx"}:service.symbol;
    std::uint64_t result = 0;
    const auto u32 = [&](unsigned i) { return static_cast<std::uint32_t>(arg(i)); };
    const auto fail = [&](std::uint32_t error) {
        last_error_ = error;
        result = 0;
    };
    const auto native_deadline = [&](std::uint64_t fallback) {
        if (!runtime_.callbacks.empty()) {
            const auto &callback = runtime_.callbacks.back();
            if (callback.continuation == Continuation::NativeOperation) {
                const auto &operation = native_operations_.at(callback.value);
                if (operation.symbol == "NtWaitForSingleObject" || operation.symbol == "NtSignalAndWaitForSingleObject")
                    return operation.information;
            }
        }
        return fallback;
    };
    if (symbol == "SuspendThread" || symbol == "ResumeThread") {
        std::uint32_t value = 0;
        if (const auto error = process_thread_control(memory, arg(0), symbol == "SuspendThread" ? 2U : 3U, value)) {
            if (*error) {
                last_error_ = *error;
                result = UINT32_MAX;
            } else
                result = value;
            arg.return_to(state, result);
            return std::nullopt;
        }
    }
    if (symbol == "GetThreadContext" || symbol == "SetThreadContext") {
        if (const auto error = process_thread_context(memory, arg(0), arg(1), symbol == "SetThreadContext")) {
            if (*error)
                fail(*error);
            else
                result = 1;
            arg.return_to(state, result);
            return std::nullopt;
        }
    }
    const auto thread_required =
        symbol == "GetThreadId" || symbol == "GetProcessIdOfThread" || symbol == "GetExitCodeThread" || symbol == "QueryThreadCycleTime" ? 0x840U
        : symbol == "SetThreadAffinityMask" || symbol == "SetThreadIdealProcessor"                   ? 0x20U
        : symbol == "SuspendThread" || symbol == "ResumeThread"                                      ? 2U
        : symbol == "GetThreadContext"                                                               ? 8U
        : symbol == "SetThreadContext"                                                               ? 16U
        : symbol == "TerminateThread"                                                                ? 1U
                                                                                                     : 0U;
    if (thread_required && thread_id(arg(0)) && !(thread_access(arg(0)) & thread_required)) {
        last_error_ = 5;
        arg.return_to(state,
                      symbol == "SuspendThread" || symbol == "ResumeThread" || symbol == "SetThreadIdealProcessor"
                          ? UINT32_MAX
                          : 0);
        return std::nullopt;
    }
    auto &current = threads_.items.at(threads_.current);
    if (current.wait.alertable && !current.apcs.empty()) {
        current.wait = {};
        if (dispatch_user_apc(state, memory, symbol == "GetQueuedCompletionStatusEx" ? 0 : 0xc0))
            return std::nullopt;
    }
    if (symbol == "SetThreadInformation" || symbol == "GetThreadInformation") {
        const auto id = thread_id(arg(0));
        const bool query = symbol == "GetThreadInformation";
        if (!id)
            fail(6);
        else if (!(thread_access(arg(0)) & (query ? 0x840U : 0x20U)))
            fail(5);
        else if (u32(1) > (query ? 2U : 3U) || !arg(2) || u32(3) != (u32(1) == 3 ? 12U : 4U))
            fail(87);
        else
            try {
                auto &target = threads_.items.at(id);
                memory.check(GuestAddress{arg(2)}, u32(3), query ? Access::Write : Access::Read);
                if (query) {
                    {
                        const auto priority=target.effective_priority(runtime_.process_priority_class,runtime_.process_background);
                        memory.store(GuestAddress{arg(2)}, 32,
                                     u32(1) == 0                   ? 5U
                                     : u32(1) == 1                 ? static_cast<unsigned>(priority)
                                     : target.dynamic_code_opt_out ? 1U
                                                                   : 0U);
                    }
                    result = 1;
                } else if (u32(1) == 3) {
                    const auto version = memory.load(GuestAddress{arg(2)}, 32);
                    const auto control = static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2) + 4}, 32));
                    const auto enabled = static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2) + 8}, 32));
                    if (version != 1 || (control & ~1U) || (enabled & ~control))
                        fail(87);
                    else {
                        HostExecutionScope probe;
                        const auto policy = !control  ? ExecutionPolicy::Automatic
                                            : enabled ? ExecutionPolicy::Efficient
                                                      : ExecutionPolicy::Responsive;
                        if (!probe.apply(policy))
                            fail(50);
                        else {
                            target.power_control = control;
                            target.power_state = enabled;
                            notify_concurrency_changed();
                            result = 1;
                        }
                    }
                } else if (u32(1) == 2) {
                    const auto enabled = memory.load(GuestAddress{arg(2)}, 32);
                    if (enabled > 1 || id != threads_.current)
                        fail(87);
                    else if (!(runtime_.dynamic_code_policy & 2U))
                        fail(5);
                    else {
                        target.dynamic_code_opt_out = enabled != 0;
                        result = 1;
                    }
                } else if (u32(1) == 1)
                    fail(87); // 此 class 僅可查詢。
                else
                    fail(50); // working-set page aging 尚無正確後端，不能假裝已設定。
            } catch (const MemoryFault &) {
                fail(998);
            }
    } else if (symbol == "QueueUserAPC") {
        const auto id = thread_id(arg(1));
        if (!id)
            fail(6);
        else if (!(thread_access(arg(1)) & 0x10))
            fail(5);
        else if (!arg(0))
            fail(87);
        else {
            auto &target = threads_.items.at(id);
            if (target.status == ThreadStatus::Exited || target.terminating)
                fail(31);
            else if (target.apcs.size() >= 1024)
                fail(8);
            else {
                target.apcs.push_back({arg(0), {arg(2)}});
                result = 1;
                threads_.yield = true;
            }
        }
    } else if (symbol == "QueryThreadCycleTime") {
        const auto id = thread_id(arg(0));
        if (!id)
            fail(6);
        else if (!arg(1))
            fail(87);
        else {
            memory.store(GuestAddress{arg(1)}, 64, threads_.items.at(id).cycles);
            result = 1;
        }
    } else if (symbol == "OpenThread") {
        try {
            const auto found = threads_.items.find(thread_from_identifier(u32(2)));
            const auto rights = u32(0);
            if (rights & 0x1000000U)
                fail(1314);
            else if (rights & ~0xf21fffffU)
                fail(50);
            else if (found == threads_.items.end())
                result = open_process_thread(memory, u32(2), rights, arg(1) != 0);
            else if (runtime_.process_thread_handles.size() >= 1024)
                fail(8);
            else
                result = open_local_thread(found->first, rights, arg(1) != 0);
        } catch (const SecurityDescriptorError &error) {
            fail(error.code);
            result = 0;
        } catch (const std::bad_alloc &) {
            fail(8);
            result = 0;
        }
    } else if (symbol.find("SRWLock") != symbol.npos) {
        // 字組編碼（與客體快速路徑共用）：bit0 獨占持有、bit1 宿主有等待者、bit2 起為共用持有數。
        // Win64 客體常式只在 bit0／bit1 皆零時以 lock cmpxchg 取得或釋放，宿主不再保存持有者身分；
        // 此處依字組判定、以 CAS 更新，並在有等待者時設 bit1，讓之後的釋放一律進宿主喚醒。
        const auto address = arg(0);
        const std::uint8_t once_width = x86_ ? 32 : 64;
        memory.check(GuestAddress{address}, once_width / 8, Access::Write);
        auto value = memory.load(GuestAddress{address}, once_width);
        // 平行 worker 的客體快速路徑可能同時改寫字組；CAS 失敗就以新值重新判定，不丟錯誤。
        const auto exchange = [&](std::uint64_t next) {
            const auto seen = memory.atomic_rmw(GuestAddress{address}, once_width, GuestMemory::AtomicRmw::CompareExchange, next, value);
            if (seen == value) return true;
            value = seen;
            return false;
        };
        const auto waiting = [&] {
            return std::any_of(threads_.items.begin(), threads_.items.end(), [&](const auto &entry) {
                const auto &thread = entry.second;
                const auto &wait = thread.wait;
                return thread.status == ThreadStatus::Waiting &&
                       ((wait.kind == ThreadWaitKind::Slim && wait.handles[0] == address) ||
                        (wait.kind == ThreadWaitKind::Condition && !wait.critical && wait.handles[1] == address));
            });
        };
        if (symbol == "InitializeSRWLock") {
            // 與 Windows 相同，初始化只把字組設為 SRWLOCK_INIT；新配置的記憶體可能殘留非零值，
            // 字組本身無法區分殘值與持有中，只保留宿主可靠追蹤的等待者檢查。
            if (waiting())
                throw HostCallFault("不可重新初始化等待中的 SRW lock");
            while (!exchange(0)) {}
            runtime_.slim_locks.erase(address);
        } else {
            const bool shared = symbol.ends_with("Shared");
            if (symbol.starts_with("Release")) {
                for (;;) {
                    if (shared) {
                        if (!(value >> 2))
                            throw HostCallFault("執行緒未持有共用 SRW lock");
                        if (exchange(value - 4)) break;
                    } else {
                        if (!(value & 1))
                            throw HostCallFault("執行緒未持有獨占 SRW lock");
                        if (exchange(value & ~UINT64_C(1))) break;
                    }
                }
                // 等待者由排程器的等待解析依字組取得鎖；bit1 保留到最後一位等待者離開。
                detail::record_srw((value & 2) ? detail::SrwEvent::HostReleaseWake : detail::SrwEvent::HostRelease);
                if (value & 2)
                    resolve_thread_waits(memory,ThreadWakeScope::Slim,address);
            } else {
                bool acquired = false, queued = false;
                for (;;) {
                    if (!(value & 1) && (shared || !(value >> 2))) {
                        if (shared && (value >> 2) == (once_width == 32 ? 0x3fffffffU : UINT64_C(0x3fffffffffffffff)))
                            throw HostCallFault("SRW reader 次數溢位");
                        if (exchange(shared ? value + 4 : value | 1)) { acquired = true; break; }
                    } else if (symbol.starts_with("Try")) {
                        break;
                    } else if (exchange(value | 2)) { queued = true; break; }
                }
                if (acquired) result = 1;
                detail::record_srw(acquired ? detail::SrwEvent::HostAcquire : queued ? detail::SrwEvent::HostQueue : detail::SrwEvent::HostTryFail);
                if (queued) {
                auto &thread = threads_.items.at(threads_.current);
                thread.status = ThreadStatus::Waiting;
                thread.wait = {};
                thread.wait.kind = ThreadWaitKind::Slim;
                thread.wait.handles = {address};
                thread.wait.shared = shared;
                schedule(state, memory);
                return std::nullopt;
                }
            }
        }
    } else if (symbol.find("ConditionVariable") != symbol.npos) {
        const auto condition = arg(0);
        memory.check(GuestAddress{condition}, x86_ ? 4 : 8, Access::Write);
        if (symbol == "InitializeConditionVariable") {
            if (std::any_of(threads_.items.begin(), threads_.items.end(), [&](const auto &entry) {
                    const auto &t = entry.second;
                    return t.status == ThreadStatus::Waiting && t.wait.kind == ThreadWaitKind::Condition &&
                           t.wait.handles[0] == condition;
                }))
                throw HostCallFault("不可重新初始化等待中的條件變數");
            memory.store(GuestAddress{condition}, x86_ ? 32 : 64, 0);
        } else if (symbol.starts_with("Wake")) {
            bool notified=false;
            for (auto &[id, thread] : threads_.items) {
                (void)id;
                auto &wait = thread.wait;
                if (thread.status == ThreadStatus::Waiting && wait.kind == ThreadWaitKind::Condition &&
                    wait.handles[0] == condition && !wait.signaled) {
                    wait.signaled = true;
                    wait.deadline = UINT64_MAX;
                    notified=true;
                    if (symbol == "WakeConditionVariable")
                        break;
                }
            }
            if(notified)resolve_thread_waits(memory,ThreadWakeScope::Condition,condition);
        } else {
            const bool critical = symbol.ends_with("CS"), shared = !critical && (u32(3) & 1);
            const auto address = arg(1);
            if (!critical && (u32(3) & ~1U))
                fail(87);
            else {
                memory.check(GuestAddress{address}, critical ? (x86_ ? 24 : 40) : (x86_ ? 4 : 8), Access::Write);
                if (critical) {
                    const auto found = runtime_.critical_sections.find(address);
                    if (found == runtime_.critical_sections.end() || found->second.owner != threads_.current ||
                        found->second.recursion != 1)
                        throw HostCallFault("條件變數必須搭配只取得一次的 critical section");
                    found->second.owner = 0;
                    found->second.recursion = 0;
                    memory.store(GuestAddress{address + (x86_ ? 4 : 8)}, 32, UINT32_MAX);
                    memory.store(GuestAddress{address + (x86_ ? 8 : 12)}, 32, 0);
                    memory.store(GuestAddress{address + (x86_ ? 12 : 16)}, x86_ ? 32 : 64, 0);
                } else {
                    const std::uint8_t width = x86_ ? 32 : 64;
                    auto word = memory.load(GuestAddress{address}, width);
                    if (shared ? !(word >> 2) : !(word & 1))
                        throw HostCallFault("條件變數的 SRW lock 尚未持有");
                    const auto next = shared ? word - 4 : word & ~UINT64_C(1);
                    if (memory.atomic_rmw(GuestAddress{address}, width, GuestMemory::AtomicRmw::CompareExchange, next, word) != word)
                        throw HostCallFault("SRW lock 狀態在服務期間被並行改寫");
                }
                // 釋放與登記等待之間沒有客體排程切點，避免遺失 wake。
                auto &thread = threads_.items.at(threads_.current);
                thread.status = ThreadStatus::Waiting;
                thread.wait = {};
                auto &wait = thread.wait;
                wait.kind = ThreadWaitKind::Condition;
                wait.handles = {condition, address};
                wait.shared = shared;
                wait.critical = critical;
                wait.deadline =
                    u32(2) == UINT32_MAX ? UINT64_MAX : runtime_.clock_ticks + std::uint64_t{u32(2)} * 10000;
                schedule(state, memory);
                return std::nullopt;
            }
        }
    } else if (symbol.starts_with("InitOnce")) {
        const auto address = arg(0);
        const std::uint8_t once_width = x86_ ? 32 : 64;
        memory.check(GuestAddress{address}, once_width / 8, Access::Write);
        const auto value = memory.load(GuestAddress{address}, once_width);
        auto found = runtime_.once_initializations.find(address);
        if (found == runtime_.once_initializations.end()) {
            if (value && symbol != "InitOnceInitialize")
                throw HostCallFault("未初始化的 INIT_ONCE");
            if (runtime_.once_initializations.size() >= 65536)
                throw HostCallFault("INIT_ONCE 記錄超過上限");
            found = runtime_.once_initializations.emplace(address, OnceInitialization{}).first;
        }
        auto &once = found->second;
        const auto has_waiters = [&] {
            return std::any_of(threads_.items.begin(), threads_.items.end(), [&](const auto &entry) {
                const auto &thread = entry.second;
                const auto &wait = thread.wait;
                return thread.status != ThreadStatus::Exited && wait.kind == ThreadWaitKind::Once &&
                       !wait.handles.empty() && wait.handles[0] == address;
            });
        };
        // 新物件可在原位址以 INIT_ONCE_STATIC_INIT 建立；已完成的宿主快取
        // 不得把前一個物件的 context 帶入新生命週期。仍在使用中的物件不能歸零。
        if (!value && (once.owner || once.complete)) {
            if (once.owner || has_waiters())
                throw HostCallFault("不可直接重設使用中的 INIT_ONCE");
            once = {};
        }
        if (symbol == "InitOnceInitialize") {
            if (once.owner || has_waiters())
                throw HostCallFault("不可重設使用中的 INIT_ONCE");
            once = {};
            memory.store(GuestAddress{address}, once_width, 0);
        } else if (symbol == "InitOnceExecuteOnce") {
            if (arg(3))
                memory.check(GuestAddress{arg(3)}, once_width / 8, Access::Write);
            if (once.complete) {
                if (arg(3))
                    memory.store(GuestAddress{arg(3)}, once_width, once.context);
                result = 1;
            } else if (!once.owner) {
                memory.check(GuestAddress{arg(1)}, 1, Access::Execute);
                const auto context = allocate_heap(memory, 8);
                if (!context)
                    fail(8);
                else {
                    memory.store(GuestAddress{context}, 64, 0);
                    once.owner = threads_.current;
                    memory.store(GuestAddress{address}, once_width, 1);
                    threads_.items.at(threads_.current).wait = {};
                    start_callbacks(state, memory, {{arg(1), {address, arg(2), context}}}, Continuation::InitOnce,
                                    context);
                    return std::nullopt;
                }
            } else {
                auto &thread = threads_.items.at(threads_.current);
                thread.status = ThreadStatus::Waiting;
                thread.wait = {};
                thread.wait.kind = ThreadWaitKind::Once;
                thread.wait.handles = {address};
                schedule(state, memory);
                return std::nullopt;
            }
            threads_.items.at(threads_.current).wait = {};
        } else if (symbol == "InitOnceBeginInitialize") {
            if (u32(1) & ~1U)
                throw HostCallFault("INIT_ONCE 非同步初始化尚未支援");
            memory.check(GuestAddress{arg(2)}, 4, Access::Write);
            if (arg(3))
                memory.check(GuestAddress{arg(3)}, once_width / 8, Access::Write);
            if (once.complete) {
                memory.store(GuestAddress{arg(2)}, 32, 0);
                if (arg(3))
                    memory.store(GuestAddress{arg(3)}, once_width, once.context);
                result = 1;
            } else if (u32(1) & 1)
                fail(31);
            else if (!once.owner) {
                once.owner = threads_.current;
                memory.store(GuestAddress{address}, once_width, 1);
                memory.store(GuestAddress{arg(2)}, 32, 1);
                if (arg(3))
                    memory.store(GuestAddress{arg(3)}, once_width, 0);
                result = 1;
            } else {
                auto &thread = threads_.items.at(threads_.current);
                thread.status = ThreadStatus::Waiting;
                thread.wait = {};
                thread.wait.kind = ThreadWaitKind::Once;
                thread.wait.handles = {address};
                schedule(state, memory);
                return std::nullopt;
            }
            // 同步 waiter 保留原呼叫重新進入；完成後不再持有此物件的等待紀錄。
            auto &wait = threads_.items.at(threads_.current).wait;
            if (wait.kind == ThreadWaitKind::Once && !wait.handles.empty() && wait.handles[0] == address)
                wait = {};
        } else {
            if ((u32(1) & ~4U) || (arg(2) & 3) || ((u32(1) & 4) && arg(2)))
                fail(87);
            else if (once.complete || once.owner != threads_.current)
                fail(31);
            else {
                once.owner = 0;
                once.complete = !(u32(1) & 4);
                once.context = arg(2);
                memory.store(GuestAddress{address}, once_width, once.complete ? once.context | 2 : 0);
                result = 1;
            }
        }
    } else if (symbol == "SetFileCompletionNotificationModes") {
        const auto modes = static_cast<std::uint8_t>(arg(1));
        const auto file = files_.find(arg(0));
        const auto socket = runtime_.network.sockets.find(arg(0));
        if (modes & ~3U)
            fail(87);
        else if (file == files_.end() && socket == runtime_.network.sockets.end())
            fail(6);
        else {
            if (file != files_.end())
                for (auto &[handle, item] : files_) {
                    (void)handle;
                    if (item.object == file->second.object)
                        item.completion_modes |= modes;
                }
            if (socket != runtime_.network.sockets.end())
                socket->second->completion_modes |= modes;
            result = 1;
        }
    } else if (symbol == "CreateIoCompletionPort") {
        const auto file = files_.find(arg(0));
        const auto socket = runtime_.network.sockets.find(arg(0));
        if (arg(1) && !runtime_.completion_ports.contains(arg(1)))
            fail(6);
        else if (arg(0) != (x86_ ? UINT32_MAX : UINT64_MAX) && file == files_.end() &&
                 socket == runtime_.network.sockets.end())
            fail(6);
        else if (socket != runtime_.network.sockets.end() && socket->second->completion_port &&
                 socket->second->completion_port != arg(1))
            fail(87);
        else if (file != files_.end() && file->second.completion_port && file->second.completion_port != arg(1))
            fail(87);
        else if (!arg(1) && runtime_.completion_ports.size() >= 1024)
            fail(8);
        else {
            result = arg(1);
            if (!result) {
                result = runtime_.allocate_wait_handle();
                runtime_.completion_ports.emplace(result, CompletionPort{});
            }
            if (socket != runtime_.network.sockets.end() && !socket->second->completion_port) {
                socket->second->completion_port = result;
                socket->second->completion_key = arg(2);
            }
            if (file != files_.end() && !file->second.completion_port) {
                file->second.completion_port = result;
                file->second.completion_key = arg(2);
            }
        }
    } else if (symbol == "PostQueuedCompletionStatus") {
        const auto found = runtime_.completion_ports.find(arg(0));
        if (found == runtime_.completion_ports.end())
            fail(6);
        else if (found->second.packets.size() >= 65536)
            fail(8);
        else {
            found->second.packets.push_back({arg(2), arg(3), 0, u32(1)});
            result = 1;
        }
    } else if (symbol == "GetQueuedCompletionStatus" || symbol == "GetQueuedCompletionStatusEx") {
        const bool extended = symbol.ends_with("Ex");
        if (!runtime_.completion_ports.contains(arg(0)))
            fail(6);
        else if (!arg(1) || !arg(3) || (extended ? u32(2) == 0 : arg(2) == 0))
            fail(87);
        else if (extended && u32(2) > 65536)
            fail(87);
        else {
            memory.check(GuestAddress{arg(1)}, extended ? std::uint64_t{u32(2)} * (x86_ ? 16 : 32) : 4, Access::Write);
            memory.check(GuestAddress{arg(3)}, extended ? 4 : (x86_ ? 4 : 8), Access::Write);
            if (!extended)
                memory.check(GuestAddress{arg(2)}, x86_ ? 4 : 8, Access::Write);
            const auto handle = arg(0);
            // 已有 packet 或零逾時不必保存／搬移整個執行緒 context；alertable 與既有 waiter 仍走原排程。
            // 先確認快速路徑資格；無法使用時不必掃描同 port 的等待者。
            const bool immediate_eligible = detail::io_wait_fastpath_enabled() && !(extended && u32(5)) &&
                (!runtime_.completion_ports.at(handle).packets.empty() || u32(4)==0);
            const bool waiter = immediate_eligible && std::any_of(threads_.items.begin(),threads_.items.end(),[&](const auto& item) {
                const auto& t=item.second;
                return t.status==ThreadStatus::Waiting && t.wait.kind==ThreadWaitKind::Completion &&
                       !t.wait.handles.empty() && t.wait.handles[0]==handle;
            });
            if (immediate_eligible && !waiter) {
                // 空佇列的零逾時仍先執行共用 I/O／工作／GUI 輪詢，避免漏掉已完成但尚未發布的 packet。
                if (runtime_.completion_ports.at(handle).packets.empty())wake_threads(memory);
                if (!runtime_.forced_exit_code() && !debugger_stopped()) {
                    const auto completion = poll_completion(state,memory,handle,extended,u32(4)==0);
                    if (completion.ready) {
                        if (completion.error)last_error_=completion.error;
                        ++io_wait_stats_.immediate;
                        if (!completion.guest_return)arg.return_to(state,completion.value);
                        return std::nullopt;
                    }
                }
            }
            auto &thread = threads_.items.at(threads_.current);
            thread.status = ThreadStatus::Waiting;
            thread.wait = {};
            thread.wait.kind = ThreadWaitKind::Completion;
            ++io_wait_stats_.waits;
            thread.wait.handles = {arg(0)};
            thread.wait.all = extended;
            thread.wait.alertable = extended && u32(5);
            thread.wait.deadline =
                u32(4) == UINT32_MAX ? UINT64_MAX : runtime_.clock_ticks + std::uint64_t{u32(4)} * 10000;
            schedule(state, memory);
            return std::nullopt;
        }
    } else if (symbol == "CreateToolhelp32Snapshot") {
        if (u32(0) & ~0x8000001eU) {
            last_error_ = 50;
            result = UINT64_MAX;
        } else if ((u32(0) & 0x18U) && u32(1) && u32(1) != process_id()) {
            last_error_ = 50;
            result = UINT64_MAX;
        } else if ((u32(0) & 0x18U) && await_loader(state, memory))
            return std::nullopt;
        else if (runtime_.process_snapshots.size() >= 128) {
            last_error_ = 8;
            result = UINT64_MAX;
        } else {
            ProcessSnapshot snapshot;
            snapshot.flags = u32(0);
            for (const auto &[id, thread] : threads_.items)
                if (thread.status != ThreadStatus::Exited)
                    snapshot.threads.push_back(thread_identifier(id));
            const auto argv = memory.load(GuestAddress{runtime_.wargv_cell}, x86_ ? 32 : 64);
            const auto name = memory.load(GuestAddress{argv}, x86_ ? 32 : 64);
            if (name)
                snapshot.executable = read_utf16(memory, GuestAddress{name});
            const auto slash = snapshot.executable.find_last_of(u"\\/");
            if (slash != snapshot.executable.npos)
                snapshot.executable.erase(0, slash + 1);
            if (snapshot.flags & 2U) {
                const auto error = capture_process_snapshot(memory, snapshot.processes);
                if (error) {
                    last_error_ = error;
                    arg.return_to(state, UINT64_MAX);
                    return std::nullopt;
                }
            }
            // 快照持有建立當下的名稱、位址、大小；後續卸載／載入不改變列舉結果。
            if ((snapshot.flags & 8U) || (x86_ && (snapshot.flags & 16U))) {
                const auto append = [&](const RuntimeImage &image, std::uint32_t references) {
                    if (!image.base)
                        return;
                    auto path = image.path;
                    if (path.empty())
                        path = name ? read_utf16(memory, GuestAddress{name}) : std::u16string{};
                    if (path.find(u':') == path.npos)
                        path = u"C:\\" + path;
                    snapshot.modules.push_back(
                        {image.base, static_cast<std::uint32_t>(image.size), references, std::move(path)});
                };
                append(runtime_.image, UINT32_MAX);
                for (const auto base : runtime_.module_order) {
                    const auto found = runtime_.loaded_modules.find(base);
                    if (found != runtime_.loaded_modules.end())
                        append(found->second.image, found->second.pinned ? UINT32_MAX : found->second.references);
                }
                for (const auto &[base, image] : runtime_.builtin_images) {
                    (void)base;
                    append(image, UINT32_MAX);
                }
            }
            result = runtime_.allocate_wait_handle();
            runtime_.process_snapshots.emplace(result, std::move(snapshot));
        }
    } else if (symbol.starts_with("Module32")) {
        const auto found = runtime_.process_snapshots.find(arg(0));
        if (found == runtime_.process_snapshots.end())
            fail(6);
        else if (!arg(1))
            fail(87);
        else {
            const bool wide = symbol.back() == 'W';
            const auto required = wide ? (x86_ ? 1064U : 1080U) : (x86_ ? 548U : 568U);
            if (memory.load(GuestAddress{arg(1)}, 32) < required)
                fail(24);
            else {
                auto &snapshot = found->second;
                if (symbol.find("First") != symbol.npos)
                    snapshot.module_cursor = 0;
                if (snapshot.module_cursor >= snapshot.modules.size())
                    fail(18);
                else {
                    const auto &module = snapshot.modules[snapshot.module_cursor];
                    auto basename = module.path;
                    const auto slash = basename.find_last_of(u"\\/");
                    if (slash != basename.npos)
                        basename.erase(0, slash + 1);
                    std::vector<std::uint8_t> bytes(required);
                    const auto put = [&](unsigned at, std::uint64_t value, unsigned count) {
                        for (unsigned n = 0; n < count; ++n)
                            bytes[at + n] = static_cast<std::uint8_t>(value >> (8 * n));
                    };
                    const auto text = [&](unsigned at, std::u16string_view value, unsigned capacity) {
                        if (wide) {
                            const auto encoded = utf16_bytes(value.substr(0, capacity - 1));
                            std::copy(encoded.begin(), encoded.end(), bytes.begin() + at);
                        } else {
                            const auto encoded = utf16_to_utf8(value);
                            std::copy_n(encoded.begin(), std::min<std::size_t>(capacity - 1, encoded.size()),
                                        bytes.begin() + at);
                        }
                    };
                    put(0, required, 4);
                    put(4, 1, 4);
                    put(8, process_id(), 4);
                    put(12, module.references, 4);
                    put(16, module.references, 4);
                    put(x86_ ? 20 : 24, module.base, x86_ ? 4 : 8);
                    put(x86_ ? 24 : 32, module.size, 4);
                    put(x86_ ? 28 : 40, module.base, x86_ ? 4 : 8);
                    text(x86_ ? 32 : 48, basename, 256);
                    text(wide ? (x86_ ? 544 : 560) : (x86_ ? 288 : 304), module.path, 260);
                    memory.write(GuestAddress{arg(1)}, bytes);
                    ++snapshot.module_cursor;
                    result = 1;
                }
            }
        }
    } else if (symbol.starts_with("Process32") || symbol.starts_with("Thread32")) {
        const auto found = runtime_.process_snapshots.find(arg(0));
        if (found == runtime_.process_snapshots.end())
            fail(6);
        else if (!arg(1))
            fail(87);
        else {
            const bool thread = symbol.starts_with("Thread32"), first = symbol.find("First") != symbol.npos,
                       wide = symbol.back() == 'W';
            const auto required = thread ? 28U : wide ? (x86_ ? 556U : 568U) : (x86_ ? 296U : 304U);
            const auto capacity = memory.load(GuestAddress{arg(1)}, 32);
            auto &snapshot = found->second;
            auto &cursor = thread ? snapshot.thread_cursor : snapshot.process_cursor;
            if (capacity < required)
                fail(24);
            else {
                if (first)
                    cursor = 0;
                if (!(snapshot.flags & (thread ? 4U : 2U)) ||
                    cursor >= (thread ? snapshot.threads.size() : snapshot.processes.size()))
                    fail(18);
                else {
                    memory.write(GuestAddress{arg(1)}, std::vector<std::uint8_t>(required));
                    memory.store(GuestAddress{arg(1)}, 32, required);
                    if (thread) {
                        memory.store(GuestAddress{arg(1) + 8}, 32, snapshot.threads[cursor]);
                        memory.store(GuestAddress{arg(1) + 12}, 32, process_id());
                        memory.store(GuestAddress{arg(1) + 16}, 32, 8);
                    } else {
                        const auto &process = snapshot.processes[cursor];
                        memory.store(GuestAddress{arg(1) + 8}, 32, process.id);
                        memory.store(GuestAddress{arg(1) + (x86_ ? 20U : 28U)}, 32, process.threads);
                        memory.store(GuestAddress{arg(1) + (x86_ ? 24U : 32U)}, 32, process.parent);
                        memory.store(GuestAddress{arg(1) + (x86_ ? 28U : 36U)}, 32, process.priority);
                        if (wide)
                            memory.write(GuestAddress{arg(1) + (x86_ ? 36U : 44U)},
                                         utf16_bytes(process.executable.substr(0, 259)));
                        else {
                            auto name = utf16_to_utf8(process.executable);
                            name.resize(std::min<std::size_t>(259, name.size()));
                            memory.write(GuestAddress{arg(1) + (x86_ ? 36U : 44U)},
                                         std::span(reinterpret_cast<const std::uint8_t *>(name.data()), name.size()));
                        }
                    }
                    ++cursor;
                    result = 1;
                }
            }
        }
    } else if (symbol.find("CriticalSection") != symbol.npos) {
        const auto address = arg(0);
        if(address==peb_base_.value+0x800){
            if(symbol=="EnterCriticalSection" || symbol=="TryEnterCriticalSection"){
                if(runtime_.loader_owner && runtime_.loader_owner!=threads_.current){
                    if(symbol=="TryEnterCriticalSection"){arg.return_to(state,0);return {};}
                    (void)await_loader(state,memory);return {};
                }
                if(runtime_.loader_depth==UINT32_MAX)throw HostCallFault("loader lock 遞迴深度溢位");
                runtime_.loader_owner=threads_.current;++runtime_.loader_depth;result=1;
            }else if(symbol=="LeaveCriticalSection")release_loader_notification();
            else if(symbol=="SetCriticalSectionSpinCount"){
                const auto at=GuestAddress{address+(x86_?20:32)};result=memory.load(at,x86_?32:64);memory.store(at,x86_?32:64,u32(1));
            }else throw HostCallFault("不可重建或刪除程序的 loader lock");
            publish_loader_lock(memory,peb_base_.value,x86_,thread_identifier(runtime_.loader_owner),runtime_.loader_depth);
            arg.return_to(state,result);return {};
        }
        memory.check(GuestAddress{address}, x86_ ? 24 : 40, Access::Write);
        if (symbol.starts_with("Initialize")) {
            if (symbol == "InitializeCriticalSectionEx" && (u32(2) & ~0x17000000U))
                fail(87);
            else {
                CriticalSection section;
                section.spin = symbol == "InitializeCriticalSection" ? 0 : u32(1);
                memory.write(GuestAddress{address}, std::vector<std::uint8_t>(x86_ ? 24 : 40));
                memory.store(GuestAddress{address + (x86_ ? 4 : 8)}, 32, UINT32_MAX);
                memory.store(GuestAddress{address + (x86_ ? 20 : 32)}, x86_ ? 32 : 64, section.spin);
                if (symbol == "InitializeCriticalSectionEx" && (u32(2) & 0x10000000U)) {
                    section.debug = allocate_heap(memory, x86_ ? 32 : 48);
                    if (!section.debug) {
                        fail(8);
                        arg.return_to(state, result);
                        return std::nullopt;
                    }
                    memory.store(GuestAddress{section.debug + (x86_ ? 4U : 8U)}, x86_ ? 32 : 64, address);
                    const auto list = section.debug + (x86_ ? 8U : 16U);
                    memory.store(GuestAddress{list}, x86_ ? 32 : 64, list);
                    memory.store(GuestAddress{list + (x86_ ? 4U : 8U)}, x86_ ? 32 : 64, list);
                    memory.store(GuestAddress{address}, x86_ ? 32 : 64, section.debug);
                }
                runtime_.critical_sections[address] = section;
                result = 1;
            }
        } else {
            const auto found = runtime_.critical_sections.find(address);
            if (found == runtime_.critical_sections.end())
                throw HostCallFault("未初始化的 critical section");
            auto &section = found->second;
            if (symbol == "SetCriticalSectionSpinCount") {
                result = section.spin;
                section.spin = u32(1);
                memory.store(GuestAddress{address + (x86_ ? 20 : 32)}, x86_ ? 32 : 64, section.spin);
            } else if (symbol == "DeleteCriticalSection") {
                const bool waiting=std::any_of(threads_.items.begin(),threads_.items.end(),[&](const auto& entry){
                    const auto& thread=entry.second;const auto& wait=thread.wait;
                    if(thread.status!=ThreadStatus::Waiting)return false;
                    if(wait.kind==ThreadWaitKind::Critical || wait.kind==ThreadWaitKind::CriticalRetry || (wait.kind==ThreadWaitKind::CrtLockRetry && wait.critical))
                        return !wait.handles.empty() && wait.handles[0]==address;
                    return wait.kind==ThreadWaitKind::Condition && wait.critical && wait.handles.size()>1 && wait.handles[1]==address;
                });
                // 舊 runtime 的物件解構可能仍持有自己的鎖。沒有其他參與者時實際
                // 回收資源；其他執行緒持有或仍在等待的未定義用法保留明確診斷。
                if ((section.owner && section.owner!=threads_.current) || waiting)
                    throw HostCallFault("不可刪除其他執行緒持有或等待中的 critical section");
                if (section.debug)
                    (void)free_heap(memory, section.debug, state);
                memory.store(GuestAddress{address},x86_ ? 32 : 64,0);
                memory.store(GuestAddress{address+(x86_ ? 4 : 8)},32,UINT32_MAX);
                memory.store(GuestAddress{address+(x86_ ? 8 : 12)},32,0);
                memory.store(GuestAddress{address+(x86_ ? 12 : 16)},x86_ ? 32 : 64,0);
                memory.store(GuestAddress{address+(x86_ ? 16 : 24)},x86_ ? 32 : 64,0);
                runtime_.critical_sections.erase(found);
            } else {
                if (symbol == "LeaveCriticalSection") {
                    if (section.owner != threads_.current || !section.recursion)
                        throw HostCallFault("執行緒未持有 critical section");
                    if (!--section.recursion)
                        section.owner = 0;
                } else if (!section.owner || section.owner == threads_.current) {
                    if (section.recursion == UINT32_MAX)
                        throw HostCallFault("critical section 遞迴過深");
                    section.owner = threads_.current;
                    ++section.recursion;
                    result = 1;
                } else if (symbol == "TryEnterCriticalSection")
                    result = 0;
                else {
                    auto &thread = threads_.items.at(threads_.current);
                    thread.status = ThreadStatus::Waiting;
                    thread.wait = {};
                    thread.wait.kind = ThreadWaitKind::Critical;
                    thread.wait.handles = {address};
                    schedule(state, memory);
                    return std::nullopt;
                }
                memory.store(GuestAddress{address + (x86_ ? 4 : 8)}, 32, section.recursion - 1U);
                memory.store(GuestAddress{address + (x86_ ? 8 : 12)}, 32, section.recursion);
                // Win32 可見的擁有者使用公開 TID；排程與鎖表仍以內部 ID 索引。
                memory.store(GuestAddress{address + (x86_ ? 12 : 16)}, x86_ ? 32 : 64, thread_identifier(section.owner));
                if(symbol=="LeaveCriticalSection" && !section.owner)
                    resolve_thread_waits(memory,ThreadWakeScope::Critical,address);
            }
        }
    } else if (symbol == "CreateThread" || symbol == "_beginthreadex" || symbol == "CreateRemoteThread" ||
               symbol == "CreateRemoteThreadEx") {
        const bool remote = symbol.starts_with("CreateRemoteThread"), extended = symbol == "CreateRemoteThreadEx";
        const unsigned shift = remote ? 1 : 0, id_argument = extended ? 7 : 5 + shift;
        const auto flags_value = u32(4 + shift);
        const auto requested = arg(1 + shift);
        if (remote && !current_process_handle(arg(0), 0x43a))
            fail(current_process_handle(arg(0)) ? 5U : process_object_type(arg(0)) == u"Process" ? 50U : 6U);
        else if (extended && arg(6))
            fail(50);
        else if (flags_value & ~0x10004U || requested > 8U * 1024 * 1024)
            fail(87);
        else {
            try {
                const auto security = synchronization_descriptor(memory, arg(shift));
                if (arg(id_argument))
                    memory.check(GuestAddress{arg(id_argument)}, 4, Access::Write);
                // 起始位址由真正啟動時的 CPU 執行權限檢查；CREATE_SUSPENDED 不提前執行它。
                result = create_guest_thread(state, memory, arg(2 + shift), arg(3 + shift), requested, flags_value);
                if (result) {
                    runtime_.process_thread_handles.at(result).second = security.inherit;
                    runtime_.handle_flags[result] = security.inherit ? 1U : 0U;
                    threads_.items.at(thread_id(result)).security_descriptor = security.bytes;
                    threads_.items.at(thread_id(result)).pseudo_access = created_thread_pseudo_access(security.bytes);
                    std::vector<std::uint8_t>* local{};int native=-1,lock=-1;bool is_thread=false;
                    if(const auto error=process_security_location(result,0,local,native,lock,is_thread))throw SecurityDescriptorError{error};
                    if(native>=0){SharedEventLock guard(lock);set_shared_object_security(native,security.bytes,true);}
                    if (arg(id_argument))
                        memory.store(GuestAddress{arg(id_argument)}, 32, thread_identifier(thread_id(result)));
                }
            } catch (const SecurityDescriptorError &error) {
                fail(error.code);
            } catch (const MemoryFault &) {
                fail(87);
            }
        }
    } else if (symbol == "TerminateThread") {
        const auto id = thread_id(arg(0));
        if (!id)
            fail(6);
        else {
            auto &target = threads_.items.at(id);
            result = 1;
            if (target.status != ThreadStatus::Exited && !target.forced_stop) {
                target.forced_stop = target.terminating = true;
                target.exit_code = u32(1);
                target.apcs.clear();
                target.wait = {};
                target.suspended = 0;
                target.status = ThreadStatus::Runnable;
                target.context.pending_cpu_trap = 0;
                target.context.rip = GuestAddress{callback_return_.value + 0xa0};
                notify_concurrency_changed();
                threads_.yield = true;
                if (id == threads_.current) {
                    state.rip = target.context.rip;
                    return force_finish_thread(state, memory);
                }
            }
        }
    } else if (symbol == "ExitThread" || symbol == "_endthreadex")
        return exit_thread(state, memory, u32(0));
    else if (symbol == "SetThreadIdealProcessor") {
        const auto id = thread_id(arg(0));
        const auto processor = u32(1), query = x86_ ? 32U : 64U;
        if (!id) {
            fail(6);
            result = UINT32_MAX;
        } else if (processor != query && processor >= runtime_.processor_count) {
            fail(87);
            result = UINT32_MAX;
        } else {
            auto &thread = threads_.items.at(id);
            result = thread.ideal_processor;
            if (processor != query) {
                thread.ideal_processor = processor;
                if (id == threads_.current && (thread.affinity & (UINT64_C(1) << processor)))
                    state.processor_id = processor;
            }
        }
    } else if (symbol == "SetThreadAffinityMask") {
        const auto id = thread_id(arg(0));
        const auto mask = runtime_.process_affinity;
        if (!id)
            fail(6);
        else if (!arg(1) || (arg(1) & ~mask))
            fail(87);
        else {
            auto &thread = threads_.items.at(id);
            result = thread.affinity & mask;
            thread.affinity = arg(1);
            if (id == threads_.current && !(arg(1) & (UINT64_C(1) << state.processor_id)))
                state.processor_id = static_cast<std::uint32_t>(std::countr_zero(arg(1)));
        }
    } else if (symbol == "SetThreadStackGuarantee" || symbol == "_resetstkoflw") {
        const bool reset=symbol=="_resetstkoflw";
        try{
            if(!reset){memory.check(GuestAddress{arg(0)},4,Access::Read);memory.check(GuestAddress{arg(0)},4,Access::Write);}
            const auto requested=reset?0U:static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)},32));std::uint32_t previous{};
            const auto error=configure_stack_guarantee(state,memory,requested,reset,previous);
            if(error){if(!reset)fail(error);}else{if(!reset)memory.store(GuestAddress{arg(0)},32,previous);result=1;}
        }catch(const MemoryFault&){if(!reset)fail(998);}catch(const std::bad_alloc&){if(!reset)fail(8);}
    } else if (symbol == "GetCurrentThreadStackLimits") {
        const auto teb = x86_ ? state.fs_base : state.gs_base;
        const std::uint8_t width = x86_ ? 32 : 64;
        const unsigned unit = width / 8;
        const auto low = memory.load(GuestAddress{teb + 2 * unit}, width),
                   high = memory.load(GuestAddress{teb + unit}, width);
        memory.check(GuestAddress{arg(0)}, unit, Access::Write);
        memory.check(GuestAddress{arg(1)}, unit, Access::Write);
        memory.store(GuestAddress{arg(0)}, width, low);
        memory.store(GuestAddress{arg(1)}, width, high);
    } else if (symbol == "GetCurrentThreadId")
        result = thread_identifier(threads_.current);
    else if (symbol == "SignalObjectAndWait") {
        const auto signal = runtime_.wait_objects.find(arg(0)), waited = runtime_.wait_objects.find(arg(1));
        const auto id = thread_id(arg(1));
        const bool valid_wait = waited != runtime_.wait_objects.end() || id || process_signaled(arg(1)).has_value() ||
                                file_io_signaled(arg(1)).has_value();
        if (signal == runtime_.wait_objects.end() || signal->second.kind == WaitObject::Kind::Timer || !valid_wait) {
            last_error_ = 6;
            result = UINT32_MAX;
        } else if (!(signal->second.access & (signal->second.kind == WaitObject::Kind::Mutex ? 1U : 2U)) ||
                   (waited != runtime_.wait_objects.end() && !(waited->second.access & 0x100000)) ||
                   ((id || process_object_type(arg(1))==u"Thread") && !(thread_access(arg(1)) & 0x100000))) {
            last_error_ = 5;
            result = UINT32_MAX;
        } else {
            auto &object = runtime_.wait_state(signal->second);
            std::uint32_t error = 0;
            if (object.kind == WaitObject::Kind::Mutex) {
                auto &mutex = runtime_.mutex_objects.at(object.mutex);
                if (mutex.shared) {
                    SharedEventLock lock(mutex.shared->lock_descriptor());
                    error = mutex.shared->release_locked(threads_.current);
                } else if (mutex.owner != threads_.current || !mutex.recursion)
                    error = 288;
                else if (!--mutex.recursion)
                    mutex.owner = 0;
            } else if (object.kind == WaitObject::Kind::Semaphore) {
                if (object.count == object.maximum)
                    error = 298;
                else
                    ++object.count;
            } else
                object.set_signal(true);
            if (error) {
                last_error_ = error;
                result = UINT32_MAX;
            } else {
                current.status = ThreadStatus::Waiting;
                current.wait = {};
                current.wait.handles = {arg(1)};
                current.wait.alertable = u32(3) != 0;
                current.wait.deadline = native_deadline(
                    u32(2) == UINT32_MAX ? UINT64_MAX : runtime_.clock_ticks + std::uint64_t{u32(2)} * 10000);
                schedule(state, memory);
                return std::nullopt;
            }
        }
    } else if (symbol == "SwitchToThread" || symbol == "Sleep" || symbol == "SleepEx") {
        const bool sleep = symbol != "SwitchToThread", alertable = symbol == "SleepEx" && u32(1);
        if (sleep && (u32(0) || alertable)) {
            auto &thread = threads_.items.at(threads_.current);
            thread.status = ThreadStatus::Waiting;
            thread.wait = {};
            thread.wait.kind = ThreadWaitKind::Sleep;
            thread.wait.alertable = alertable;
            auto delay=std::uint64_t{u32(0)}*10000;
            // 明確忽略高解析度要求時採用 15.625 ms 粗粒度；保留 timeBeginPeriod 的原請求，解除時恢復。
            if((runtime_.process_power_control&runtime_.process_power_state&4U) && delay)delay=(delay+156249)/156250*156250;
            thread.wait.deadline =u32(0)==UINT32_MAX?UINT64_MAX:runtime_.clock_ticks+delay;
            schedule(state, memory);
            return std::nullopt;
        }
        if (!sleep)
            result = std::any_of(threads_.items.begin(), threads_.items.end(),
                                 [&](const auto &entry) {
                                     return entry.first != threads_.current &&
                                            entry.second.status == ThreadStatus::Runnable && !entry.second.suspended;
                                 })
                         ? 1U
                         : 0U;
        threads_.yield = true;
    } else if (symbol.starts_with("WaitFor")) {
        const bool multiple = symbol.find("Multiple") != std::string_view::npos;
        const auto count = multiple ? u32(0) : 1U, timeout = u32(multiple ? 3 : 1);
        ThreadWait wait;
        wait.all = multiple && u32(2);
        wait.alertable = symbol.ends_with("Ex") && u32(multiple ? 4 : 2);
        wait.deadline =
            native_deadline(timeout == UINT32_MAX ? UINT64_MAX : runtime_.clock_ticks + std::uint64_t{timeout} * 10000);
        if (!count || count > 64) {
            last_error_ = 87;
            result = UINT32_MAX;
        } else {
            for (unsigned i = 0; i < count; ++i)
                wait.handles.push_back(
                    multiple ? memory.load(GuestAddress{arg(1) + i * (x86_ ? 4U : 8U)}, x86_ ? 32 : 64) : arg(0));
            bool valid = true;
            for (std::size_t i = 0; i < wait.handles.size(); ++i)
                if ((!runtime_.wait_objects.contains(wait.handles[i]) && !thread_id(wait.handles[i]) &&
                     !process_signaled(wait.handles[i]).has_value() &&
                     !file_io_signaled(wait.handles[i]).has_value()) ||
                    std::find(wait.handles.begin(), wait.handles.begin() + static_cast<std::ptrdiff_t>(i),
                              wait.handles[i]) != wait.handles.begin() + static_cast<std::ptrdiff_t>(i))
                    valid = false;
            if (!valid) {
                last_error_ = 6;
                result = UINT32_MAX;
            } else if (std::any_of(wait.handles.begin(), wait.handles.end(), [&](auto handle) {
                           const auto object = runtime_.wait_objects.find(handle);
                           return (object != runtime_.wait_objects.end() && !(object->second.access & 0x100000)) ||
                                  ((thread_id(handle) || process_object_type(handle)==u"Thread") && !(thread_access(handle) & 0x100000));
                       })) {
                last_error_ = 5;
                result = UINT32_MAX;
            } else {
                auto &thread = threads_.items.at(threads_.current);
                thread.wait = std::move(wait);
                thread.status = ThreadStatus::Waiting;
                schedule(state, memory);
                return std::nullopt;
            }
        }
    } else if (symbol == "SetWaitableTimer" || symbol == "SetWaitableTimerEx" || symbol == "CancelWaitableTimer") {
        const auto found = runtime_.wait_objects.find(arg(0));
        if (found == runtime_.wait_objects.end() || found->second.kind != WaitObject::Kind::Timer)
            fail(6);
        else if (!(found->second.access & 2U))
            fail(5);
        else if (symbol == "CancelWaitableTimer") {
            runtime_.wait_state(found->second).active = false;
            runtime_.wait_state(found->second).absolute_due=0;
            cancel_waitable_timer_apcs(found->second.identity);
            result = 1;
        } else if (std::bit_cast<std::int32_t>(u32(2)) < 0)
            fail(87);
        else {
            std::u16string reason;const auto error=symbol=="SetWaitableTimerEx"?read_power_reason(memory,arg(5),reason):0U;
            if(error){fail(error);arg.return_to(state,0);return std::nullopt;}
            if(arg(3) && !handles(GuestAddress{arg(3)}))memory.check(GuestAddress{arg(3)},1,Access::Execute);
            const auto due = memory.load(GuestAddress{arg(1)}, 64);
            auto &timer = runtime_.wait_state(found->second);
            cancel_waitable_timer_apcs(found->second.identity);
            timer.absolute_due=due>>63?0:due;
            const auto interval=~due+1;
            timer.timer_nominal_due=due>>63?(interval>UINT64_MAX-runtime_.clock_ticks?UINT64_MAX:runtime_.clock_ticks+interval):runtime_.system_deadline(due);
            timer.timer_tolerance=symbol=="SetWaitableTimerEx"?u32(6):0;timer.due=coalesce_waitable_timer(timer);
            timer.timer_owner=arg(3)?threads_.current:0;timer.timer_routine=arg(3);timer.timer_argument=arg(4);timer.timer_reason=std::move(reason);
            timer.period = std::uint64_t{u32(2)} * 10000;
            timer.active = true;
            timer.count = 0;
            result = 1;
            if(symbol=="SetWaitableTimer" && arg(5))last_error_=50; // 仍設置 timer，但此後端不支援宿主睡眠喚醒。
        }
    } else {
        const auto id = thread_id(arg(0));
        if (!id) {
            fail(6);
            if (symbol == "SuspendThread" || symbol == "ResumeThread")
                result = UINT32_MAX;
        } else {
            auto &thread = threads_.items.at(id);
            if (symbol == "GetThreadId")
                result = thread_identifier(id);
            else if (symbol == "GetProcessIdOfThread")
                result = process_id(); // 執行緒 handle 只涵蓋本程序的執行緒。
            else if (symbol == "GetExitCodeThread") {
                memory.store(GuestAddress{arg(1)}, 32, thread.status == ThreadStatus::Exited ? thread.exit_code : 259);
                result = 1;
            } else
                throw HostCallFault("未知的執行緒服務");
        }
    }
    arg.return_to(state, result);
    return std::nullopt;
}
} // namespace x64arm
