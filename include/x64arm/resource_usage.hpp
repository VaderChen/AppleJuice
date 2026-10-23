#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>

namespace x64arm {
// 各引擎程序獨立取樣；子程序繼承同一報告群組，由 Launcher 彙總，不混入其他程式或主機負載。
bool resource_usage_enabled() noexcept;
void record_network_transfer(std::uint64_t received, std::uint64_t sent) noexcept;
void record_gpu_interval(double start, double end) noexcept;
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
