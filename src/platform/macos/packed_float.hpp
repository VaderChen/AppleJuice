#pragma once
#include <bit>
#include <cmath>
#include <cstdint>

namespace x64arm {
// GL_UNSIGNED_INT_10F_11F_11F_REV／Metal RG11B10Float 共用無符號浮點格式。
inline float unpack_unsigned_float(std::uint32_t bits,unsigned fraction) {
    const auto exponent=bits>>fraction,mantissa=bits&((1U<<fraction)-1);
    if(!exponent)return std::ldexp(static_cast<float>(mantissa),-14-static_cast<int>(fraction));
    return std::bit_cast<float>(((exponent==31?255:exponent+112)<<23)|(mantissa<<(23-fraction)));
}
inline std::uint32_t pack_unsigned_float(float value,unsigned fraction) {
    const auto bits=std::bit_cast<std::uint32_t>(value),exponent=(bits>>23)&255,mantissa=bits&0x7fffff;
    if(exponent==255 && mantissa)return (31U<<fraction)|(1U<<(fraction-1));
    if(bits>>31)return 0;
    if(exponent==255)return 31U<<fraction;
    const auto target=static_cast<int>(exponent)-112;
    if(target>=31)return 31U<<fraction;
    const auto shift=target<=0?24-static_cast<int>(fraction)-target:23-static_cast<int>(fraction);
    if(shift>24)return 0;
    const auto significand=mantissa|0x800000U;
    auto rounded=significand>>shift;const auto remainder=significand&((1U<<shift)-1),half=1U<<(shift-1);
    rounded+=(remainder>half || (remainder==half && (rounded&1)));
    return (target>0?(static_cast<unsigned>(target)-1)<<fraction:0)+rounded;
}
inline float unpack_rg11b10(std::uint32_t bits,unsigned channel) {
    return unpack_unsigned_float((bits>>(channel==2?22:channel*11))&(channel==2?1023U:2047U),channel==2?5:6);
}
inline std::uint32_t pack_rg11b10(float r,float g,float b) {
    return pack_unsigned_float(r,6)|(pack_unsigned_float(g,6)<<11)|(pack_unsigned_float(b,5)<<22);
}
}
