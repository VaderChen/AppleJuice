#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace x64arm {
// GetPerformanceInfo 的十個 SIZE_T 欄位，統一以 4 KiB 客體頁表示。
struct SystemPerformance {std::array<std::uint64_t,10> pages{};std::uint32_t error{};};
SystemPerformance native_system_performance();
struct MemoryResourceState {bool low{},high{};std::uint32_t error{};};
MemoryResourceState system_memory_resource_state(const SystemPerformance&);
struct SystemCpuTime {std::uint64_t idle{},kernel{},user{};std::uint32_t error{};};
SystemCpuTime native_system_cpu_time();
// 各宿主邏輯處理器自開機以來的時間（100 ns），欄位語義與 native_system_cpu_time 相同；失敗回傳空集合。
std::vector<SystemCpuTime> native_processor_cpu_times();
}
