#pragma once
#include <cstdint>
#include <array>
#include <optional>
namespace x64arm::crt_c_locale {
// 同一分類表供 CRT 的直接查詢、wctype 遮罩及 __pctype_func 使用。
inline constexpr auto character_masks=[] {
    std::array<std::uint16_t,128> result{};
    for(unsigned c=0;c<result.size();++c){
        const bool upper=c>='A' && c<='Z',lower=c>='a' && c<='z',digit=c>='0' && c<='9';
        unsigned mask=upper?1U:lower?2U:digit?4U:0U;
        if(c==' ' || (c>=9 && c<=13))mask|=8;
        if(c>32 && c<127 && !upper && !lower && !digit)mask|=16;
        if(c<32 || c==127)mask|=32;
        if(c==' ')mask|=64;
        if(digit || (c>='a' && c<='f') || (c>='A' && c<='F'))mask|=128;
        result[c]=static_cast<std::uint16_t>(mask);
    }
    return result;
}();
inline constexpr std::uint16_t character_mask(std::uint32_t value){
    return value<character_masks.size()?character_masks[value]:0;
}
// 既有 CRT C／POSIX locale 的單位元組轉換；不混用 Windows ACP。
inline std::optional<std::uint8_t> encode(char16_t value){
    if(value>255)return std::nullopt;
    return static_cast<std::uint8_t>(value);
}
}
