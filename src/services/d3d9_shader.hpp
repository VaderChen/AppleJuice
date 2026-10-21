#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace x64arm {
std::vector<std::array<std::uint32_t,2>> graphics_legacy_semantics(std::span<const std::uint8_t> bytes,bool output);
}
