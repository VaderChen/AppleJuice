#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace x64arm::detail {
// worker 服務與協調者本地服務之間的映射鎖（讀多寫少）。讀端只改自己所在讀者槽的計數（獨立快取線），
// 寫端設定寫者旗標後等全部讀者槽歸零。取代 libc++ std::shared_mutex：它的讀鎖也要先取內部 std::mutex，
// 所有 worker 的服務因此排成一列（8 vCPU 的 QPC 熱迴圈取樣，93% 的互斥等待在 lock_shared／unlock_shared）。
// 語義與 std::shared_mutex 相同：同一執行緒可巢狀取讀鎖；寫鎖不可重入，持讀鎖時也不可再取寫鎖。
// 讀寫雙方都以 seq_cst 先公告自己、再檢查對方（Dekker），不可能同時進入。
class ParallelMappingLock {
public:
    void lock_shared() noexcept {
        auto& reader=readers_[slot()];
        // 巢狀讀鎖：外層已讓寫者等待，直接累加，不可再讓步（否則與等待外層釋放的寫者互等）。
        if(depth_++){reader.count.fetch_add(1,std::memory_order_relaxed);return;}
        for(;;){
            reader.count.fetch_add(1,std::memory_order_seq_cst);
            if(!writer_.load(std::memory_order_seq_cst))return;
            reader.count.fetch_sub(1,std::memory_order_release);
            for(unsigned spins=0;writer_.load(std::memory_order_acquire);++spins)backoff(spins);
        }
    }
    void unlock_shared() noexcept {
        --depth_;readers_[slot()].count.fetch_sub(1,std::memory_order_release);
    }
    void lock() noexcept {
        writers_.lock();
        writer_.store(1,std::memory_order_seq_cst);
        for(auto& reader:readers_)
            for(unsigned spins=0;reader.count.load(std::memory_order_seq_cst);++spins)backoff(spins);
    }
    void unlock() noexcept {
        writer_.store(0,std::memory_order_release);
        writers_.unlock();
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
    static void backoff(unsigned spins) noexcept {
        if(spins<256){
#if defined(__aarch64__)
            __builtin_arm_yield();
#endif
        }else std::this_thread::yield();
    }
    std::array<Reader,reader_slots> readers_{};
    alignas(128) std::atomic<std::uint32_t> writer_{};
    std::mutex writers_;
    // 只有單一實例（parallel_mapping_mutex），巢狀深度以執行緒區域變數記錄即可。
    static inline thread_local unsigned depth_{};
};
}
