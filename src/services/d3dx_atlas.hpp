#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <vector>
namespace x64arm::d3dx_atlas {
using Progress=std::function<std::uint32_t(float)>;
struct Request {
    unsigned mode{},charts{},width{},height{},options{},dimension{},components{},stride{};
    float stretch{},gutter{},frequency{};
    std::vector<std::array<float,3>> positions;
    std::vector<std::array<float,2>> texcoords;
    std::vector<std::uint32_t> indices,adjacency,false_edges;
    std::vector<float> imt,signal;
};
struct Result {
    std::vector<std::array<float,2>> uv;
    std::vector<std::uint32_t> indices,partitions,remap;
    std::vector<float> imt;
    float stretch{};std::uint32_t charts{};
};
std::uint32_t run(const Request&,const Progress&,Result&);
}
