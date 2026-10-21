#pragma once
#include "x64arm/windows_keyboard.hpp"
namespace x64arm {
inline bool windows_input_layout_supported(std::uint64_t layout){
    if(!layout || layout==0x04090409)return true;
    const auto low=static_cast<std::uint32_t>(layout);
    if(layout>>32 && (!(low&0x80000000U) || layout>>32!=UINT32_MAX))return false;
    return native_input_layout_supported(low);
}
}
