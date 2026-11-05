#pragma once
#include "x64arm/memory.hpp"
#include "x64arm/windows_runtime.hpp"
namespace x64arm::mf_buffer {
inline constexpr std::uint64_t image_offset=0xc0000;
void initialize(GuestMemory&,std::map<std::uint64_t,RuntimeImage>&,bool,std::uint64_t);
}
