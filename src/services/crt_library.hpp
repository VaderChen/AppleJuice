#pragma once
#include <array>
#include <string_view>
namespace x64arm {
// Release CRT 的共用 provider；不把任意版本或 debug CRT 名稱視為相同 ABI。
inline constexpr std::array<std::string_view,7> legacy_crt_libraries{
    "msvcr70.dll","msvcr71.dll","msvcr80.dll","msvcr90.dll",
    "msvcr100.dll","msvcr110.dll","msvcr120.dll"};
inline bool legacy_crt_library(std::string_view name) {
    for(const auto library:legacy_crt_libraries)if(name==library)return true;
    return false;
}
inline constexpr std::array<std::string_view,4> modern_crt_libraries{
    "msvcrt.dll","ucrtbase.dll","vcruntime140.dll","vcruntime140_1.dll"};
inline bool crt_library(std::string_view name) {
    for(const auto library:modern_crt_libraries)if(name==library)return true;
    return name.starts_with("api-ms-win-crt-") || legacy_crt_library(name);
}
inline bool legacy_32bit_time(std::string_view name) {
    return name=="msvcrt.dll" || name=="msvcr70.dll" || name=="msvcr71.dll";
}
}
