#pragma once
#include "x64arm/memory.hpp"
#include <array>
namespace x64arm::wasapi {
using Guid=std::array<std::uint8_t,16>;
constexpr Guid clsid{0x95,0x03,0xde,0xbc,0x2f,0xe5,0x7c,0x46,0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e};
inline bool is_class(const GuestMemory& memory,std::uint64_t p){Guid g{};if(!p)return false;memory.read(GuestAddress{p},g);return g==clsid;}
}
