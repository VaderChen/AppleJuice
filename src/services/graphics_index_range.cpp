#include "x64arm/graphics_index_range.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string_view>
#if defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>
#endif

namespace x64arm {

// 同時求最小／最大索引，供 base vertex 與頂點資料範圍驗證；不更動索引順序。
GraphicsIndexRange graphics_index_range(std::span<const std::uint8_t> data,unsigned width){
    if((width!=2 && width!=4) || data.size()%width)throw std::invalid_argument("索引資料寬度不合法");
    if(data.empty())return {};
    GraphicsIndexRange result{UINT32_MAX,0};std::size_t offset=0;
#if defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    static const bool vectorized=[] {const auto* value=std::getenv("X64ARM_GRAPHICS_INDEX_NEON");return !value || std::string_view(value)!="0";}();
    if(vectorized && data.size()>=16){
        if(width==2){
            auto lo=vdupq_n_u16(UINT16_MAX),hi=vdupq_n_u16(0);
            for(;data.size()-offset>=16;offset+=16){const auto values=vreinterpretq_u16_u8(vld1q_u8(data.data()+offset));lo=vminq_u16(lo,values);hi=vmaxq_u16(hi,values);}
            result={vminvq_u16(lo),vmaxvq_u16(hi)};
        }else{
            auto lo=vdupq_n_u32(UINT32_MAX),hi=vdupq_n_u32(0);
            for(;data.size()-offset>=16;offset+=16){const auto values=vreinterpretq_u32_u8(vld1q_u8(data.data()+offset));lo=vminq_u32(lo,values);hi=vmaxq_u32(hi,values);}
            result={vminvq_u32(lo),vmaxvq_u32(hi)};
        }
    }
#endif
    for(;offset<data.size();offset+=width){std::uint32_t index=0;std::memcpy(&index,data.data()+offset,width);result.minimum=std::min(result.minimum,index);result.maximum=std::max(result.maximum,index);}
    return result;
}
} // namespace x64arm
