#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace x64arm::audio {
// 使用者手動設定；尚未加入任何自動啟用條件。
inline bool environment_enabled(){const auto value=std::getenv("X64ARM_AUDIO_ENVIRONMENT");return value && std::string_view(value)=="1";}
// Windows 的 SPEAKER_* 位元順序即交錯 PCM 的聲道順序。
constexpr unsigned channel_mask(unsigned channels) {
    constexpr unsigned masks[]{0,4,3,0xb,0x33,0x607,0x60f,0x70f,0x63f};
    return channels<9?masks[channels]:0;
}
struct SpeakerPosition {float x{},y{},z{};bool lfe{};};
// 宿主使用右手座標：x 向右、y 向上、-z 向前。
inline SpeakerPosition speaker_position(unsigned speaker) {
    constexpr float d=.7071067811865475F;
    switch(speaker){
    case 1:return {-.5F,0,-.8660254F};case 2:return {.5F,0,-.8660254F};
    case 4:return {0,0,-1};case 8:return {0,0,0,true};
    case 16:return {-d,0,d};case 32:return {d,0,d};
    case 64:return {-d,0,-d};case 128:return {d,0,-d};
    case 256:return {0,0,1};case 512:return {-1,0,0};case 1024:return {1,0,0};
    case 2048:return {0,1,0};case 4096:return {-d,d,-d};case 8192:return {0,d,-d};
    case 16384:return {d,d,-d};case 32768:return {-d,d,d};case 65536:return {0,d,d};
    case 131072:return {d,d,d};default:return {};
    }
}
inline unsigned speaker_at(unsigned mask,unsigned index) {
    while(mask && index--)mask&=mask-1;
    return mask?1U<<std::countr_zero(mask):0;
}
inline bool valid_channel_layout(unsigned channels,unsigned mask) {
    mask=mask?mask:channel_mask(channels);
    return channels && channels<=18 && mask && !(mask&~0x3ffffU) && std::popcount(mask)==static_cast<int>(channels);
}
inline float pcm_sample(const std::uint8_t* p,unsigned bits,bool floating) {
    if(floating){std::uint32_t raw=0;for(unsigned i=0;i<4;++i)raw|=std::uint32_t{p[i]}<<(8*i);const auto value=std::bit_cast<float>(raw);return std::isfinite(value)?std::clamp(value,-1.F,1.F):0.F;}
    if(bits==8)return (static_cast<float>(*p)-128.F)/128.F;
    std::uint32_t raw=0;for(unsigned i=0;i<bits/8;++i)raw|=std::uint32_t{p[i]}<<(8*i);
    if(bits==16)return static_cast<float>(static_cast<std::int16_t>(raw))/32768.F;
    if(bits==24)return static_cast<float>(static_cast<std::int32_t>(raw<<8))/2147483648.F;
    return static_cast<float>(static_cast<std::int32_t>(raw))/2147483648.F;
}
}
