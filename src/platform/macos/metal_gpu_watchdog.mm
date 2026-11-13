// GPU 卡住防護：所有 Metal 後端在 commit 前經 track_gpu_commands 登記命令緩衝，背景執行緒定期檢查是否完成。
// 命令緩衝送出後超過門檻（預設 5 秒；Windows 的 GPU 逾時偵測與復原為 2 秒）仍未完成，視為 GPU 卡住：
// 先把卡住批次的狀態與繪圖標籤寫到 stderr，再立刻結束程序。卡住的 GPU 工作只能靠擁有者程序結束來釋放；
// 放著不處理會讓 WindowServer 等不到 GPU，最後整台機器被看門狗重開（2026-11-13 RE8 實際發生過）。
// 後端唯一的 GPU 端事件等待是跨 context 的資源交接（graphics_handoff）：發出訊號的命令緩衝在等待者編碼前已提交，
// 等待時間不超過前一批 GPU 工作的執行時間，所以命令緩衝不會合法地在 GPU 上長時間等待。
// 計時用不含系統睡眠的時鐘，睡眠喚醒後不會把在途批次誤判為卡住。
// X64ARM_GPU_HANG_TIMEOUT_MS=<毫秒> 調整門檻，0 關閉（只供診斷對照）。
#include "resource_usage.hpp"

#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace x64arm {
namespace {

std::uint64_t uptime_ns() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

std::uint64_t hang_timeout_ns() {
    static const std::uint64_t value = [] {
        const char* text = std::getenv("X64ARM_GPU_HANG_TIMEOUT_MS");
        if (!text || !*text) return std::uint64_t{5000} * 1000000;
        return std::strtoull(text, nullptr, 10) * 1000000;
    }();
    return value;
}

const char* status_name(MTLCommandBufferStatus status) {
    switch (status) {
    case MTLCommandBufferStatusNotEnqueued: return "not-enqueued";
    case MTLCommandBufferStatusEnqueued: return "enqueued";
    case MTLCommandBufferStatusCommitted: return "committed";
    case MTLCommandBufferStatusScheduled: return "scheduled";
    case MTLCommandBufferStatusCompleted: return "completed";
    case MTLCommandBufferStatusError: return "error";
    }
    return "?";
}

class GpuWatchdog {
public:
    static GpuWatchdog& instance() {
        // 刻意不解構：程序結束時背景執行緒可能仍在等待。
        static GpuWatchdog* watchdog = new GpuWatchdog();
        return *watchdog;
    }
    void add(id<MTLCommandBuffer> command, std::function<std::string()> describe) {
        const std::lock_guard lock(mutex_);
        items_.push_back({command, uptime_ns(), std::move(describe)});
        if (!started_) {
            started_ = true;
            std::thread([this] { run(); }).detach();
        }
        wake_.notify_one();
    }

private:
    struct Item {
        id<MTLCommandBuffer> command;
        std::uint64_t committed{};
        std::function<std::string()> describe;
    };
    static bool finished(const Item& item) { return item.command.status >= MTLCommandBufferStatusCompleted; }

    [[noreturn]] void hang(Item item, std::size_t pending, std::uint64_t age) {
        const auto* label = item.command.label ? item.command.label.UTF8String : nullptr;
        std::string detail = item.describe ? item.describe() : std::string{};
        std::fprintf(stderr,
                     "[gpu-hang] Metal 命令緩衝送出 %.1f 秒仍未完成（狀態 %s、標籤 %s、在途 %zu 個），GPU 卡住；結束程序以釋放 GPU。"
                     "設定 X64ARM_METAL_LABELS=1 可記錄卡住批次的繪圖標籤%s%s\n",
                     static_cast<double>(age) / 1e9, status_name(item.command.status), label ? label : "-", pending,
                     detail.empty() ? "" : "：", detail.c_str());
        std::fflush(stderr);
        // DXGI_ERROR_DEVICE_HUNG（0x887A0006）的低 8 位元，與 Windows 上 GPU 逾時後程序結束的情形對應。
        _exit(6);
    }

    void run() {
        pthread_setname_np("x64arm.gpu.watchdog");
        const auto timeout = hang_timeout_ns();
        std::unique_lock lock(mutex_);
        for (;;) {
            if (items_.empty()) wake_.wait(lock, [&] { return !items_.empty(); });
            else wake_.wait_for(lock, std::chrono::milliseconds(250));
            std::erase_if(items_, finished);
            const auto now = uptime_ns();
            for (const auto& item : items_) {
                if (now - item.committed < timeout) continue;
                // 再確認一次：剛好在檢查之間完成的批次不算卡住。
                if (finished(item)) continue;
                const std::size_t pending = items_.size();
                const std::uint64_t age = now - item.committed;
                auto copy = item;
                lock.unlock();
                hang(std::move(copy), pending, age);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Item> items_;
    bool started_{};
};

} // namespace

void watch_gpu_commands(id<MTLCommandBuffer> commands, std::function<std::string()> describe) {
    if (!commands || !hang_timeout_ns()) return;
    GpuWatchdog::instance().add(commands, std::move(describe));
}

} // namespace x64arm
