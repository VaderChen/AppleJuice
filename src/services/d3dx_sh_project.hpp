#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace x64arm {
class NativeGraphics;
struct GraphicsResource;
struct GraphicsShader;
using ShCubeCoefficients=std::array<std::array<float,36>,3>;
using ShCubeShaders=std::array<std::shared_ptr<GraphicsShader>,2>;

std::array<float,4> d3dx_cube_direction(unsigned face,float u,float v);

class ShCubeProjection {
public:
    enum class Weighting { TexelSolidAngle, NormalizedDifferential };
    ShCubeProjection(unsigned order,Weighting weighting):order_(order),weighting_(weighting){}
    void add_face(unsigned face,unsigned edge,std::uint32_t format,std::span<const std::uint8_t> pixels);
    ShCubeCoefficients coefficients() const;
private:
    unsigned order_;
    Weighting weighting_;
    double weight_{};
    std::array<std::array<double,36>,3> sums_{};
};

std::optional<ShCubeCoefficients> sh_project_cube_metal(NativeGraphics& native,
    const std::shared_ptr<GraphicsResource>& cube,unsigned order,ShCubeShaders& shaders);
}
