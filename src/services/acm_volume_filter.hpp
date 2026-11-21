#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace x64arm::acm {
inline constexpr unsigned volume_tag=1,volume_size=36,unity_gain=65536;
inline constexpr unsigned filter_support=4;
inline constexpr unsigned standard_gains[]{unity_gain,unity_gain/2,unity_gain/4,0};

inline std::vector<std::uint8_t> volume_format(std::uint32_t gain) {
    std::vector<std::uint8_t> bytes(volume_size);
    bytes[0]=volume_size;
    bytes[4]=volume_tag;
    for(unsigned i=0;i<4;++i)bytes[32+i]=static_cast<std::uint8_t>(gain>>(i*8));
    return bytes;
}

inline void apply_volume(std::span<const std::uint8_t> input,std::span<std::uint8_t> output,
                         unsigned bits,bool floating,std::uint32_t gain){
    if(gain==unity_gain){
        std::copy(input.begin(),input.end(),output.begin());
        return;
    }
    const auto sample_size=bits/8;
    const auto sign=std::uint32_t{1}<<(bits-1);
    for(std::size_t at=0;at<input.size();at+=sample_size){
        std::uint32_t packed=0;
        for(unsigned byte=0;byte<sample_size;++byte)packed|=std::uint32_t{input[at+byte]}<<(byte*8);
        if(floating){
            const auto sample=std::bit_cast<float>(packed);
            const auto volume=std::isfinite(sample)?sample*(static_cast<float>(gain)/unity_gain):0.F;
            packed=std::bit_cast<std::uint32_t>(std::clamp(volume,-1.F,1.F));
        }else{
            const auto sample=bits==8?static_cast<std::int32_t>(packed)-128:
                std::bit_cast<std::int32_t>((packed^sign)-sign);
            const auto scaled=std::int64_t{sample}*gain/unity_gain;
            const auto limited=std::clamp(scaled,-static_cast<std::int64_t>(sign),std::int64_t{sign}-1);
            packed=static_cast<std::uint32_t>(bits==8?limited+128:limited);
        }
        for(unsigned byte=0;byte<sample_size;++byte)output[at+byte]=static_cast<std::uint8_t>(packed>>(byte*8));
    }
}
}
