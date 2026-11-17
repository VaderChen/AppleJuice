#pragma once
// GDI+ 1.1 影像效果（gdipluseffects.h）：效果 GUID、參數大小與範圍檢查、各效果的像素運算。
// 像素為非預乘 ARGB（a<<24 | r<<16 | g<<8 | b）。逐通道效果（亮度對比、色階、色彩曲線、色彩平衡、色彩對照表）先組成
// B、G、R、A 各 256 項的對照表再套用，同一份表即 GdipBitmapApplyEffect 的 auxData；模糊與銳利化在預乘空間取樣。
#include "gdiplus_image_attributes.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace x64arm::gdiplus_effect {
enum class Kind {Blur,Sharpen,ColorMatrix,ColorLut,BrightnessContrast,HueSaturationLightness,Levels,Tint,ColorBalance,RedEye,ColorCurve};
using Guid=std::array<std::uint8_t,16>;
using Table=std::array<std::uint8_t,1024>;
struct Region {std::int32_t x{},y{},width{},height{};};
constexpr Guid guid(std::uint32_t a,std::uint16_t b,std::uint16_t c,std::array<std::uint8_t,8> d){
    return {static_cast<std::uint8_t>(a),static_cast<std::uint8_t>(a>>8),static_cast<std::uint8_t>(a>>16),static_cast<std::uint8_t>(a>>24),
        static_cast<std::uint8_t>(b),static_cast<std::uint8_t>(b>>8),static_cast<std::uint8_t>(c),static_cast<std::uint8_t>(c>>8),d[0],d[1],d[2],d[3],d[4],d[5],d[6],d[7]};
}
inline std::optional<Kind> kind(const Guid& value){
    static constexpr std::pair<Guid,Kind> table[]{
        {guid(0x633c80a4,0x1843,0x482b,{0x9e,0xf2,0xbe,0x28,0x34,0xc5,0xfd,0xd4}),Kind::Blur},
        {guid(0x63cbf3ee,0xc526,0x402c,{0x8f,0x71,0x62,0xc5,0x40,0xbf,0x51,0x42}),Kind::Sharpen},
        {guid(0x718f2615,0x7933,0x40e3,{0xa5,0x11,0x5f,0x68,0xfe,0x14,0xdd,0x74}),Kind::ColorMatrix},
        {guid(0xa7ce72a9,0x0f7f,0x40d7,{0xb3,0xcc,0xd0,0xc0,0x2d,0x5c,0x32,0x12}),Kind::ColorLut},
        {guid(0xd3a1dbe1,0x8ec4,0x4c17,{0x9f,0x4c,0xea,0x97,0xad,0x1c,0x34,0x3d}),Kind::BrightnessContrast},
        {guid(0x8b2dd6c3,0xeb07,0x4d87,{0xa5,0xf0,0x71,0x08,0xe2,0x6a,0x9c,0x5f}),Kind::HueSaturationLightness},
        {guid(0x99c354ec,0x2a31,0x4f3a,{0x8c,0x34,0x17,0xa8,0x03,0xb3,0x3a,0x25}),Kind::Levels},
        {guid(0x1077af00,0x2848,0x4441,{0x94,0x89,0x44,0xad,0x4c,0x2d,0x7a,0x2c}),Kind::Tint},
        {guid(0x537e597d,0x251e,0x48da,{0x96,0x64,0x29,0xca,0x49,0x6b,0x70,0xf8}),Kind::ColorBalance},
        {guid(0x74d29d05,0x69a4,0x4266,{0x95,0x49,0x3c,0xc5,0x28,0x36,0xb6,0x32}),Kind::RedEye},
        {guid(0xdd6a0022,0x58e4,0x4a67,{0x9d,0x9b,0xd4,0x8e,0xb8,0x81,0xa5,0x3d}),Kind::ColorCurve}};
    for(const auto& [id,result]:table)if(id==value)return result;
    return std::nullopt;
}
// 參數結構大小；RedEyeCorrectionParams 為 UINT＋RECT*（x64 對齊到 16）。
inline unsigned parameter_size(Kind value,bool x86){
    switch(value){
    case Kind::Blur:case Kind::Sharpen:case Kind::BrightnessContrast:case Kind::Tint:return 8;
    case Kind::ColorMatrix:return 100;
    case Kind::ColorLut:return 1024;
    case Kind::HueSaturationLightness:case Kind::Levels:case Kind::ColorBalance:case Kind::ColorCurve:return 12;
    case Kind::RedEye:return x86?8:16;
    }
    return 0;
}
inline std::int32_t integer(std::span<const std::uint8_t> p,unsigned at){std::int32_t v{};std::memcpy(&v,p.data()+at,4);return v;}
inline float real(std::span<const std::uint8_t> p,unsigned at){float v{};std::memcpy(&v,p.data()+at,4);return v;}
// 各參數的允許範圍（gdipluseffects.h 的說明）；超出範圍回 InvalidParameter。
inline bool valid(Kind value,std::span<const std::uint8_t> p){
    const auto within=[&](unsigned at,std::int32_t low,std::int32_t high){const auto v=integer(p,at);return v>=low && v<=high;};
    switch(value){
    case Kind::Blur:{const auto r=real(p,0);return std::isfinite(r) && r>=0 && r<=255;}
    case Kind::Sharpen:{const auto r=real(p,0),a=real(p,4);return std::isfinite(r) && std::isfinite(a) && r>=0 && r<=255 && a>=0 && a<=100;}
    case Kind::ColorMatrix:{for(unsigned i=0;i<25;++i)if(!std::isfinite(real(p,i*4)))return false;return true;}
    case Kind::ColorLut:return true;
    case Kind::BrightnessContrast:return within(0,-255,255) && within(4,-100,100);
    case Kind::HueSaturationLightness:return within(0,-180,180) && within(4,-100,100) && within(8,-100,100);
    case Kind::Levels:return within(0,0,100) && within(4,-100,100) && within(8,0,100);
    case Kind::Tint:return within(0,-180,180) && within(4,-100,100);
    case Kind::ColorBalance:return within(0,-100,100) && within(4,-100,100) && within(8,-100,100);
    case Kind::RedEye:return true;
    case Kind::ColorCurve:{
        const auto adjustment=integer(p,0);if(adjustment<0 || adjustment>7 || !within(4,0,3))return false;
        // 曝光與密度 −255～255；對比、亮部、暗部、中間調 −100～100；白／黑飽和 0～255。
        return adjustment<=1?within(8,-255,255):adjustment<=5?within(8,-100,100):within(8,0,255);
    }
    }
    return false;
}
inline std::uint8_t clamp8(double v){return static_cast<std::uint8_t>(std::lround(std::clamp(v,0.0,255.0)));}
// 逐通道對照表；非逐通道效果回 nullopt。
inline std::optional<Table> table(Kind value,std::span<const std::uint8_t> p){
    Table t{};for(unsigned c=0;c<4;++c)for(unsigned i=0;i<256;++i)t[c*256+i]=static_cast<std::uint8_t>(i);
    const auto each=[&](auto&& map,unsigned channels){for(unsigned c=0;c<3;++c)if(channels&(1U<<c))for(unsigned i=0;i<256;++i)t[c*256+i]=clamp8(map(static_cast<double>(i),c));};
    switch(value){
    case Kind::ColorLut:std::copy_n(p.begin(),1024,t.begin());return t;
    case Kind::BrightnessContrast:{
        const double brightness=integer(p,0),contrast=integer(p,4);
        const double factor=contrast>=0?100.0/(100.0-0.99*contrast):(100.0+contrast)/100.0;
        each([&](double v,unsigned){return (v+brightness-128.0)*factor+128.0;},7);return t;
    }
    case Kind::Levels:{
        // 高光以下、暗部以上的範圍拉伸到 0～255，中間調以 2^(−midtone/100) 為 gamma（正值變亮）。
        const double white=255.0*integer(p,0)/100.0,black=255.0*integer(p,8)/100.0,gamma=std::pow(2.0,-integer(p,4)/100.0);
        each([&](double v,unsigned){const double span=white-black;const double u=span>0?std::clamp((v-black)/span,0.0,1.0):(v>=white?1.0:0.0);return 255.0*std::pow(u,gamma);},7);return t;
    }
    case Kind::ColorBalance:{
        // 正值往紅／綠／藍推進，負值往青／洋紅／黃推進，按通道剩餘空間比例調整。
        const std::array<double,3> amount{integer(p,8)/100.0,integer(p,4)/100.0,integer(p,0)/100.0}; // B、G、R
        each([&](double v,unsigned c){const auto a=amount[c];return a>=0?v+(255.0-v)*a:v*(1.0+a);},7);return t;
    }
    case Kind::ColorCurve:{
        const auto adjustment=integer(p,0),channel=integer(p,4);const double value_level=integer(p,8);
        const unsigned channels=channel==0?7U:channel==1?4U:channel==2?2U:1U;
        each([&](double v,unsigned){
            const double u=v/255.0;
            switch(adjustment){
            case 0:return v*std::pow(2.0,value_level/100.0);
            case 1:return v*std::pow(2.0,-value_level/100.0);
            case 2:{const double f=value_level>=0?100.0/(100.0-0.99*value_level):(100.0+value_level)/100.0;return (v-128.0)*f+128.0;}
            case 3:return v+value_level*1.275*u*u;
            case 4:return v+value_level*1.275*(1-u)*(1-u);
            case 5:return 255.0*std::pow(u,std::pow(2.0,-value_level/100.0));
            case 6:return value_level>0?v*255.0/value_level:255.0;
            default:return value_level<255?(v-value_level)*255.0/(255.0-value_level):(v>=255?255.0:0.0);
            }},channels);
        return t;
    }
    default:return std::nullopt;
    }
}
inline std::array<double,3> to_hsl(double r,double g,double b){
    const double high=std::max({r,g,b}),low=std::min({r,g,b}),l=(high+low)/2;
    if(high==low)return {0,0,l};
    const double d=high-low,s=l>0.5?d/(2-high-low):d/(high+low);
    double h=high==r?(g-b)/d+(g<b?6:0):high==g?(b-r)/d+2:(r-g)/d+4;return {h*60,s,l};
}
inline std::array<double,3> from_hsl(double h,double s,double l){
    if(s<=0)return {l,l,l};
    h=std::fmod(std::fmod(h,360.0)+360.0,360.0)/360.0;const double q=l<0.5?l*(1+s):l+s-l*s,p=2*l-q;
    const auto channel=[&](double t){if(t<0)t+=1;if(t>1)t-=1;if(t<1.0/6)return p+(q-p)*6*t;if(t<0.5)return q;if(t<2.0/3)return p+(q-p)*(2.0/3-t)*6;return p;};
    return {channel(h+1.0/3),channel(h),channel(h-1.0/3)};
}
inline std::uint32_t pack(unsigned a,double r,double g,double b){return (a<<24)|(std::uint32_t{clamp8(r)}<<16)|(std::uint32_t{clamp8(g)}<<8)|clamp8(b);}
// 預乘浮點的可分離高斯模糊（σ＝半徑÷2）；區域外取樣：expand 為真時視為透明，否則延用區域邊緣。
inline void blur(std::vector<std::uint32_t>& pixels,unsigned width,Region region,double radius,bool expand){
    if(radius<=0 || region.width<=0 || region.height<=0)return;
    const auto half=static_cast<int>(std::ceil(radius));const double sigma=std::max(radius/2,0.5);
    std::vector<double> kernel(static_cast<std::size_t>(half)*2+1);double total=0;
    for(int i=-half;i<=half;++i){kernel[static_cast<std::size_t>(i+half)]=std::exp(-i*i/(2*sigma*sigma));total+=kernel[static_cast<std::size_t>(i+half)];}
    for(auto& k:kernel)k/=total;
    const auto w=static_cast<std::size_t>(region.width),h=static_cast<std::size_t>(region.height);
    std::vector<std::array<double,4>> work(w*h),next(w*h);
    for(std::size_t y=0;y<h;++y)for(std::size_t x=0;x<w;++x){
        const auto v=pixels[(static_cast<std::size_t>(region.y)+y)*width+static_cast<std::size_t>(region.x)+x];const double a=(v>>24)/255.0;
        work[y*w+x]={((v>>16)&255U)*a,((v>>8)&255U)*a,(v&255U)*a,a};
    }
    const auto pass=[&](bool horizontal){
        for(std::size_t y=0;y<h;++y)for(std::size_t x=0;x<w;++x){
            std::array<double,4> sum{};
            for(int i=-half;i<=half;++i){
                auto sx=static_cast<long long>(x)+(horizontal?i:0),sy=static_cast<long long>(y)+(horizontal?0:i);
                if(sx<0 || sy<0 || sx>=static_cast<long long>(w) || sy>=static_cast<long long>(h)){if(expand)continue;sx=std::clamp<long long>(sx,0,static_cast<long long>(w)-1);sy=std::clamp<long long>(sy,0,static_cast<long long>(h)-1);}
                const auto& s=work[static_cast<std::size_t>(sy)*w+static_cast<std::size_t>(sx)];const auto k=kernel[static_cast<std::size_t>(i+half)];for(unsigned c=0;c<4;++c)sum[c]+=s[c]*k;
            }
            next[y*w+x]=sum;
        }
        work.swap(next);
    };
    pass(true);pass(false);
    for(std::size_t y=0;y<h;++y)for(std::size_t x=0;x<w;++x){
        const auto& s=work[y*w+x];const auto a=std::clamp(s[3],0.0,1.0);
        pixels[(static_cast<std::size_t>(region.y)+y)*width+static_cast<std::size_t>(region.x)+x]=a>0?pack(clamp8(a*255),s[0]/a,s[1]/a,s[2]/a):0;
    }
}
// 套用效果到區域（已裁切在影像內）；回傳逐通道效果的對照表。areas 為紅眼修正的 RECT（left、top、right、bottom）。
inline std::optional<Table> apply(Kind value,std::span<const std::uint8_t> p,std::span<const std::array<std::int32_t,4>> areas,std::vector<std::uint32_t>& pixels,unsigned width,Region region){
    const auto each=[&](auto&& map){for(auto y=region.y;y<region.y+region.height;++y)for(auto x=region.x;x<region.x+region.width;++x){auto& v=pixels[static_cast<std::size_t>(y)*width+static_cast<std::size_t>(x)];v=map(v,x,y);}};
    if(const auto lut=table(value,p)){
        each([&](std::uint32_t v,auto,auto){return (std::uint32_t{(*lut)[768+(v>>24)]}<<24)|(std::uint32_t{(*lut)[512+((v>>16)&255U)]}<<16)|(std::uint32_t{(*lut)[256+((v>>8)&255U)]}<<8)|(*lut)[v&255U];});
        return lut;
    }
    switch(value){
    case Kind::Blur:blur(pixels,width,region,real(p,0),integer(p,4)!=0);break;
    case Kind::Sharpen:{
        // 非銳利遮罩：原值＋amount％×（原值−模糊值），alpha 不變。
        auto blurred=pixels;blur(blurred,width,region,real(p,0),false);const double amount=real(p,4)/100.0;
        each([&](std::uint32_t v,auto x,auto y){const auto s=blurred[static_cast<std::size_t>(y)*width+static_cast<std::size_t>(x)];
            const auto channel=[&](unsigned shift){const double a=(v>>shift)&255U,b=(s>>shift)&255U;return a+amount*(a-b);};
            return pack(v>>24,channel(16),channel(8),channel(0));});
        break;
    }
    case Kind::ColorMatrix:{
        GdiplusColorAdjust adjust;std::array<double,25> matrix{};for(unsigned i=0;i<25;++i)matrix[i]=real(p,i*4);adjust.matrix=matrix;
        each([&](std::uint32_t v,auto,auto){return gdiplus_recolor(adjust,v);});break;
    }
    case Kind::HueSaturationLightness:{
        const double hue=integer(p,0),saturation=integer(p,4)/100.0,lightness=integer(p,8)/100.0;
        each([&](std::uint32_t v,auto,auto){auto [h,s,l]=to_hsl(((v>>16)&255U)/255.0,((v>>8)&255U)/255.0,(v&255U)/255.0);
            h+=hue;s=std::clamp(s*(1+saturation),0.0,1.0);l=lightness>=0?l+(1-l)*lightness:l*(1+lightness);
            const auto [r,g,b]=from_hsl(h,s,l);return pack(v>>24,r*255,g*255,b*255);});
        break;
    }
    case Kind::Tint:{
        // 色相決定的純色以像素亮度著色；amount 為負時改用互補色。
        double hue=integer(p,0),amount=integer(p,4)/100.0;if(amount<0){hue+=180;amount=-amount;}
        const auto [tr,tg,tb]=from_hsl(hue,1,0.5);const double tint_luma=0.299*tr+0.587*tg+0.114*tb;
        each([&](std::uint32_t v,auto,auto){const double r=((v>>16)&255U),g=((v>>8)&255U),b=(v&255U),luma=0.299*r+0.587*g+0.114*b;const double scale=tint_luma>0?luma/tint_luma:0;
            return pack(v>>24,r+(tr*scale-r)*amount,g+(tg*scale-g)*amount,b+(tb*scale-b)*amount);});
        break;
    }
    case Kind::RedEye:
        // 區域內紅色明顯高於綠、藍的像素，紅色改為綠藍平均。
        each([&](std::uint32_t v,auto x,auto y){
            const bool inside=std::any_of(areas.begin(),areas.end(),[&](const auto& a){return x>=a[0] && x<a[2] && y>=a[1] && y<a[3];});if(!inside)return v;
            const unsigned r=(v>>16)&255U,g=(v>>8)&255U,b=v&255U;if(r<64 || r*2<std::max(g,b)*3)return v;
            return (v&0xff00ffffU)|(((g+b)/2)<<16);});
        break;
    default:break;
    }
    return std::nullopt;
}
}
