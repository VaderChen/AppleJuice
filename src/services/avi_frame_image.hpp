#pragma once
#include "avi_container.hpp"
#include <optional>
#include "x64arm/media_decode.hpp"
#include "x64arm/windows_dib_rows.hpp"

namespace x64arm::avi {
bool decode_frame_image(View data,unsigned width,unsigned height,Bytes& rgba);
struct FrameLayout {
    unsigned width{},height{},stride{},depth{},compression{};
    bool top_down{};
    std::array<unsigned,4> masks{0xff0000,0xff00,0xff,0};
    std::vector<unsigned> palette;
    unsigned image_size()const{return stride*height;}
    bool raw()const{return compression==0 || compression==3 || compression==6;}
};
inline std::size_t frame_header_size(View header){
    if(header.size()<40)return 0;
    const auto size=word(header,0),compression=word(header,16),depth=word(header,12)>>16;
    const auto used=word(header,32);
    const auto colors=used?used:depth && depth<=8?1U<<depth:0U;
    if(size<40 || size>4096 || colors>256)return 0;
    return size+(size==40?(compression==3?12U:compression==6?16U:0U):0U)+colors*4U;
}
inline std::optional<FrameLayout> frame_layout(View header){
    const auto size=frame_header_size(header);
    if(!size || size>header.size() || (word(header,12)&65535)!=1)return {};
    FrameLayout f;
    f.width=word(header,4);const auto signed_height=word(header,8);
    f.top_down=signed_height>INT32_MAX;f.height=f.top_down?0U-signed_height:signed_height;
    f.depth=word(header,12)>>16;f.compression=word(header,16);
    if(!f.width || !f.height || f.width>16384 || f.height>16384 || std::uint64_t(f.width)*f.height>32U*1024*1024)return {};
    if(f.raw()){
        if(f.depth!=1 && f.depth!=4 && f.depth!=8 && f.depth!=16 && f.depth!=24 && f.depth!=32)return {};
        f.stride=((f.width*f.depth+31)/32)*4;
        if(f.compression){
            if((f.depth!=16 && f.depth!=32) || size<(f.compression==6?56U:52U))return {};
            for(unsigned i=0;i<3;++i)f.masks[i]=word(header,40+i*4);
            if(f.compression==6 || word(header,0)>=56)f.masks[3]=word(header,52);
            if(!valid_dib_masks(f.masks,f.depth))return {};
        }else if(f.depth==16)f.masks={0x7c00,0x03e0,0x001f,0};
        if(f.depth<=8){
            if((size-word(header,0))/4>(1U<<f.depth))return {};
            for(auto at=std::size_t(word(header,0));at<size;at+=4){
                const auto bgr=word(header,at);f.palette.push_back(((bgr&255)<<16)|(bgr&0xff00)|((bgr>>16)&255));
            }
        }
    }else if(f.compression!=1 && f.compression!=2 && f.compression!=4 && f.compression!=5 &&
             f.compression!=fourcc('M','J','P','G') && f.compression!=fourcc('J','P','E','G') &&
             f.compression!=fourcc('p','n','g',' ') && !media::video_decoder_available(f.compression))return {};
    return f;
}
inline Bytes frame_output(View input,const FrameLayout& source,const Bytes& requested){
    Bytes out=requested;
    if(out.empty()){
        if(source.raw())out.assign(input.begin(),input.begin()+static_cast<std::ptrdiff_t>(frame_header_size(input)));
        else{out.resize(40);put(out,0,40);put(out,12,(24U<<16)|1);}
    }
    if(!word(out,4))put(out,4,source.width);
    if(!word(out,8))put(out,8,word(input,8));
    const auto layout=frame_layout(out);
    if(!layout || !layout->raw())return {};
    put(out,20,layout->image_size());
    if(layout->depth<=8)put(out,32,static_cast<unsigned>(layout->palette.size()));
    return out;
}
// 軟體視訊解碼器輸出 NV12；AVI 的傳統 SD 視訊以 BT.601 轉為 DIB。
inline Bytes frame_rgba(const video::DecodedFrame& decoded){
    const unsigned w=decoded.width,h=decoded.height;
    const auto luma_size=std::size_t(w)*h;
    if(!w || !h || (w&1) || decoded.nv12.size()<luma_size+std::size_t(w)*((h+1)/2))return {};
    Bytes rgba(luma_size*4);
    auto channel=[](int value){return static_cast<std::uint8_t>(std::clamp((value+128)>>8,0,255));};
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        const auto at=std::size_t(y)*w+x,uv=luma_size+(y/2)*w+(x&~1U);
        const int lum=decoded.nv12[at],u=decoded.nv12[uv]-128,v=decoded.nv12[uv+1]-128;
        const auto base=decoded.full_range?256*lum:298*(lum-16);
        rgba[at*4]=channel(base+(decoded.full_range?359:409)*v);
        rgba[at*4+1]=channel(base-(decoded.full_range?88:100)*u-(decoded.full_range?183:208)*v);
        rgba[at*4+2]=channel(base+(decoded.full_range?454:516)*u);rgba[at*4+3]=255;
    }
    return rgba;
}
// 原始 DIB 可保留列填充和色表索引。轉換時與 GDI 共用列編解碼，獨立壓縮影格沿用 WIC 的影像解碼器。
inline bool frame_pixels(View format,const FrameLayout& src,View packet,const FrameLayout& dst,
                         std::span<std::uint8_t> pixels,unsigned x,unsigned y,unsigned width,unsigned height,Bytes rgba={}){
    if(src.raw() && packet.size()<src.image_size())return false;
    if(x>dst.width || y>dst.height || !width || !height || width>dst.width-x || height>dst.height-y)return false;
    if(src.raw() && !x && !y && width==src.width && width==dst.width && height==src.height && height==dst.height &&
       src.depth==dst.depth && src.masks==dst.masks && src.palette==dst.palette && src.top_down==dst.top_down){
        std::copy_n(packet.begin(),pixels.size(),pixels.begin());return true;
    }
    if(!src.raw() && rgba.empty()){
        Bytes bitmap;
        if(src.compression==1 || src.compression==2){
            bitmap.assign(format.begin(),format.end());bitmap.insert(bitmap.end(),packet.begin(),packet.end());packet=bitmap;
        }
        if(!decode_frame_image(packet,src.width,src.height,rgba))return false;
    }
    DibRowCodec decoder(src.depth,src.masks,src.palette),encoder(dst.depth,dst.masks,dst.palette);
    std::vector<unsigned> row(src.width),out(width);
    Bytes partial(dst.stride);
    for(unsigned line=0;line<height;++line){
        const auto sy=line*src.height/height;
        if(src.raw()){
            if(!decoder.decode(packet.subspan((src.top_down?sy:src.height-1-sy)*src.stride,src.stride),row))return false;
        }else std::memcpy(row.data(),rgba.data()+std::size_t(sy)*src.width*4,src.width*4);
        const auto dy=y+line;
        auto destination=pixels.subspan((dst.top_down?dy:dst.height-1-dy)*dst.stride,dst.stride);
        for(unsigned i=0;i<width;++i)out[i]=row[i*src.width/width];
        const auto bit=x*dst.depth,bit_count=width*dst.depth;
        if(!(bit%8) && !(bit_count%8)){
            if(!encoder.encode(out,destination.subspan(bit/8,bit_count/8)))return false;
        }else{
            // 色表 DIB 的矩形可落在位元組中央，左右相鄰像素仍屬呼叫端。
            if(!encoder.encode(out,partial))return false;
            for(unsigned i=0;i<bit_count;++i){
                const auto mask=1U<<(7-(bit+i)%8);
                auto& byte=destination[(bit+i)/8];
                byte=static_cast<std::uint8_t>((byte&~mask)|(((partial[i/8]>>(7-i%8))&1U)?mask:0U));
            }
        }
    }
    return true;
}
}
