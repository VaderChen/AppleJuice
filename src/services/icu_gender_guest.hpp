#pragma once
#include "x64arm/windows_address_layout.hpp"

namespace x64arm::icu_gender {
constexpr std::uint64_t image_address(bool x86) noexcept {
    return windows_address_layout::private_code_address(x86?ExecutionMode::X86:ExecutionMode::X64,22);
}
}
