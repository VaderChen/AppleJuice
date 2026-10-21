#pragma once
#include <cstdint>
#include <vector>
namespace x64arm {
struct ProcessStartupData {std::vector<std::uint8_t> reserved2;unsigned pointer_bytes{};};
// 引擎私有傳輸保留原始 STARTUPINFO bytes 與來源 ABI；不是宿主路徑或 Windows HANDLE。
int create_process_startup_descriptor(const ProcessStartupData&,int minimum_descriptor);
ProcessStartupData read_process_startup_descriptor(int);
}
