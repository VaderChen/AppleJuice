#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>
namespace x64arm {
// Media Foundation 與 D3D11 共用真正資源；回讀／寫回沿用原生 Metal 或 VideoToolbox surface。
struct MediaSurface {
    std::uint32_t row_pitch{},height{},size{},format{};
    std::function<std::vector<std::uint8_t>()> read;
    std::function<void(std::span<const std::uint8_t>)> write;
};
}
