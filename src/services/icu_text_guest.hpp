#pragma once

#include "x64arm/windows_address_layout.hpp"
#include <array>
#include <string_view>

namespace x64arm::icu_text {
constexpr std::uint64_t image_address(bool x86) noexcept {
    return windows_address_layout::private_code_address(x86?ExecutionMode::X86:ExecutionMode::X64,2);
}

inline constexpr std::array<std::string_view,3> heap_imports{"GetProcessHeap","HeapAlloc","HeapFree"};
}
