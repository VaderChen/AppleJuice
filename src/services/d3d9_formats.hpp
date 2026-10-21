#pragma once
#include <cstdint>
#include <stdexcept>
#include <span>
#include <vector>
namespace x64arm::d3d9_abi {
inline std::uint32_t sampled_format(std::uint32_t format);
inline std::uint32_t native_format(std::uint32_t format){
    switch(format){case 116:return 2;case 113:return 10;case 36:return 11;case 115:return 16;case 35:return 24;case 32:return 28;case 112:return 34;case 34:return 35;case 114:return 41;case 51:return 49;case 111:return 54;case 81:return 56;case 50:return 61;case 28:return 65;case 0x31545844:return 71;case 0x33545844:return 74;case 0x35545844:return 77;case 23:return 85;case 25:return 86;case 21:return 87;case 22:return 88;case 26:return 115;default:throw std::invalid_argument("D3D9 FORMAT 無原生對應："+std::to_string(format));}
}
inline std::uint32_t sampled_format(std::uint32_t format){return format==50 || format==51?28:format==81?11:native_format(format);}
inline std::vector<std::uint8_t> sampled_pixels(std::span<const std::uint8_t> pixels,std::uint32_t format){
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
