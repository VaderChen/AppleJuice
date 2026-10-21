#pragma once
#include <array>
#include <cstdint>
#include <vector>
namespace x64arm {
struct D3D10SpriteItem {
    std::array<float,16> world{};
    std::array<float,2> uv{},size{1,1};
    std::array<float,4> color{1,1,1,1};
    std::uint64_t texture{};
    unsigned slice{};bool retained{};
};
struct D3D10SpriteData {
    std::array<float,16> view{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},projection=view;
    std::vector<D3D10SpriteItem> items;
    unsigned flags{},capacity{512};bool active{};
};
}
