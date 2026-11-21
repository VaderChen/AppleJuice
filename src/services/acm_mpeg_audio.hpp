#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace x64arm::acm {
inline constexpr unsigned mpeg=0x50,mp3=0x55;
inline bool is_mpeg_audio(unsigned tag){return tag==mpeg || tag==mp3;}

struct MpegFrame {unsigned bytes,frames,layer,rate,channels;};

inline std::optional<MpegFrame> mpeg_frame(std::array<std::uint8_t,4> bytes){
    const auto header=(std::uint32_t{bytes[0]}<<24)|(std::uint32_t{bytes[1]}<<16)|
        (std::uint32_t{bytes[2]}<<8)|bytes[3];
    const auto version=(header>>19)&3U,layer_bits=(header>>17)&3U,index=(header>>12)&15U,frequency=(header>>10)&3U;
    if((header&0xffe00000U)!=0xffe00000U || version==1 || !layer_bits || !index || index==15 ||
       frequency==3 || (header&3U)==2)return {};
    constexpr unsigned rates[]{44100,48000,32000};
    constexpr unsigned mpeg1[][15]{
        {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448},
        {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384},
        {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320}
    };
    constexpr unsigned low_layer1[]{0,32,48,56,64,80,96,112,128,144,160,176,192,224,256};
    constexpr unsigned low_layers23[]{0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
    const auto layer=4-layer_bits;
    const auto rate=rates[frequency]/(version==3?1U:version==2?2U:4U);
    const auto bitrate=1000*(version==3?mpeg1[layer-1][index]:layer==1?low_layer1[index]:low_layers23[index]);
    const auto padding=(header>>9)&1U;
    const auto frames=layer==1?384U:layer==3 && version!=3?576U:1152U;
    const auto size=layer==1?(12*bitrate/rate+padding)*4:frames/8*bitrate/rate+padding;
    return MpegFrame{size,frames,layer,rate,((header>>6)&3U)==3?1U:2U};
}
}
