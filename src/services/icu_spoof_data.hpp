#pragma once

#include <unicode/parseerr.h>
#include <unicode/utypes.h>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace x64arm::icu_spoof {
inline constexpr std::size_t header_size=96;
using Data=std::vector<std::uint8_t>;

std::size_t serialized_size(std::span<const std::uint8_t> header,std::size_t capacity);
bool valid_data(std::span<const std::uint8_t> bytes);
bool empty_data(std::span<const std::uint8_t> bytes);
Data identity_data();
Data compile_source(std::string_view source,std::int32_t& error_type,UParseError& error,UErrorCode& status);
}
