#pragma once
#include "texture_image.hpp"
#include "texture_transform.hpp"
extern "C" {
#include "softfloat.h"
}

namespace x64arm::texture_image {
struct Decoded {GraphicsResourceDesc desc;std::uint32_t file_format{};std::vector<std::vector<std::uint8_t>> pixels;};
// 非浮點及壓縮格式經過共用影像轉換；所有 mip 與 array／cube element 保留。
inline void convert_format(Decoded& image,std::uint32_t format,std::uint32_t filter=UINT32_MAX){
    const auto& d=image.desc;
    for(unsigned layer=0;layer<d.layers;++layer)for(unsigned mip=0;mip<d.levels;++mip){
        const std::array size{std::max(1U,d.width>>mip),std::max(1U,d.height>>mip),std::max(1U,d.depth>>mip)};
        auto& data=image.pixels.at(layer*d.levels+mip);
        data=transform(data,d.format,size,{0,0,0,size[0],size[1],size[2]},format,size,filter);
    }
}
// 浮點紋理共用分量轉換；缺少的 RGB 分量填零、alpha 填一，保留 HDR 值。
inline void convert_float(Decoded& image,std::uint32_t format){
    const auto layout=[](std::uint32_t f)->std::pair<unsigned,unsigned>{
        switch(f){case 2:return {4,4};case 6:return {3,4};case 16:return {2,4};case 41:return {1,4};
            case 10:return {4,2};case 34:return {2,2};case 54:return {1,2};default:return {};}
    };
    const auto [sc,sb]=layout(image.desc.format);const auto [dc,db]=layout(format);
    if(!sc || !dc)throw Error(0x88982f80U,"紋理格式轉換尚未支援");
    // SoftFloat 使用執行緒區域狀態，不讓影像轉換污染客體的浮點環境。
    const auto rounding=softfloat_roundingMode,flags=softfloat_exceptionFlags;
    struct Restore {std::uint_fast8_t rounding,flags;~Restore(){softfloat_roundingMode=rounding;softfloat_exceptionFlags=flags;}} restore{rounding,flags};
    softfloat_roundingMode=softfloat_round_near_even;
    for(auto& source:image.pixels){
        if(source.size()%(sc*sb))throw Error(0x80070057U,"浮點紋理資料長度不符");
        const auto count=source.size()/(sc*sb);
        if(count>512U*1024U*1024U/(dc*db))throw Error(0x80070057U,"轉換後紋理過大");
        std::vector<std::uint8_t> destination(count*dc*db);
        for(std::size_t i=0;i<count;++i)for(unsigned c=0;c<dc;++c){
            std::uint32_t value=c==3?0x3f800000U:0;
            if(c<sc){value=0;for(unsigned b=0;b<sb;++b)value|=std::uint32_t{source[(i*sc+c)*sb+b]}<<(8*b);if(sb==2)value=f16_to_f32({static_cast<std::uint16_t>(value)}).v;}
            if(db==2)value=f32_to_f16({value}).v;
            for(unsigned b=0;b<db;++b)destination[(i*dc+c)*db+b]=static_cast<std::uint8_t>(value>>(8*b));
        }
        source=std::move(destination);
    }
}
inline std::uint32_t get32(std::span<const std::uint8_t> bytes,std::size_t at){
    if(at>bytes.size() || bytes.size()-at<4)throw Error(0x88760b59U,"影像標頭遭截斷");
    std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[at+i]}<<(8*i);return value;
}
inline Decoded decode(std::span<const std::uint8_t> bytes){
    Decoded out;auto& d=out.desc;d.dimension=3;d.bind=8;
    if(bytes.empty())throw Error(0x88760b59U,"影像資料為空");
    if(bytes.size()>=4 && get32(bytes,0)==0x20534444U){
        out.file_format=4;decode_dds(bytes,d,out.pixels);
        return out;
    }
#ifdef __APPLE__
    Cf data(CFDataCreate(nullptr,bytes.data(),static_cast<CFIndex>(bytes.size())));if(!data.value)throw std::bad_alloc{};
    Cf source(CGImageSourceCreateWithData(data.value,nullptr));if(!source.value)throw Error(0x88760b59U,"影像格式無法辨識");
    // 無法辨識的資料（含空資料）沒有 UTI；CFEqual 不接受 NULL。
    const auto type=CGImageSourceGetType(source.value);if(!type)throw Error(0x88760b59U,"影像格式無法辨識");
    out.file_format=CFEqual(type,CFSTR("public.png"))?3:CFEqual(type,CFSTR("public.jpeg"))?1:CFEqual(type,CFSTR("public.tiff"))?10:CFEqual(type,CFSTR("com.compuserve.gif"))?11:0;
    Cf image(CGImageSourceCreateImageAtIndex(source.value,0,nullptr));if(!image.value)throw Error(0x88760b59U,"影像解碼失敗");
    const auto width=CGImageGetWidth(image.value),height=CGImageGetHeight(image.value);if(!width || !height || width>16384 || height>16384 || width*height>64*1024*1024)throw Error(0x80070057U,"影像尺寸超限");
    d.width=static_cast<std::uint32_t>(width);d.height=static_cast<std::uint32_t>(height);d.format=28;
    out.pixels.emplace_back(width*height*4);auto& pixels=out.pixels[0];Cf colors(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    Cf context(CGBitmapContextCreate(pixels.data(),width,height,8,width*4,colors.value,static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast)|kCGBitmapByteOrder32Big));
    if(!context.value)throw Error(0x80004005U,"無法建立影像解碼表面");
    CGContextDrawImage(context.value,CGRectMake(0,0,static_cast<CGFloat>(width),static_cast<CGFloat>(height)),image.value);
    for(std::size_t i=0;i<pixels.size();i+=4){const auto alpha=pixels[i+3];if(alpha && alpha!=255)for(unsigned c=0;c<3;++c)pixels[i+c]=static_cast<std::uint8_t>(std::min(255U,(unsigned(pixels[i+c])*255U+alpha/2U)/alpha));}
    return out;
#else
    throw Error(0x88982f50U,"此平台未提供影像解碼器");
#endif
}
}
