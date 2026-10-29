#pragma once
#include "x64arm/memory.hpp"
#include <span>
#include <vector>

namespace x64arm {
// SEC_IMAGE 的純資料布局；不建立程序、不解析匯入、不執行重定位或入口。
struct PeSectionRange {
    std::uint32_t rva{},size{};
    Permissions permissions{};
    bool shared{};
    std::vector<std::uint8_t> bytes;
};
struct PeSectionImage {
    std::uint64_t preferred_base{},stack_reserve{},stack_commit{};
    std::uint32_t image_size{},entry{},subsystem{},subsystem_version{},os_version{};
    std::uint32_t characteristics{},dll_characteristics{},machine{},contains_code{},loader_flags{},file_size{},checksum{};
    std::vector<PeSectionRange> ranges;
};
PeSectionImage parse_pe_section_image(std::span<const std::uint8_t> bytes,std::size_t memory_limit);
}
