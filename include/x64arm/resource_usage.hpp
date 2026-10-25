#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace x64arm {
// 各引擎程序獨立取樣；子程序繼承同一報告群組，由 Launcher 彙總，不混入其他程式或主機負載。
bool resource_usage_enabled() noexcept;
void record_network_transfer(std::uint64_t received, std::uint64_t sent) noexcept;
void record_gpu_interval(double start, double end) noexcept;
// 常駐診斷：以 Launcher 已收集的每程序用量快照輸出（"diagnostics" 物件）；未啟用用量報告時不記錄。
// Launcher 會剝除 X64ARM_* 環境變數，這是正式啟動流程中唯一能取得呈現路徑狀態的管道。
void record_diagnostic(const char* key, std::int64_t delta = 1) noexcept;
void set_diagnostic(const char* key, std::string_view text) noexcept;
// 報告器只提出要求；協調者在既有服務邊界取樣，背景執行緒不碰映射或 heap 容器。
// 固定大小且只留最新一份；各欄為不同分類，不能相加當成宿主 RSS。
struct GuestMemoryUsage {
    std::uint64_t committed{},private_commit{},peak_committed{},peak_private_commit{},virtual_size{};
    std::uint64_t service_heap_bytes{},service_heap_blocks{},service_heap_largest{};
    std::uint64_t large_heap_bytes{},large_heap_blocks{},quick_heap_bytes{},quick_heap_blocks{},small_heap_mapped{};
};
bool take_guest_memory_sample_request() noexcept;
void record_guest_memory_usage(const GuestMemoryUsage&) noexcept;
void mark_gpu_timing_unavailable() noexcept;
double take_gpu_busy_seconds(double start, double end);
// D3DKMT 節點統計：第一次查詢後開始累計本程序 Metal command buffer 的 GPU 執行時間（100 ns）。
void enable_gpu_statistics() noexcept;
bool gpu_statistics_enabled() noexcept;
std::uint64_t gpu_running_time() noexcept;

class ResourceUsageReporter {
public:
    explicit ResourceUsageReporter(const std::filesystem::path& path);
    ~ResourceUsageReporter();
    ResourceUsageReporter(const ResourceUsageReporter&) = delete;
    ResourceUsageReporter& operator=(const ResourceUsageReporter&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool root_{};
};
}
