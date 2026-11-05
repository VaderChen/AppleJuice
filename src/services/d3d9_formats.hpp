#pragma once
#include <cstdint>
#include <stdexcept>
#include <span>
#include <vector>
#include "d3d9_format_contract.h"
namespace x64arm::d3d9_abi {
inline std::uint32_t sampled_format(std::uint32_t format);
inline std::uint32_t native_format(std::uint32_t format){
    if(const auto mapped=d9_native_format(format))return mapped;
    throw std::invalid_argument("D3D9 FORMAT 無原生對應："+std::to_string(format));
}
inline std::uint32_t sampled_format(std::uint32_t format){return format==23 || format==25 || format==26 || format==50 || format==51?28:format==22?87:format==81?11:native_format(format);}
inline std::vector<std::uint8_t> sampled_pixels(std::span<const std::uint8_t> pixels,std::uint32_t format){
    // BGRX 的 X 位元不屬於 alpha；保留 BGR 位元，只將取樣 alpha 設為 1。
    if(format==22){
        if(pixels.size()%4)throw std::invalid_argument("D3D9 BGRX 紋理資料長度不符");
        std::vector<std::uint8_t> out(pixels.begin(),pixels.end());
        for(std::size_t i=3;i<out.size();i+=4)out[i]=255;
        return out;
    }
    if(format==23 || format==25 || format==26){
        if(pixels.size()%2)throw std::invalid_argument("D3D9 16-bit 色彩紋理資料長度不符");
        std::vector<std::uint8_t> out(pixels.size()*2);
        for(std::size_t i=0;i<pixels.size()/2;++i){
            const unsigned value=pixels[i*2]|(unsigned{pixels[i*2+1]}<<8);
            const unsigned bits=format==26?4:5,green=format==23?6:bits,mask=(1U<<bits)-1,green_mask=(1U<<green)-1;
            out[i*4]=static_cast<std::uint8_t>((((value>>(bits+green))&mask)*255+mask/2)/mask);
            out[i*4+1]=static_cast<std::uint8_t>((((value>>bits)&green_mask)*255+green_mask/2)/green_mask);
            out[i*4+2]=static_cast<std::uint8_t>(((value&mask)*255+mask/2)/mask);
            out[i*4+3]=static_cast<std::uint8_t>(format==23?255:format==25?(value&0x8000?255:0):(value>>12)*17);
        }
        return out;
    }
    if(format!=50 && format!=51 && format!=81)return {pixels.begin(),pixels.end()};
    const unsigned step=format==50?1:2,size=format==81?8:4;if(pixels.size()%step)throw std::invalid_argument("D3D9 亮度紋理資料長度不符");std::vector<std::uint8_t> out(pixels.size()/step*size);
    for(std::size_t i=0;i<pixels.size()/step;++i){if(format==81){for(unsigned c=0;c<3;++c){out[i*8+c*2]=pixels[i*2];out[i*8+c*2+1]=pixels[i*2+1];}out[i*8+6]=out[i*8+7]=255;}else{out[i*4]=out[i*4+1]=out[i*4+2]=pixels[i*step];out[i*4+3]=format==51?pixels[i*2+1]:255;}}
    return out;
}
inline std::uint32_t legacy_format(std::uint32_t format){
    switch(format){
    case 2:return 116;case 10:return 113;case 11:return 36;
    case 16:return 115;case 24:return 35;case 28:return 32;
    case 34:return 112;case 35:return 34;case 41:return 114;
    case 49:return 51;case 54:return 111;case 56:return 81;
    case 61:return 50;case 65:return 28;
    case 71:return 0x31545844;case 74:return 0x33545844;case 77:return 0x35545844;
    case 85:return 23;case 86:return 25;case 87:return 21;case 88:return 22;
    case 115:return 26;
    default:throw std::invalid_argument("影像格式無對應 D3D9 FORMAT："+std::to_string(format));
    }
}
}
