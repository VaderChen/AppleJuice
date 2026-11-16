#pragma once
// GLU tessellator 的宿主端三角化結果（windows_glu_tess.cpp），供不需經客體回呼的使用者（如 wglUseFontOutlines）直接取用。
// 交點頂點直接帶入計算出的座標；三角形相對法線為逆時針，loops 為 BOUNDARY_ONLY 時的邊界（內部在左側）。
#include "x64arm/windows_opengl.hpp"
#include <array>
#include <optional>
#include <vector>
namespace x64arm {
struct GluTriangulation {
    std::vector<std::array<double,3>> positions;
    std::vector<std::array<std::size_t,3>> triangles;
    std::vector<std::vector<std::size_t>> loops;
};
std::optional<GluTriangulation> glu_triangulate(const GluTessellator&);
}
