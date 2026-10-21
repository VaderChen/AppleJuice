#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace x64arm {
struct GlslSource { std::uint32_t stage{};std::string source;std::string preamble{}; };
struct GlslAttribute { std::string name;std::uint32_t location{},components{},columns{1}; };
struct GlslUniform {
    std::string name;
    std::uint32_t type{},components{1},columns{1},count{1},offset{},array_stride{},matrix_stride{};
};
struct GlslSampler { std::string name;std::uint32_t texture{},sampler{},count{1}; };
struct GlslStage {
    std::uint32_t stage{},buffer_index{};
    std::string msl,flipped_msl,entry,clip_mask,point_size_enabled;
    std::vector<GlslAttribute> attributes;
    std::vector<GlslUniform> uniforms;
    std::vector<GlslSampler> samplers;
    std::vector<std::uint8_t> initial_uniforms;
};
struct GlslResult { bool success{};std::string log;std::vector<GlslStage> stages; };
// validate 採 OpenGL 語義；link 跨 stage 配置介面並產生 Metal 與反射資料。
GlslResult validate_glsl(const GlslSource& source);
GlslResult link_glsl(std::span<const GlslSource> sources);
}
