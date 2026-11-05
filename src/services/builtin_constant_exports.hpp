#pragma once
#include "crt_library.hpp"
#include "msvcp_numeric_limits.hpp"
namespace x64arm::detail {
// 不可變資料與可呼叫服務分別發布；資料保持真正的 PE RVA 及唯讀頁面。
inline std::span<const msvcp_numeric::Constant> builtin_constant_exports(std::string_view family){
    static const msvcp_numeric::Constant crt[]{{"_HUGE",0x7ff0000000000000ULL,8}};
    if(family=="msvcp90.dll")return msvcp_numeric::constants();
    if(crt_library(family))return crt;
    return {};
}
inline constexpr std::uint32_t builtin_constant_rva=4096;
}
