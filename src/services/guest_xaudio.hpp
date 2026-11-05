#pragma once
#include "x64arm/memory.hpp"
#include "x64arm/windows_runtime.hpp"
namespace x64arm::xaudio_lowering {
// callback 首頁之後的獨立 64 KiB 槽；既有小型常式由 0x80000 起。
inline constexpr std::uint64_t image_offset=0x10000,image_capacity=0x10000;
void initialize(GuestMemory&,std::map<std::uint64_t,RuntimeImage>&,bool,std::uint64_t);
}
