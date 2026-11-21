#pragma once

#include "avi_frame_image.hpp"

namespace x64arm::avi {
inline bool is_run_length(const FrameLayout& layout){
    return layout.compression==1 || layout.compression==2;
}

// RLE 的位移及提早 EOL 會保留前一個影格的索引。呼叫端在完整驗證封包後才發布新影格。
inline bool decode_run_length(View packet,const FrameLayout& dib,std::span<std::uint8_t> image){
    const bool four=dib.depth==4;
    unsigned x=0,y=0;
    std::size_t at=0;
    while(packet.size()-at>=2){
        const unsigned count=packet[at++],value=packet[at++];
        if(!count && value==1)return true;
        if(y>=dib.height)return false;
        if(!count && !value){
            x=0;
            ++y;
            continue;
        }
        if(!count && value==2){
            if(packet.size()-at<2 || packet[at]>dib.width-x || packet[at+1]>=dib.height-y)return false;
            x+=packet[at++];
            y+=packet[at++];
            continue;
        }
        const auto pixels=count?count:value;
        const auto bytes=count?0U:four?(pixels+1)/2:pixels;
        const auto padded=(bytes+1)&~1U;
        if(pixels>dib.width-x || packet.size()-at<padded)return false;
        auto row=image.subspan(std::size_t(y)*dib.stride,dib.stride);
        for(unsigned i=0;i<pixels;++i,++x){
            const auto source=count?value:packet[at+(four?i/2:i)];
            if(four){
                const auto index=(source>>((i&1)?0:4))&15;
                const auto shift=(x&1)?0:4;
                row[x/2]=static_cast<std::uint8_t>((row[x/2]&~(15U<<shift))|(index<<shift));
            }else row[x]=static_cast<std::uint8_t>(source);
        }
        at+=padded;
    }
    return false;
}

} // namespace x64arm::avi
