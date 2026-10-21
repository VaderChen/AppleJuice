#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace x64arm::d3dx_mesh {
struct Optimized {
    std::vector<std::uint32_t> indices, attributes, adjacency, faces, vertices;
};
std::uint32_t optimize(std::span<const std::uint32_t> indices,unsigned vertices,std::span<const std::uint32_t> adjacency,std::span<const std::uint32_t> attributes,unsigned flags,Optimized& output);
std::uint32_t validate(std::span<const std::uint32_t> indices,unsigned vertices,std::span<const std::uint32_t> adjacency,std::string& messages);
std::uint32_t clean(std::vector<std::uint32_t>& indices,unsigned vertices,std::vector<std::uint32_t>& adjacency,std::span<const std::uint32_t> attributes,std::vector<std::uint32_t>& duplicates,bool bowties);
}
