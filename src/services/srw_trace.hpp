#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace x64arm::detail {
// 診斷：X64ARM_TRACE_SRW=<目錄> 時統計進入服務層的 SRW 慢路徑在 worker 與協調者各情況的次數，
// 約每 2 秒附加一列累計值至 srw-<pid>.log。未設定時只多一次靜態指標檢查。
enum class SrwEvent : unsigned {
    WorkerAcquire,WorkerSpinAcquire,WorkerTryFail,WorkerRelease,WorkerDefer,
    HostAcquire,HostTryFail,HostQueue,HostRelease,HostReleaseWake,Count
};
struct SrwTrace {
    const char* directory=std::getenv("X64ARM_TRACE_SRW");
    std::array<std::atomic<std::uint64_t>,static_cast<std::size_t>(SrwEvent::Count)> counts{};
    std::atomic<long long> last{};
    void write(long long now) noexcept {
        const auto path=std::string(directory)+"/srw-"+std::to_string(::getpid())+".log";
        if(auto* out=std::fopen(path.c_str(),"a")){
            const auto at=[&](SrwEvent e){return static_cast<unsigned long long>(counts[static_cast<std::size_t>(e)].load(std::memory_order_relaxed));};
            std::fprintf(out,"srw unix_ms=%lld worker_acquire=%llu worker_spin_acquire=%llu worker_try_fail=%llu worker_release=%llu worker_defer=%llu "
                "host_acquire=%llu host_try_fail=%llu host_queue=%llu host_release=%llu host_release_wake=%llu\n",now,
                at(SrwEvent::WorkerAcquire),at(SrwEvent::WorkerSpinAcquire),at(SrwEvent::WorkerTryFail),at(SrwEvent::WorkerRelease),
                at(SrwEvent::WorkerDefer),at(SrwEvent::HostAcquire),at(SrwEvent::HostTryFail),at(SrwEvent::HostQueue),
                at(SrwEvent::HostRelease),at(SrwEvent::HostReleaseWake));
            std::fclose(out);
        }
    }
    static long long now() noexcept {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }
    // 正常結束時補寫最後累計；被強制結束的程序只留下週期列。
    ~SrwTrace(){if(directory && last.load())write(now());}
};
inline void record_srw(SrwEvent event) noexcept {
    static SrwTrace trace;
    if(!trace.directory)return;
    trace.counts[static_cast<std::size_t>(event)].fetch_add(1,std::memory_order_relaxed);
    const auto now=SrwTrace::now();
    auto previous=trace.last.load(std::memory_order_relaxed);
    if(now-previous<2000 || !trace.last.compare_exchange_strong(previous,now))return;
    trace.write(now);
}
}
