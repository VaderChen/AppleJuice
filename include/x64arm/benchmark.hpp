#pragma once
#include <filesystem>
#include <ostream>
namespace x64arm {
void benchmark_core(const std::filesystem::path& fixtures, unsigned samples, std::ostream& output);
}
