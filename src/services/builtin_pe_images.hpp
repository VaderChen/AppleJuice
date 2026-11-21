#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace x64arm::detail {
std::span<const std::uint8_t> builtin_pe_bytes(std::string_view module, bool x86);
bool builtin_pe_has_export(std::string_view module, std::string_view symbol, bool x86);
}
