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
const ProcessorInformation& native_processor_information();
class GuestMemory;
ProcessorInformation guest_processor_information(const GuestMemory&);
}
