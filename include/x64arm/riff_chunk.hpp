#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>

namespace x64arm::riff {
inline constexpr std::uint32_t riff_id=0x46464952, list_id=0x5453494c;
inline std::uint32_t word(std::span<const std::uint8_t> bytes){
    if(bytes.size()<4)throw std::invalid_argument("RIFF DWORD 截斷");
    return std::uint32_t{bytes[0]}|(std::uint32_t{bytes[1]}<<8)|(std::uint32_t{bytes[2]}<<16)|(std::uint32_t{bytes[3]}<<24);
}
struct Chunk {std::uint32_t id{},size{};std::uint64_t data{},next{};};
// MMIO 與 ANI 使用相同的大小、父容器界線及偶數對齊檢查。
inline Chunk chunk(std::span<const std::uint8_t> header,std::uint64_t offset,std::uint64_t limit){
    if(header.size()<8 || offset>limit || limit-offset<8)throw std::invalid_argument("RIFF chunk 標頭截斷");
    const auto size=word(header.subspan(4));const auto padded=std::uint64_t{size}+(size&1U);
    if(padded>limit-offset-8)throw std::invalid_argument("RIFF chunk 內容或對齊截斷");
    return {word(header),size,offset+8,offset+8+padded};
}
}
