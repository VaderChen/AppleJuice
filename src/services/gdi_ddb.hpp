#pragma once
#include "x64arm/windows_palette.hpp"

namespace x64arm {
// DDB 使用 WORD 對齊的掃描列，與 DIB 的 DWORD 對齊不同。
inline GuiBitmap gdi_ddb(std::uint32_t width,std::uint32_t height,std::uint16_t depth){
    GuiBitmap bitmap{width,height,std::vector<std::uint32_t>(static_cast<std::size_t>(width)*height)};
    bitmap.bits_per_pixel=depth;
    bitmap.stride=(width*depth+15)/16*2;
    bitmap.dib_palette=gui_dib_palette(depth);
    if(depth==16)bitmap.dib_masks={0x7c00,0x3e0,0x1f,0};
    else bitmap.dib_masks[3]=0;
    return bitmap;
}
inline bool gdi_bitmap_dimensions_fit(std::uint32_t width,std::uint32_t height){
    return width<=16384 && height<=16384 && std::uint64_t{width}*height<=64*1024*1024;
}
}
