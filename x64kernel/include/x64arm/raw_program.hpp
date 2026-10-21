#pragma once
#include "x64arm/interpreter.hpp"
#include "x64arm/pe_file_cache.hpp"
#include <filesystem>
namespace x64arm {
struct RawConfig {
    GuestAddress load_address{0x10000};
    GuestAddress entry{0x10000};
    GuestAddress stack_address{0x100000};
    std::size_t stack_size{64 * 1024};
};
struct RawProgram { GuestState state; GuestMemory memory; };
RawProgram prepare_raw(std::span<const std::uint8_t> bytes, RawConfig config = {});
std::vector<std::uint8_t> read_raw_file(const std::filesystem::path& path);
// PE overlay 不映射至客體；檔案讀取上限與客體容量分離。
std::vector<std::uint8_t> read_pe_file(const std::filesystem::path& path);
}
