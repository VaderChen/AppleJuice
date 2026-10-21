#pragma once
#include "x64arm/windows_graphics.hpp"
#include <algorithm>
#include <functional>
#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

namespace x64arm::texture_image {
struct Error:std::runtime_error {
    std::uint32_t hresult;
    Error(std::uint32_t code,const char* message):std::runtime_error(message),hresult(code){}
};
inline void put(std::vector<std::uint8_t>& out,std::size_t at,std::uint32_t value){
    for(unsigned i=0;i<4;++i)out.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));
}
// DDS 保留原格式、mip、array、cube face 及 volume slice，不轉成螢幕截圖。
inline std::vector<std::uint8_t> dds(const GraphicsResourceDesc& d,const std::function<std::vector<std::uint8_t>(unsigned)>& read){
    if(d.dimension<2 || d.dimension>4 || !d.width || !d.height || !d.depth || !d.levels || !d.layers || d.samples!=1)
        throw Error(0x80070057U,"DDS 紋理描述不合法或尚未 resolve MSAA");
    const bool cube=(d.misc&4)!=0,bc=graphics_format_bc_family(d.format)!=0;
    if(cube && (d.dimension!=3 || d.layers%6))throw Error(0x80070057U,"DDS cube 面數不合法");
    std::uint32_t fourcc=0;
    if(d.format==71)fourcc=0x31545844U;
    if(d.format==74)fourcc=0x33545844U;
    if(d.format==77)fourcc=0x35545844U;
    if(d.format==80)fourcc=0x31495441U;
    if(d.format==83)fourcc=0x32495441U;
    const bool rgb=d.format==28 || d.format==87 || d.format==88;
    const bool extended=d.dimension==2 || d.layers>(cube?6U:1U) || (!rgb && !fourcc);
    const auto pitch=graphics_format_pitch(d.format,d.width,d.height);
    std::vector<std::uint8_t> out(extended?148:128);
    put(out,0,0x20534444U);put(out,4,124);
    put(out,8,0x1007U|(bc?0x80000U:8U)|(d.levels>1?0x20000U:0U)|(d.dimension==4?0x800000U:0U));
    put(out,12,d.height);put(out,16,d.width);put(out,20,bc?pitch[0]*pitch[1]:pitch[0]);
    put(out,24,d.dimension==4?d.depth:0);put(out,28,d.levels);put(out,76,32);
    if(extended || fourcc){put(out,80,4);put(out,84,extended?0x30315844U:fourcc);}
    else{put(out,80,d.format==88?0x40:0x41);put(out,88,32);put(out,92,d.format==28?0xff:0xff0000);put(out,96,0xff00);put(out,100,d.format==28?0xff0000:0xff);put(out,104,d.format==88?0:0xff000000);}
    put(out,108,0x1000U|(d.levels>1?0x400008U:0U)|(cube || d.dimension==4 || d.layers>1?8U:0U));
    put(out,112,cube?0xfe00U:d.dimension==4?0x200000U:0U);
    if(extended){put(out,128,d.format);put(out,132,d.dimension);put(out,136,cube?4:0);put(out,140,cube?d.layers/6:d.layers);}
    for(unsigned layer=0;layer<d.layers;++layer)for(unsigned level=0;level<d.levels;++level){
        auto data=read(layer*d.levels+level);
        const auto row=graphics_format_pitch(d.format,std::max(1U,d.width>>level),std::max(1U,d.height>>level));
        const auto expected=std::uint64_t{row[0]}*row[1]*std::max(1U,d.depth>>level);
        if(data.size()!=expected || data.size()>512U*1024U*1024U-out.size())throw Error(0x80070057U,"DDS 子資源資料長度不符或過大");
        out.insert(out.end(),data.begin(),data.end());
    }
    return out;
}
// TIFF 6.0：以未壓縮 RGBA 與 ExtraSamples=2 保存未預乘 alpha，
// 避免 ImageIO 的 8-bit 預乘／反預乘造成不可逆的色彩精度損失。
inline std::vector<std::uint8_t> tiff_rgba(const GraphicsResourceDesc& d,const std::vector<std::uint8_t>& bytes){
    constexpr std::uint32_t entries=11,bits=8+2+entries*12+4,pixels=bits+8;
    std::vector<std::uint8_t> out(pixels);
    auto short_at=[&](std::size_t at,std::uint16_t value){out.at(at)=value&255;out.at(at+1)=value>>8;};
    short_at(0,0x4949);short_at(2,42);put(out,4,8);short_at(8,entries);
    std::size_t at=10;
    auto tag=[&](std::uint16_t id,std::uint16_t type,std::uint32_t count,std::uint32_t value){
        short_at(at,id);short_at(at+2,type);put(out,at+4,count);put(out,at+8,value);at+=12;
    };
    tag(256,4,1,d.width);tag(257,4,1,d.height);tag(258,3,4,bits);
    tag(259,3,1,1);tag(262,3,1,2);tag(273,4,1,pixels);
    tag(277,3,1,4);tag(278,4,1,d.height);tag(279,4,1,static_cast<std::uint32_t>(bytes.size()));
    tag(284,3,1,1);tag(338,3,1,2);
    for(unsigned i=0;i<4;++i)short_at(bits+i*2,8);
    out.insert(out.end(),bytes.begin(),bytes.end());
    return out;
}
#ifdef __APPLE__
template<class T> struct Cf {
    T value;
    explicit Cf(T v):value(v){}
    ~Cf(){if(value)CFRelease(value);}
    Cf(const Cf&)=delete;
};
#endif
inline std::vector<std::uint8_t> encode(const GraphicsResourceDesc& d,std::uint32_t format,const std::function<std::vector<std::uint8_t>(unsigned)>& read){
    if(format==4)return dds(d,read);
    // 一般影像格式保存第一個 2D 子資源；DDS 才能完整保存 array／volume。
    if(d.dimension!=3 || d.samples!=1 || !d.width || !d.height || std::uint64_t{d.width}*d.height>64U*1024U*1024U)
        throw Error(0x80070057U,"影像輸出需要可讀回的 2D 紋理");
    auto bytes=read(0);
    if(d.format!=28 && d.format!=29 && d.format!=87 && d.format!=88 && d.format!=91 && d.format!=93)
        throw Error(0x88982f80U,"影像編碼器不支援此紋理像素格式；可改存 DDS");
    if(bytes.size()!=std::uint64_t{d.width}*d.height*4)throw Error(0x80070057U,"影像像素長度不符");
    for(std::size_t i=0;i<bytes.size();i+=4){if(d.format>=87)std::swap(bytes[i],bytes[i+2]);if(d.format==88 || d.format==93)bytes[i+3]=255;}
    if(format==10)return tiff_rgba(d,bytes);
#ifdef __APPLE__
    CFStringRef type=nullptr;
    switch(format){case 0:type=CFSTR("com.microsoft.bmp");break;case 1:type=CFSTR("public.jpeg");break;case 3:type=CFSTR("public.png");break;case 10:type=CFSTR("public.tiff");break;case 11:type=CFSTR("com.compuserve.gif");break;default:throw Error(0x88982f50U,"沒有對應的原生影像編碼器");}
    Cf colors(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    Cf provider(CGDataProviderCreateWithData(nullptr,bytes.data(),bytes.size(),nullptr));
    if(!colors.value || !provider.value)throw std::bad_alloc{};
    Cf image(CGImageCreate(d.width,d.height,8,32,static_cast<std::size_t>(d.width)*4,colors.value,
        static_cast<CGBitmapInfo>(kCGImageAlphaLast)|kCGBitmapByteOrder32Big,provider.value,nullptr,false,kCGRenderingIntentDefault));
    Cf data(CFDataCreateMutable(nullptr,0));
    if(!image.value || !data.value)throw std::bad_alloc{};
    Cf destination(CGImageDestinationCreateWithData(data.value,type,1,nullptr));
    if(!destination.value)throw Error(0x88982f50U,"原生影像編碼器不可用");
    CGImageDestinationAddImage(destination.value,image.value,nullptr);
    if(!CGImageDestinationFinalize(destination.value))throw Error(0x80004005U,"原生影像編碼失敗");
    const auto* begin=CFDataGetBytePtr(data.value);
    return {begin,begin+CFDataGetLength(data.value)};
#else
    throw Error(0x88982f50U,"此平台未提供影像編碼器");
#endif
}
}
