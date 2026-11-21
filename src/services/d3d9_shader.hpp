#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace x64arm {
std::vector<std::array<std::uint32_t,2>> graphics_legacy_semantics(std::span<const std::uint8_t> bytes,bool output);
// 回傳整條指令的 DWORD 數，含指令自身。版本 token 由呼叫端處理。
std::optional<unsigned> legacy_shader_instruction_words(std::uint32_t version,std::uint32_t token);
std::optional<std::string> legacy_shader_validation_error(std::span<const std::uint8_t> bytes);
}
