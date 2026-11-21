#pragma once

#include "x64arm/windows_address_layout.hpp"
#include <cstdlib>
#include <string_view>

namespace x64arm::crtdll_text {
inline constexpr auto image_address=windows_address_layout::private_code_address(ExecutionMode::X86,14);

inline bool resident_enabled(){
    static const bool enabled=[] {
        const auto* value=std::getenv("X64ARM_GUEST_CRTDLL_TEXT");
        return !value || std::string_view(value)!="0";
    }();
    return enabled;
}
}
