#pragma once
#include <cstdint>
namespace x64arm {
// 內部取樣 view：原始深度紋理把 R 值複製到 RGBA；不與公開 UAV 旗標重疊。
inline constexpr std::uint32_t graphics_view_replicate_red=0x10000000U;
inline constexpr std::uint32_t graphics_view_red_only=0x20000000U;
}
