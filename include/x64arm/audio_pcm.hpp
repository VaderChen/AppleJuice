#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
namespace x64arm::audio::detail {
template<unsigned Bits> inline float pcm_sample(const std::uint8_t* p){
    if constexpr(Bits==8)return (static_cast<float>(*p)-128.F)/128.F;
    if constexpr(Bits==16){const auto raw=static_cast<std::uint16_t>(p[0]|(static_cast<unsigned>(p[1])<<8));return static_cast<float>(static_cast<std::int16_t>(raw))/32768.F;}
    if constexpr(Bits==32){std::uint32_t raw=0;for(unsigned n=0;n<4;++n)raw|=std::uint32_t{p[n]}<<(n*8);const auto value=std::bit_cast<float>(raw);return std::isfinite(value)?std::clamp(value,-1.F,1.F):0.F;}
}
#if defined(__aarch64__)
template<unsigned Bits> inline float32x4_t pcm_four(const std::uint8_t* p){
    if constexpr(Bits==8){std::uint32_t raw;std::memcpy(&raw,p,4);const auto bytes=vcreate_u8(raw);const auto values=vmovl_u16(vget_low_u16(vmovl_u8(bytes)));return vmulq_n_f32(vsubq_f32(vcvtq_f32_u32(values),vdupq_n_f32(128)),1.F/128);}
    if constexpr(Bits==16){const auto words=vreinterpret_s16_u8(vld1_u8(p));return vmulq_n_f32(vcvtq_f32_s32(vmovl_s16(words)),1.F/32768);}
    if constexpr(Bits==32){
        const auto raw=vreinterpretq_u32_u8(vld1q_u8(p));const auto finite=vmvnq_u32(vceqq_u32(vandq_u32(raw,vdupq_n_u32(0x7f800000)),vdupq_n_u32(0x7f800000)));
        auto value=vreinterpretq_f32_u32(vandq_u32(raw,finite));
        // 比較＋選取保留 -0，並在任何 FP 比較前移除 NaN／Inf。
        value=vbslq_f32(vcgtq_f32(value,vdupq_n_f32(1)),vdupq_n_f32(1),value);
        return vbslq_f32(vcltq_f32(value,vdupq_n_f32(-1)),vdupq_n_f32(-1),value);
    }
}
#endif
// 已確認連續 PCM 區間，格式每區塊只分派一次；不重排乘法或聲道。
template<unsigned Bits,unsigned Channels> inline std::array<float,2> pcm_block(const std::uint8_t* input,float* out,std::size_t frames,float left,float right){
    std::array<float,2> peak{};std::size_t frame=0;
#if defined(__aarch64__)
    auto lpeak=vdupq_n_f32(0),rpeak=lpeak;
    for(;frame+4<=frames;frame+=4){
        const auto* source=input+frame*Channels*(Bits/8);auto a=pcm_four<Bits>(source),b=a;
        if constexpr(Channels==2){const auto samples=vuzpq_f32(a,pcm_four<Bits>(source+4*(Bits/8)));a=samples.val[0];b=samples.val[1];}
        a=vmulq_n_f32(a,left);b=vmulq_n_f32(b,right);const float32x4x2_t stereo{{a,b}};vst2q_f32(out+frame*2,stereo);
        lpeak=vmaxq_f32(lpeak,vabsq_f32(a));rpeak=vmaxq_f32(rpeak,vabsq_f32(b));
    }
    peak={vmaxvq_f32(lpeak),vmaxvq_f32(rpeak)};
#endif
    for(;frame<frames;++frame){const auto* source=input+frame*Channels*(Bits/8);const auto a=pcm_sample<Bits>(source)*left,b=pcm_sample<Bits>(source+(Channels==1?0:Bits/8))*right;out[frame*2]=a;out[frame*2+1]=b;peak[0]=std::max(peak[0],std::abs(a));peak[1]=std::max(peak[1],std::abs(b));}
    return peak;
}
inline std::array<float,2> pcm_block(const std::uint8_t* input,float* out,std::size_t frames,unsigned bits,unsigned channels,float left,float right){
    if(bits==8)return channels==1?pcm_block<8,1>(input,out,frames,left,right):pcm_block<8,2>(input,out,frames,left,right);
    if(bits==16)return channels==1?pcm_block<16,1>(input,out,frames,left,right):pcm_block<16,2>(input,out,frames,left,right);
    return channels==1?pcm_block<32,1>(input,out,frames,left,right):pcm_block<32,2>(input,out,frames,left,right);
}
}
