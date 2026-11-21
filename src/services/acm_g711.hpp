#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>

namespace x64arm::acm {
inline constexpr unsigned a_law=6,mu_law=7;
inline bool is_g711(unsigned tag){return tag==a_law || tag==mu_law;}

inline int decode_g711(std::uint8_t value,unsigned tag){
    if(tag==a_law){
        const auto code=value^0x55;
        const auto segment=(code>>4)&7;
        const auto fraction=(code&15)<<4;
        const auto sample=segment?(fraction+264)<<(segment-1):fraction+8;
        return (code&128)?sample:-sample;
    }
    const auto code=value^0xff;
    const auto sample=(((code&15)<<3)+132)<<((code>>4)&7);
    return (code&128)?132-sample:sample-132;
}

inline std::uint8_t encode_g711(int sample,unsigned tag){
    if(tag==a_law){
        const auto magnitude=static_cast<unsigned>(sample<0?-sample-1:sample);
        const auto segment=static_cast<unsigned>(std::bit_width(magnitude|255U)-8);
        const auto code=(segment<<4)|((magnitude>>std::max(4U,segment+3U))&15U);
        return static_cast<std::uint8_t>(code^(sample<0?0x55U:0xd5U));
    }
    const auto magnitude=static_cast<unsigned>(std::min(sample<0?-sample:sample,32635)+132);
    const auto segment=static_cast<unsigned>(std::bit_width(magnitude|255U)-8);
    const auto code=(segment<<4)|((magnitude>>(segment+3))&15U);
    return static_cast<std::uint8_t>(code^(sample<0?0x7fU:0xffU));
}

inline void convert_g711(std::span<const std::uint8_t> input,std::span<std::uint8_t> output,
                         unsigned from,unsigned to){
    const auto samples=from==1?input.size()/2:input.size();
    for(std::size_t i=0;i<samples;++i){
        const int sample=from==1?std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(
            unsigned{input[i*2]}|(unsigned{input[i*2+1]}<<8))):decode_g711(input[i],from);
        if(to==1){
            const auto value=static_cast<std::uint16_t>(sample);
            output[i*2]=static_cast<std::uint8_t>(value);
            output[i*2+1]=static_cast<std::uint8_t>(value>>8);
        }else output[i]=encode_g711(sample,to);
    }
}
}
