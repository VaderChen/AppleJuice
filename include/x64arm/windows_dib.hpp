#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <span>
namespace x64arm {
inline bool valid_dib_masks(std::span<const std::uint32_t> masks,unsigned bpp) noexcept {
    const auto allowed=bpp==32?UINT32_MAX:(1U<<bpp)-1;std::uint32_t occupied=0;
    for(unsigned i=0;i<masks.size();++i){const auto mask=masks[i];if(!mask){if(i<3)return false;continue;}
        if((mask&~allowed) || (mask&occupied))return false;const auto bits=mask>>std::countr_zero(mask);if(bits&(bits+1))return false;occupied|=mask;}
    return true;
}
inline std::uint32_t dib_channel(std::uint32_t pixel,std::uint32_t mask) noexcept {
    if(!mask)return 255;const auto shift=std::countr_zero(mask);mask>>=shift;
    return static_cast<std::uint32_t>(((static_cast<std::uint64_t>(pixel>>shift)&mask)*255+mask/2)/mask);
}
inline std::uint32_t dib_pack_channel(std::uint32_t channel,std::uint32_t mask) noexcept {
    if(!mask)return 0;const auto shift=std::countr_zero(mask);const auto bits=mask>>shift;
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(channel&255)*bits+127)/255)<<shift;
}
// 每次影像轉換只解析一次格式；與 scalar helper 共用相同的整數捨入契約。
// 所有完整 byte 通道均可直接移位，不依賴特定 RGB／BGR 排列。
class DibPixelCodec {
    std::array<unsigned,4> shifts_{};
    std::array<std::uint32_t,4> ranges_{};
    std::uint32_t occupied_{};
    bool byte_channels_{true};
public:
    explicit DibPixelCodec(const std::array<std::uint32_t,4>& masks) noexcept {
        for(unsigned n=0;n<4;++n){
            occupied_|=masks[n];
            if(masks[n]){shifts_[n]=static_cast<unsigned>(std::countr_zero(masks[n]));ranges_[n]=masks[n]>>shifts_[n];}
            byte_channels_&=ranges_[n]==0 || ranges_[n]==255;
        }
    }
    std::uint32_t decode(std::uint32_t pixel) const noexcept {
        std::uint32_t color=0;
        if(byte_channels_){
            for(unsigned n=0;n<4;++n)color|=(ranges_[n]?((pixel>>shifts_[n])&255):255)<<(n*8);
        }else{
            for(unsigned n=0;n<4;++n){
                const auto range=ranges_[n];
                const auto value=range?static_cast<std::uint32_t>((static_cast<std::uint64_t>((pixel>>shifts_[n])&range)*255+range/2)/range):255;
                color|=value<<(n*8);
            }
        }
        return color;
    }
    std::uint32_t encode(std::uint32_t color,std::uint32_t previous=0) const noexcept {
        std::uint32_t pixel=previous&~occupied_;
        if(byte_channels_){
            for(unsigned n=0;n<4;++n)if(ranges_[n])pixel|=((color>>(n*8))&255)<<shifts_[n];
        }else{
            for(unsigned n=0;n<4;++n)pixel|=static_cast<std::uint32_t>((static_cast<std::uint64_t>((color>>(n*8))&255)*ranges_[n]+127)/255)<<shifts_[n];
        }
        return pixel;
    }
};
}
