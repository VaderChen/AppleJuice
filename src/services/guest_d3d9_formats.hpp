#pragma once
#include <array>
#include "x64arm/memory.hpp"
#include "x64arm/windows_runtime.hpp"
namespace x64arm::d3d9_formats {
inline constexpr std::uint64_t image_offset=0xb0000;
void initialize(GuestMemory&,std::map<std::uint64_t,RuntimeImage>&,bool,std::uint64_t,const std::array<std::uint64_t,2>& trig);
}
