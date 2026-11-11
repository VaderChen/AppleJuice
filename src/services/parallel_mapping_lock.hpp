#pragma once
#include <array>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace x64arm::detail {
// worker 服務與協調者本地服務之間的映射鎖（讀多寫少）。讀端只改自己所在讀者槽的計數（獨立快取線），
// 寫端設定寫者旗標後等全部讀者槽歸零。取代 libc++ std::shared_mutex：它的讀鎖也要先取內部 std::mutex，
// 所有 worker 的服務因此排成一列（8 vCPU 的 QPC 熱迴圈取樣，93% 的互斥等待在 lock_shared／unlock_shared）。
// 語義與 std::shared_mutex 相同：同一執行緒可巢狀取讀鎖；寫鎖不可重入，持讀鎖時也不可再取寫鎖。
// 讀寫雙方都以 seq_cst 先公告自己、再檢查對方（Dekker），不可能同時進入。
class ParallelMappingLock {
public:
    // 統計（只在 X64ARM_SERVICE_COUNTS 設定時計時）：寫者取鎖次數、等讀者排空與持鎖時間；讀者遇到寫者的次數、等待時間與讓出次數。
    // 由 dump_service_counts 輸出，用來分辨 worker 服務的讀鎖等待是持鎖時間長還是取鎖次數多造成。
    struct Stats {std::atomic<std::uint64_t> writer_locks{},writer_drain_ns{},writer_hold_ns{},reader_waits{},reader_wait_ns{},reader_yields{};};
    static Stats& stats() noexcept {static Stats value;return value;}
    static bool timing() noexcept {static const bool value=[] {const auto* text=std::getenv("X64ARM_SERVICE_COUNTS");return text && *text;}();return value;}
    void lock_shared() noexcept {
        auto& reader=readers_[slot()];
        // 巢狀讀鎖：外層已讓寫者等待，直接累加，不可再讓步（否則與等待外層釋放的寫者互等）。
        if(depth_++){reader.count.fetch_add(1,std::memory_order_relaxed);return;}
        for(;;){
            reader.count.fetch_add(1,std::memory_order_seq_cst);
            if(!writer_.load(std::memory_order_seq_cst))return;
            reader.count.fetch_sub(1,std::memory_order_release);
            const auto started=timing()?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
            unsigned spins=0;std::uint64_t since=0;for(;writer_.load(std::memory_order_acquire);++spins)backoff(spins,since);
            if(timing()){auto& s=stats();s.reader_waits.fetch_add(1,std::memory_order_relaxed);
                s.reader_wait_ns.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count()),std::memory_order_relaxed);
                if(spins>=256)s.reader_yields.fetch_add(spins-255,std::memory_order_relaxed);}
        }
    }
    void unlock_shared() noexcept {
        --depth_;readers_[slot()].count.fetch_sub(1,std::memory_order_release);
    }
    // 是否有寫者正在等讀者排空：長時間持讀鎖的一方（D3D11 串流重播的整批上鎖）據此提早放開。只是提示，不保證同步。
    bool writer_waiting() const noexcept {return writer_.load(std::memory_order_relaxed)!=0;}
    // 這條執行緒是否持獨占鎖（等背景工作前核對，避免與被擋住的讀者互等）。
    bool held_exclusive() const noexcept {return exclusive_;}
    void lock() noexcept {
        const auto started=timing()?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        writers_.lock();exclusive_=true;
        writer_.store(1,std::memory_order_seq_cst);
        std::uint64_t since=0;
        for(auto& reader:readers_)
            for(unsigned spins=0;reader.count.load(std::memory_order_seq_cst);++spins)backoff(spins,since);
        if(timing()){hold_started_=std::chrono::steady_clock::now();
            stats().writer_drain_ns.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(hold_started_-started).count()),std::memory_order_relaxed);}
    }
    void unlock() noexcept {
        if(timing()){auto& s=stats();s.writer_locks.fetch_add(1,std::memory_order_relaxed);
            s.writer_hold_ns.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-hold_started_).count()),std::memory_order_relaxed);}
        writer_.store(0,std::memory_order_release);
        exclusive_=false;writers_.unlock();
    }
private:
    static constexpr std::size_t reader_slots=64;
    struct alignas(128) Reader {std::atomic<std::uint32_t> count{};};
    // 依宿主執行緒首次使用的順序分配讀者槽；worker 池最多 64 條，超過時共用槽只影響擴展性，不影響正確性。
    static std::size_t slot() noexcept {
        static std::atomic<std::size_t> next{};
        thread_local const std::size_t index=next.fetch_add(1,std::memory_order_relaxed)%reader_slots;
        return index;
    }
    // 等待對方時先自旋 256 次 yield 指令（約零點幾微秒），之後以 sched_yield 讓出。sched_yield 會壓低優先序，同核有其他
    // 可執行緒時可能被排出一整個時間片；寫者排空期間旗標已設，新讀者也進不來。因此 256 次之後改以 cntvct 計時、isb 間隔
    // 自旋到上限（預設 30 µs，X64ARM_MAPPING_LOCK_SPIN_US=<微秒>；0 恢復直接讓出）才讓出：雙人成行操作中讀者等待 9.5% → 7.8%
    //（每次 6.7 → 5.2 µs）。since 為本次等待進入限時自旋的計數器值。
    static std::uint64_t spin_limit() noexcept {
#if defined(__aarch64__)
        static const std::uint64_t value=[] {
            const auto* text=std::getenv("X64ARM_MAPPING_LOCK_SPIN_US");
            const std::uint64_t microseconds=text && *text?static_cast<std::uint64_t>(std::strtoull(text,nullptr,10)):30U;
            std::uint64_t frequency=0;asm volatile("mrs %0, cntfrq_el0":"=r"(frequency));
            return microseconds*frequency/1000000U;
        }();
        return value;
#else
        return 0;
#endif
    }
    static void backoff(unsigned spins,std::uint64_t& since) noexcept {
#if defined(__aarch64__)
        if(spins<256){__builtin_arm_yield();return;}
        if(const auto limit=spin_limit()){
            std::uint64_t now=0;asm volatile("mrs %0, cntvct_el0":"=r"(now));
            if(spins==256)since=now;
            if(now-since<limit){__builtin_arm_isb(15);return;}
        }
#else
        (void)since;if(spins<256)return;
#endif
        std::this_thread::yield();
    }
    std::array<Reader,reader_slots> readers_{};
    alignas(128) std::atomic<std::uint32_t> writer_{};
    std::chrono::steady_clock::time_point hold_started_{}; // 只由持鎖者讀寫
    std::mutex writers_;
    // 只有單一實例（parallel_mapping_mutex），巢狀深度以執行緒區域變數記錄即可。
    static inline thread_local unsigned depth_{};
    static inline thread_local bool exclusive_{};
};
// worker 服務入口持有的映射共用鎖（其他路徑為空）：D3D11 繪製前置路徑通過資格後可提早放開，之後不再碰客體記憶體。
// 放開後才失敗時拋 D3D11RelockMapping：完整路徑要讀客體記憶體，由 worker 入口依鎖序（映射 → 圖形）重新取鎖後再呼叫一次。
ParallelMappingLock& parallel_mapping_mutex() noexcept;
std::shared_lock<ParallelMappingLock>*& d3d11_worker_mapping_guard() noexcept;
struct D3D11RelockMapping {};
}
