#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// GDI+ ImageAttributes 的重新著色（純運算）。ColorAdjustType：Default 0、Bitmap 1、Brush 2、Pen 3、Text 4；
// 某類別沒有任何設定時沿用 Default。套用順序：色彩對應表 → 色彩鍵 → 色彩矩陣（含灰階矩陣）→ 臨界值 → gamma → 輸出通道。
namespace x64arm {
struct GdiplusColorAdjust {
    bool noop{};std::optional<std::array<double,25>> matrix{};std::array<double,25> gray{};unsigned matrix_flags{};
    std::optional<double> threshold{},gamma{};std::optional<std::pair<std::uint32_t,std::uint32_t>> keys{};std::optional<unsigned> channel{};
    std::optional<std::u16string> profile{};std::vector<std::pair<std::uint32_t,std::uint32_t>> remap{};
    bool configured()const{return noop || matrix || threshold || gamma || keys || channel || profile || !remap.empty();}
};
struct GdiplusImageAttributes {
    std::array<GdiplusColorAdjust,5> adjust{};
    // DrawImage 超出來源影像時的外圍模式與 Clamp 顏色；預設為 Clamp 透明。
    unsigned wrap{4};std::uint32_t wrap_color{};bool clamp{};bool cached{};
    const GdiplusColorAdjust& category(unsigned type)const{return type<5 && adjust[type].configured()?adjust[type]:adjust[0];}
};
inline std::uint32_t gdiplus_recolor(const GdiplusColorAdjust& a,std::uint32_t argb){
    if(a.noop)return argb;
    for(const auto& [from,to]:a.remap)if(from==argb){argb=to;break;}
    if(a.keys){const auto [low,high]=*a.keys;bool inside=true;for(unsigned shift=0;shift<24;shift+=8){const auto c=(argb>>shift)&255U;if(c<((low>>shift)&255U) || c>((high>>shift)&255U))inside=false;}if(inside)return 0;}
    std::array<double,4> c{((argb>>16)&255U)/255.0,((argb>>8)&255U)/255.0,(argb&255U)/255.0,(argb>>24)/255.0};
    if(a.matrix){
        // 列向量 [r g b a 1] 乘 5×5 矩陣；SkipGrays（1）不調整灰色，AltGray（2）灰色改用灰階矩陣。
        const bool gray=c[0]==c[1] && c[1]==c[2];
        if(!(gray && a.matrix_flags==1)){const auto& m=gray && a.matrix_flags==2?a.gray:*a.matrix;std::array<double,4> out{};
            for(unsigned j=0;j<4;++j){out[j]=m[20+j];for(unsigned i=0;i<4;++i)out[j]+=c[i]*m[i*5+j];}c=out;}
    }
    for(auto& v:c)v=std::clamp(v,0.0,1.0);
    if(a.threshold)for(unsigned i=0;i<3;++i)c[i]=c[i]>=*a.threshold?1:0;
    if(a.gamma && *a.gamma>0)for(unsigned i=0;i<3;++i)c[i]=std::pow(c[i],*a.gamma);
    if(a.channel){
        // 輸出 CMYK 單一通道的灰階影像：墨量越多越暗。
        const auto k=1-std::max({c[0],c[1],c[2]});double ink=k;
        if(*a.channel<3)ink=k>=1?0:(1-c[*a.channel]-k)/(1-k);
        c[0]=c[1]=c[2]=1-std::clamp(ink,0.0,1.0);
    }
    std::uint32_t out=static_cast<std::uint32_t>(std::lround(c[3]*255))<<24;
    for(unsigned i=0;i<3;++i)out|=static_cast<std::uint32_t>(std::lround(c[i]*255))<<(16-8*i);
    return out;
}
}
