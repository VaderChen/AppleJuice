#pragma once
#include <cstdint>
#include <type_traits>
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace x64arm {
// x86 CRC32、ARM CRC32C 與入口雜湊共用反射 Castagnoli 語義；無初值／結尾 XOR。
// bytes 為 1／2／4／8，低位 byte 先處理。常數求值與非 ARM 平台保留相同結果。
inline constexpr std::uint32_t cpu_crc32c(std::uint32_t crc,std::uint64_t data,unsigned bytes) noexcept {
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    if(!std::is_constant_evaluated()){
        switch(bytes){
        case 1:return __crc32cb(crc,static_cast<std::uint8_t>(data));
        case 2:return __crc32ch(crc,static_cast<std::uint16_t>(data));
        case 4:return __crc32cw(crc,static_cast<std::uint32_t>(data));
        default:return __crc32cd(crc,data);
        }
    }
#endif
    for(unsigned bit=0;bit<bytes*8U;++bit){
        crc^=static_cast<std::uint32_t>((data>>bit)&1U);
        crc=(crc>>1)^((crc&1U)?0x82f63b78U:0U);
    }
    return crc;
}
}
