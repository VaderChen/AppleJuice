#pragma once

#include "x64arm/windows_address_layout.hpp"

namespace x64arm::icu_bidi {
inline constexpr std::size_t image_slot=15;
inline constexpr std::size_t image_slots=2;
static_assert(image_slot+image_slots<=windows_address_layout::private_code_slots);

constexpr std::uint64_t image_address(bool x86) noexcept {
    return windows_address_layout::private_code_address(x86?ExecutionMode::X86:ExecutionMode::X64,image_slot);
}
}
