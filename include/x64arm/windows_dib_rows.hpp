#pragma once
#include "x64arm/windows_dib.hpp"
#include <algorithm>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
namespace x64arm {
// 以一份格式描述處理整列；既有 scalar codec 負責非標準 bitfield。
class DibRowCodec {
    DibPixelCodec scalar_;
    std::span<const std::uint32_t> palette_;
    unsigned bpp_;
    bool bgr_{},alpha_{},rgb555_{},rgb565_{};
    unsigned nearest(std::uint32_t color)const noexcept {
        unsigned best=0;std::uint32_t distance=UINT32_MAX;std::size_t p=0;
#if defined(__aarch64__)
        const auto mask=vdupq_n_u32(255);const auto red=vreinterpretq_s32_u32(vdupq_n_u32(color&255)),green=vreinterpretq_s32_u32(vdupq_n_u32((color>>8)&255)),blue=vreinterpretq_s32_u32(vdupq_n_u32((color>>16)&255));
        for(;p+4<=palette_.size();p+=4){
            const auto colors=vld1q_u32(palette_.data()+p);
            const auto r=vsubq_s32(vreinterpretq_s32_u32(vandq_u32(colors,mask)),red),g=vsubq_s32(vreinterpretq_s32_u32(vandq_u32(vshrq_n_u32(colors,8),mask)),green),b=vsubq_s32(vreinterpretq_s32_u32(vandq_u32(vshrq_n_u32(colors,16),mask)),blue);
            const auto distances=vreinterpretq_u32_s32(vaddq_s32(vaddq_s32(vmulq_s32(r,r),vmulq_s32(g,g)),vmulq_s32(b,b)));const auto minimum=vminvq_u32(distances);
            if(minimum<distance){std::array<std::uint32_t,4> lanes;vst1q_u32(lanes.data(),distances);unsigned lane=0;while(lanes[lane]!=minimum)++lane;distance=minimum;best=static_cast<unsigned>(p)+lane;}
        }
#endif
        for(;p<palette_.size();++p){std::uint32_t sum=0;for(unsigned c=0;c<3;++c){const auto delta=static_cast<int>((color>>(c*8))&255)-static_cast<int>((palette_[p]>>(c*8))&255);sum+=static_cast<std::uint32_t>(delta*delta);}if(sum<distance){distance=sum;best=static_cast<unsigned>(p);}}
        return best;
    }
    bool fits(std::size_t bytes,std::size_t colors)const noexcept {
        if(bpp_==1 || bpp_==4 || bpp_==8){const auto per_byte=8/bpp_;return colors/per_byte+(colors%per_byte!=0)<=bytes;}
        return (bpp_==16 || bpp_==24 || bpp_==32) && colors<=bytes/(bpp_/8);
    }
public:
    DibRowCodec(unsigned bpp,const std::array<std::uint32_t,4>& masks,std::span<const std::uint32_t> palette={}):scalar_(masks),palette_(palette),bpp_(bpp){
        bgr_=(bpp==24 || bpp==32) && masks[0]==0xff0000 && masks[1]==0xff00 && masks[2]==0xff && (masks[3]==0 || (bpp==32 && masks[3]==0xff000000));alpha_=masks[3]!=0;
        rgb555_=bpp==16 && masks==std::array<std::uint32_t,4>{0x7c00,0x3e0,0x1f,0};rgb565_=bpp==16 && masks==std::array<std::uint32_t,4>{0xf800,0x7e0,0x1f,0};
    }
    bool decode(std::span<const std::uint8_t> bytes,std::span<std::uint32_t> colors,bool* changed=nullptr)const noexcept {
        if(!fits(bytes.size(),colors.size()))return false;std::size_t x=0;
#if defined(__aarch64__)
        if(bgr_)for(;x+8<=colors.size();x+=8){
            uint8x8x4_t rgba;
            if(bpp_==24){const auto bgr=vld3_u8(bytes.data()+x*3);rgba={{bgr.val[2],bgr.val[1],bgr.val[0],vdup_n_u8(255)}};}
            else {const auto bgra=vld4_u8(bytes.data()+x*4);rgba={{bgra.val[2],bgra.val[1],bgra.val[0],alpha_?bgra.val[3]:vdup_n_u8(255)}};}
            auto* output=reinterpret_cast<std::uint8_t*>(colors.data()+x);
            if(changed && !*changed){const auto prior=vld4_u8(output);auto difference=veor_u8(prior.val[0],rgba.val[0]);for(unsigned n=1;n<4;++n)difference=vorr_u8(difference,veor_u8(prior.val[n],rgba.val[n]));*changed=vmaxv_u8(difference)!=0;}
            vst4_u8(output,rgba);
        }
        if(rgb555_ || rgb565_)for(;x+4<=colors.size();x+=4){
            const auto raw=vmovl_u16(vreinterpret_u16_u8(vld1_u8(bytes.data()+x*2)));const auto mask=vdupq_n_u32(31);
            const auto scale5=[](auto v){return vshrq_n_u32(vaddq_u32(vmulq_n_u32(v,527),vdupq_n_u32(23)),6);};
            const auto r=scale5(vandq_u32(rgb565_?vshrq_n_u32(raw,11):vshrq_n_u32(raw,10),mask)),b=scale5(vandq_u32(raw,mask));
            const auto g=rgb565_?vshrq_n_u32(vaddq_u32(vmulq_n_u32(vandq_u32(vshrq_n_u32(raw,5),vdupq_n_u32(63)),259),vdupq_n_u32(33)),6):scale5(vandq_u32(vshrq_n_u32(raw,5),mask));
            const auto rgba=vorrq_u32(vorrq_u32(r,vshlq_n_u32(g,8)),vorrq_u32(vshlq_n_u32(b,16),vdupq_n_u32(0xff000000)));
            if(changed && !*changed)*changed=vminvq_u32(vceqq_u32(vld1q_u32(colors.data()+x),rgba))!=UINT32_MAX;vst1q_u32(colors.data()+x,rgba);
        }
#endif
        if(bpp_<=8){for(;x<colors.size();++x){const auto index=(bytes[x*bpp_/8]>>(8-bpp_-x*bpp_%8))&((1U<<bpp_)-1);if(index>=palette_.size())return false;const auto color=palette_[index]|0xff000000U;if(changed && colors[x]!=color)*changed=true;colors[x]=color;}}
        else for(;x<colors.size();++x){std::uint32_t pixel=0;for(unsigned n=0;n<bpp_/8;++n)pixel|=std::uint32_t{bytes[x*(bpp_/8)+n]}<<(n*8);const auto color=scalar_.decode(pixel);if(changed && colors[x]!=color)*changed=true;colors[x]=color;}
        return true;
    }
    bool encode(std::span<const std::uint32_t> colors,std::span<std::uint8_t> bytes,bool preserve_unused=false)const noexcept {
        if(!fits(bytes.size(),colors.size()) || (bpp_<=8 && (palette_.empty() || palette_.size()>(1U<<bpp_))))return false;std::size_t x=0;
#if defined(__aarch64__)
        if(bgr_)for(;x+8<=colors.size();x+=8){const auto rgba=vld4_u8(reinterpret_cast<const std::uint8_t*>(colors.data()+x));
            if(bpp_==24){const uint8x8x3_t bgr{{rgba.val[2],rgba.val[1],rgba.val[0]}};vst3_u8(bytes.data()+x*3,bgr);}
            else{const auto alpha=alpha_?rgba.val[3]:preserve_unused?vld4_u8(bytes.data()+x*4).val[3]:vdup_n_u8(0);const uint8x8x4_t bgra{{rgba.val[2],rgba.val[1],rgba.val[0],alpha}};vst4_u8(bytes.data()+x*4,bgra);}
        }
        if(rgb555_ || rgb565_)for(;x+4<=colors.size();x+=4){const auto color=vld1q_u32(colors.data()+x),mask=vdupq_n_u32(255);
            const auto pack=[](auto v,unsigned range){const auto value=vaddq_u32(vmulq_n_u32(v,range),vdupq_n_u32(127));return vshrq_n_u32(vaddq_u32(vaddq_u32(value,vdupq_n_u32(1)),vshrq_n_u32(value,8)),8);};
            const auto r=pack(vandq_u32(color,mask),31),g=pack(vandq_u32(vshrq_n_u32(color,8),mask),rgb565_?63U:31U),b=pack(vandq_u32(vshrq_n_u32(color,16),mask),31);
            auto pixel=vorrq_u32(vorrq_u32(rgb565_?vshlq_n_u32(r,11):vshlq_n_u32(r,10),vshlq_n_u32(g,5)),b);
            if(rgb555_ && preserve_unused)pixel=vorrq_u32(pixel,vandq_u32(vmovl_u16(vreinterpret_u16_u8(vld1_u8(bytes.data()+x*2))),vdupq_n_u32(0x8000)));
            vst1_u8(bytes.data()+x*2,vreinterpret_u8_u16(vmovn_u32(pixel)));
        }
#endif
        if(bpp_<=8){for(;x<colors.size();++x){const auto shift=8-bpp_-x*bpp_%8;const auto at=x*bpp_/8;
            const auto previous=(bytes[at]>>shift)&((1U<<bpp_)-1);
            // 相同 RGB 的色表入口仍有不同索引；未改變的像素保留其原始索引。
            const auto index=preserve_unused && previous<palette_.size() && (palette_[previous]&0xffffffU)==(colors[x]&0xffffffU)?previous:nearest(colors[x]);
            bytes[at]=static_cast<std::uint8_t>((bytes[at]&~(((1U<<bpp_)-1)<<shift))|(index<<shift));}}
        else for(;x<colors.size();++x){std::uint32_t previous=0;if(preserve_unused)for(unsigned n=0;n<bpp_/8;++n)previous|=std::uint32_t{bytes[x*(bpp_/8)+n]}<<(n*8);const auto pixel=scalar_.encode(colors[x],previous);for(unsigned n=0;n<bpp_/8;++n)bytes[x*(bpp_/8)+n]=static_cast<std::uint8_t>(pixel>>(n*8));}
        return true;
    }
};
}
