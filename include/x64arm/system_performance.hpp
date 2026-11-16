#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace x64arm {
// GetPerformanceInfo 的十個 SIZE_T 欄位，統一以 4 KiB 客體頁表示。
// swap_*：宿主置換空間（對應 Windows 分頁檔），同為 4 KiB 頁；EnumPageFiles 使用。
struct SystemPerformance {std::array<std::uint64_t,10> pages{};std::uint64_t swap_total_pages{},swap_used_pages{};std::uint32_t error{};};
// 回報給客體的處理器標稱時脈（MHz）：ETW 記錄檔與閒置週期換算共用。
inline constexpr std::uint32_t windows_processor_mhz=2400;
SystemPerformance native_system_performance();
struct MemoryResourceState {bool low{},high{};std::uint32_t error{};};
MemoryResourceState system_memory_resource_state(const SystemPerformance&);
struct SystemCpuTime {std::uint64_t idle{},kernel{},user{};std::uint32_t error{};};
SystemCpuTime native_system_cpu_time();
// 各宿主邏輯處理器自開機以來的時間（100 ns），欄位語義與 native_system_cpu_time 相同；失敗回傳空集合。
std::vector<SystemCpuTime> native_processor_cpu_times();
}
