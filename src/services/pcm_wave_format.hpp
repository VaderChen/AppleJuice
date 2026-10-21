#pragma once
#include "x64arm/memory.hpp"
#include <array>
#include <algorithm>
#include <optional>
#include <span>

namespace x64arm {
struct PcmWaveFormat {unsigned channels{},rate{},bits{},align{};bool floating{};};
// API 自行選擇已提供的編碼與採樣率；結構布局、平均位元組數與聲道語義共用。
inline std::optional<PcmWaveFormat> parse_pcm_wave_format(std::span<const std::uint8_t> bytes,
    bool extended=false,bool floating=false,unsigned minimum_rate=100,unsigned maximum_rate=200000){
    if(bytes.size()<16)return {};
    const auto get=[&](unsigned at,unsigned width=16){unsigned value=0;for(unsigned i=0;i<width/8;++i)value|=unsigned{bytes[at+i]}<<(i*8);return value;};
    unsigned tag=get(0);PcmWaveFormat f{get(2),get(4,32),get(14),get(12),tag==3};unsigned extra=0;
    if(tag!=1){if(bytes.size()<18)return {};extra=get(16);}
    if(tag==0xfffe){
        if(!extended || extra<22 || extra>256)return {};
        if(bytes.size()<18+extra)return {};std::array<std::uint8_t,16> subtype{};std::copy_n(bytes.data()+24,16,subtype.data());
        constexpr std::array<std::uint8_t,16> pcm{1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71},ieee{3,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
        if(subtype!=pcm && subtype!=ieee)return {};f.floating=subtype==ieee;tag=f.floating?3U:1U;
        const auto mask=get(20,32);if(get(18)!=f.bits || (mask && mask!=(f.channels==1?4U:3U)))return {};
    }else if(extra && tag!=1)return {};
    if((tag!=1 && tag!=3) || (f.floating && !floating) || (f.channels!=1 && f.channels!=2) || f.rate<minimum_rate || f.rate>maximum_rate ||
       (f.floating?f.bits!=32:f.bits!=8 && f.bits!=16) || f.align!=f.channels*f.bits/8 || get(8,32)!=f.rate*f.align)return {};
    return f;
}
inline std::optional<PcmWaveFormat> read_pcm_wave_format(const GuestMemory& memory,std::uint64_t p,
    bool extended=false,bool floating=false,unsigned minimum_rate=100,unsigned maximum_rate=200000){
    std::array<std::uint8_t,274> bytes{};memory.read(GuestAddress{p},std::span(bytes).first(16));
    const unsigned tag=unsigned{bytes[0]}|(unsigned{bytes[1]}<<8);unsigned size=16;
    if(tag!=1){memory.read(GuestAddress{p+16},std::span(bytes).subspan(16,2));size=18;
        const unsigned extra=unsigned{bytes[16]}|(unsigned{bytes[17]}<<8);
        if(tag==0xfffe && extended && extra>=22 && extra<=256){memory.read(GuestAddress{p+18},std::span(bytes).subspan(18,extra));size+=extra;}}
    return parse_pcm_wave_format(std::span(bytes).first(size),extended,floating,minimum_rate,maximum_rate);
}
}
