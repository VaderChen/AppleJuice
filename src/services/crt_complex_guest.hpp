#pragma once
#include "x64arm/windows_address_layout.hpp"

namespace x64arm::crt_complex {
constexpr std::uint64_t image_address(bool x86) noexcept {
    constexpr std::size_t slot=25;
    static_assert(slot<windows_address_layout::private_code_slots);
    return windows_address_layout::private_code_address(x86?ExecutionMode::X86:ExecutionMode::X64,slot);
}
}
