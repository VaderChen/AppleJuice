#pragma once
#include "crt_uchar.h"
#include <optional>
#include <string_view>

namespace x64arm {
inline std::optional<CrtUcharFunction> crt_uchar_function(std::string_view name) {
    if (name == "mbrtoc16") return CrtMbrtoc16;
    if (name == "mbrtoc32") return CrtMbrtoc32;
    if (name == "c16rtomb") return CrtC16rtomb;
    if (name == "c32rtomb") return CrtC32rtomb;
    return std::nullopt;
}
} // namespace x64arm
