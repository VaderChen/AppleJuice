#pragma once
#include <cstdint>
#include <string>

namespace x64arm {
// 身分與標稱時脈是硬體描述，不代表翻譯後的 x86 指令吞吐量。
// 沒有可靠資料的時脈保留 0；不把 DVFS 上限當成即時運作時脈。
struct ProcessorInformation {
    std::string name{"x64arm virtual AMD64 processor"};
    std::string manufacturer{"x64arm"};
    std::string socket{"Virtual Socket 0"};
    std::uint32_t maximum_mhz{}, nominal_mhz{};
};
// 相容處理器 TSC 的固定計數頻率，與宿主即時／base clock 分開。
// 沿用既有 TSC 政策：已知標稱值 → 已知上限 → 100 ns（10 MHz）。
constexpr std::uint32_t processor_timestamp_mhz(std::uint32_t nominal,std::uint32_t maximum) noexcept {
    return nominal?nominal:(maximum?maximum:10U);
}
const ProcessorInformation& native_processor_information();
class GuestMemory;
ProcessorInformation guest_processor_information(const GuestMemory&);
}
