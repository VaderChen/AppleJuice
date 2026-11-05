#pragma once
#include "x64arm/memory.hpp"
#include "x64arm/windows_runtime.hpp"
namespace x64arm::mf_image {
inline constexpr std::uint64_t image_offset=0xe0000;
void initialize(GuestMemory& memory,std::map<std::uint64_t,RuntimeImage>& images,bool x86,std::uint64_t gateway);
}
