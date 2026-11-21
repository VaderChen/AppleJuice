#pragma once

#include "x64arm/windows_address_layout.hpp"
#include <cstddef>
#include <cstdint>

namespace x64arm::icu_trie {
constexpr std::uint64_t image_address(bool x86) noexcept {
    return windows_address_layout::private_code_address(x86?ExecutionMode::X86:ExecutionMode::X64,0);
}

// UCPTrie 的公開巨集會直接讀取這個結構與兩個客體陣列。
template<class Pointer> struct View {
    Pointer index, data;
    std::int32_t indexLength, dataLength, highStart;
    std::uint16_t shifted12HighStart;
    std::int8_t type, valueWidth;
    std::uint32_t reserved32;
    std::uint16_t reserved16, index3NullOffset;
    std::int32_t dataNullOffset;
    std::uint32_t nullValue;
};
static_assert(sizeof(View<std::uint32_t>)==40 && offsetof(View<std::uint32_t>,dataNullOffset)==32);
static_assert(sizeof(View<std::uint64_t>)==48 && offsetof(View<std::uint64_t>,dataNullOffset)==40);
}
