#pragma once
#include <cstdint>
#include <span>

namespace x64arm {
struct GraphicsIndexRange { std::uint32_t minimum{},maximum{}; };
GraphicsIndexRange graphics_index_range(std::span<const std::uint8_t> data,unsigned width);
} // namespace x64arm
