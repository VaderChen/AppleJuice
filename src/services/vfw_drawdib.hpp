#pragma once

#include "avi_frame_image.hpp"
#include "x64arm/memory.hpp"
#include "x64arm/windows_palette.hpp"
#include <chrono>

namespace x64arm::vfw {
inline constexpr unsigned draw_update=0x0002, draw_same_dc=0x0004, draw_same_format=0x0008,
    draw_hidden=0x0010, draw_animate=0x0020, draw_direct=0x0080,
    draw_background=0x0200, draw_non_key=0x0400, draw_hurry=0x0800, draw_halftone=0x1000;

struct DrawDib {
    std::uint64_t dc{},memory_dc{},bitmap{},pixels{},codec{},palette{};
    avi::Bytes input,output;
    std::array<std::int32_t,4> sizes{};
    std::array<std::int32_t,2> origin{};
    unsigned flags{},playback_us{};
    bool busy{},has_frame{};
    std::array<std::uint64_t,7> timing{};
};

struct DrawTimer {
    using Clock=std::chrono::steady_clock;
    Clock::time_point start=Clock::now();
    std::uint64_t& counter;
    ~DrawTimer(){
        counter+=static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count());
    }
};

inline bool draw_rectangle(std::int32_t x,std::int32_t y,std::int32_t w,std::int32_t h) {
    return w>0 && h>0 && std::int64_t{x}+w<=INT32_MAX && std::int64_t{y}+h<=INT32_MAX;
}

inline bool same_draw_header(const avi::Bytes& old,const avi::Bytes& current) {
    return old.size()==current.size() && current.size()>=40 &&
        std::equal(old.begin(),old.begin()+20,current.begin()) &&
        std::equal(old.begin()+24,old.end(),current.begin()+24);
}

inline avi::Bytes gdi_draw_header(avi::Bytes header) {
    if(avi::word(header,16)==6 && avi::word(header,0)==40)avi::put(header,0,56);
    if(avi::word(header,16)==6)avi::put(header,16,3);
    return header;
}

inline std::pair<avi::Bytes,avi::Bytes> halftone_dib(GuestMemory& memory,std::uint64_t address,
                                                   const avi::FrameLayout& layout) {
    constexpr std::array<unsigned,16> threshold{0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    const auto palette=gui_dib_palette(8);
    const auto stride=(layout.width+3)&~3U;
    avi::Bytes header(40+palette.size()*4), pixels(static_cast<std::size_t>(stride)*layout.height);
    avi::put(header,0,40);
    avi::put(header,4,layout.width);
    avi::put(header,8,layout.height);
    avi::put(header,12,(8U<<16)|1);
    avi::put(header,20,static_cast<unsigned>(pixels.size()));
    for(unsigned n=0;n<palette.size();++n){
        const auto color=palette[n];
        avi::put(header,40+n*4,((color&255)<<16)|(color&0xff00)|(color>>16));
    }
    const DibRowCodec decode(layout.depth,layout.masks,layout.palette);
    avi::Bytes row(layout.stride);
    std::vector<unsigned> colors(layout.width);
    for(unsigned y=0;y<layout.height;++y){
        memory.read(GuestAddress{address+std::uint64_t{y}*layout.stride},row);
        if(!decode.decode(row,colors))return {};
        for(unsigned x=0;x<layout.width;++x){
            const auto quantize=[&](unsigned level) {
                return (level*16+threshold[(y%4)*4+x%4]*51)/(16*51);
            };
            const auto color=colors[x];
            const auto red=quantize(color&255),green=quantize((color>>8)&255),blue=quantize((color>>16)&255);
            pixels[static_cast<std::size_t>(y)*stride+x]=static_cast<std::uint8_t>(10+red+6*green+36*blue);
        }
    }
    return {std::move(header),std::move(pixels)};
}

inline std::optional<avi::FrameLayout> stretch_layout(GuestMemory& memory,std::uint64_t address){
    if(!address)return {};
    std::array<std::uint8_t,40> header;
    memory.read(GuestAddress{address},header);
    const auto height=avi::word(header,8);
    avi::FrameLayout layout;
    layout.width=avi::word(header,4);
    layout.top_down=height>INT32_MAX;
    layout.height=layout.top_down?0U-height:height;
    layout.depth=avi::word(header,12)>>16;
    if(avi::word(header,0)<40 || !layout.width || !layout.height || layout.width>16384 ||
       layout.height>16384 || std::uint64_t{layout.width}*layout.height>32U*1024*1024 ||
       (layout.depth!=8 && layout.depth!=16 && layout.depth!=24))return {};
    // StretchDIB 逐像素複製原始值，不存取後接的色表或 RGB 位元遮罩。
    layout.stride=(layout.width*layout.depth+31)/32*4;
    return layout;
}

inline void stretch_dib(GuestMemory& memory,const avi::FrameLayout& dst,std::uint64_t destination,
                        const std::array<std::int32_t,4>& dest,const avi::FrameLayout& src,
                        std::uint64_t source,const std::array<std::int32_t,4>& from) {
    if(!destination || !source || dst.depth!=src.depth)return;
    const auto [dx,dy,dw,dh]=dest;
    const auto [sx,sy,sw,sh]=from;
    if(!draw_rectangle(dx,dy,dw,dh) || !draw_rectangle(sx,sy,sw,sh) ||
       sx<0 || sy<0 || std::int64_t{sx}+sw>src.width || std::int64_t{sy}+sh>src.height)return;
    const auto left=std::max(dx,0),top=std::max(dy,0);
    const auto right=std::min(dx+dw,static_cast<std::int32_t>(dst.width));
    const auto bottom=std::min(dy+dh,static_cast<std::int32_t>(dst.height));
    if(left>=right || top>=bottom)return;
    const auto bytes=dst.depth/8;
    const auto row_address=[](std::uint64_t base,const avi::FrameLayout& bmp,unsigned row){
        return base+std::uint64_t{bmp.top_down?row:bmp.height-1-row}*bmp.stride;
    };
    // 先截取來源矩形，讓來源與目的緩衝區重疊時仍依原始像素取樣。
    const auto source_stride=static_cast<std::size_t>(sw)*bytes;
    avi::Bytes input(source_stride*static_cast<std::size_t>(sh)),output(static_cast<std::size_t>(right-left)*bytes);
    for(int row=0;row<sh;++row)
        memory.read(GuestAddress{row_address(source,src,static_cast<unsigned>(sy+row))+std::uint64_t(sx)*bytes},
                    std::span(input).subspan(static_cast<std::size_t>(row)*source_stride,source_stride));
    for(int y=top;y<bottom;++y)
        memory.check(GuestAddress{row_address(destination,dst,static_cast<unsigned>(y))+std::uint64_t(left)*bytes},
                     output.size(),Access::Write);
    for(int y=top;y<bottom;++y){
        const auto row=static_cast<std::size_t>((std::int64_t{y}-dy)*sh/dh)*source_stride;
        for(int x=left;x<right;++x){
            const auto column=static_cast<std::size_t>((std::int64_t{x}-dx)*sw/dw)*bytes;
            std::memcpy(output.data()+static_cast<std::size_t>(x-left)*bytes,input.data()+row+column,bytes);
        }
        memory.write(GuestAddress{row_address(destination,dst,static_cast<unsigned>(y))+std::uint64_t(left)*bytes},output);
    }
}
}
