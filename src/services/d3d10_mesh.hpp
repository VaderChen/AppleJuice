#pragma once
#include "x64arm/windows_graphics.hpp"
#include <array>
#include <string>
#include <vector>
namespace x64arm {
struct D3D10MeshData {
    unsigned vertices{},faces{},flags{};
    std::string position;double epsilon{};
    std::vector<GraphicsElement> declaration;
    std::vector<unsigned> strides;
    std::vector<std::array<unsigned,5>> attributes;
};
}
