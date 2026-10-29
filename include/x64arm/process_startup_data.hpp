#pragma once
#include <cstdint>
#include <array>
#include <optional>
#include <vector>
namespace x64arm {
struct NativeProcessParameters {
    std::array<std::vector<std::uint8_t>,9> strings;
    std::array<std::uint32_t,9> window{};
    std::array<std::uint64_t,3> standard_handles{};
    std::uint32_t flags{1},console_flags{};
};
struct ProcessStartupData {
    std::vector<std::uint8_t> reserved2;unsigned pointer_bytes{};
    std::optional<NativeProcessParameters> native;
};
// 引擎私有傳輸保留原始 STARTUPINFO bytes 與來源 ABI；不是宿主路徑或 Windows HANDLE。
int create_process_startup_descriptor(const ProcessStartupData&,int minimum_descriptor);
ProcessStartupData read_process_startup_descriptor(int);
}
