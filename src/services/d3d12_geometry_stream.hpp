#pragma once
#include "x64arm/windows_graphics.hpp"

namespace x64arm {
// 線段與三角形 GS 的運算介面，沿用現代著色器的 D3D 資源對應表。
// 合成的幾何輸入與輸出緩衝只占 Metal 槽位，不占 D3D 暫存器。
struct GraphicsGeometryStream {
    struct Field {
        std::string semantic;
        uint32_t index{},reg{},component{},width{},component_type{};
        bool input{},position{};
    };
    std::vector<Field> interface;
    GraphicsModernShader shader;
    uint32_t input_slot{},output_slot{},input_vertices{},output_width{},max_vertices{};
    uint32_t input_registers{},output_registers{},output_primitive_words{};
};
GraphicsGeometryStream graphics_modern_geometry_stream(std::span<const uint8_t> bytes);
}
