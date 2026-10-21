#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string_view>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace x64arm::font_export_detail {
inline bool acceleration_enabled(){
    const auto* value=std::getenv("X64ARM_FONT_EXPORT_ACCELERATION");
    return !value || std::string_view(value)!="0";
}
// SFNT 使用 big-endian 32-bit word 加總；unsigned 溢位與零補齊保持原契約。
inline std::uint32_t checksum(std::span<const std::uint8_t> data,bool accelerated){
    std::size_t i=0;std::uint32_t result=0;
#if defined(__aarch64__)
    if(accelerated){
        auto sum=vdupq_n_u32(0);
        for(;data.size()-i>=16;i+=16){
            const auto words=vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data.data()+i)));
            sum=vaddq_u32(sum,words);
        }
        result=vaddvq_u32(sum);
    }
#else
    (void)accelerated;
#endif
    for(;i<data.size();i+=4){
        std::uint32_t word=0;
        for(unsigned j=0;j<4;++j)word=(word<<8)|(i+j<data.size()?data[i+j]:0);
        result+=word;
    }
    return result;
}
}
