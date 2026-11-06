#pragma once
#include "x64arm/video_codec.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace x64arm::mf {
// Sink 要求的未壓縮影像配置。未指定尺寸時沿用 Windows H.264 解碼器的 16 對齊幾何（UE 依此計算色度平面位置）。
enum class VideoFormat {Nv12,Yv12,I420,Yuy2,Uyvy,Bgra};
struct VideoLayout {VideoFormat format{};std::uint32_t width{},height{},stride{},lines{};std::size_t size{};bool bottom_up{};};
inline VideoLayout video_layout(VideoFormat format,std::uint32_t width,std::uint32_t height,bool tight,std::int64_t requested_stride){
    const auto align=[](std::uint32_t value){return (value+15U)&~15U;};
    VideoLayout layout{format,width,height,0,height,0,false};
    const auto minimum=format==VideoFormat::Bgra?width*4U:format==VideoFormat::Yuy2 || format==VideoFormat::Uyvy?width*2U:(tight?width:align(width));
    const auto wanted=static_cast<std::uint64_t>(requested_stride<0?-requested_stride:requested_stride);
    layout.stride=wanted>=minimum && wanted<=UINT32_MAX?static_cast<std::uint32_t>(wanted):minimum;
    layout.bottom_up=format==VideoFormat::Bgra && requested_stride<0;
    switch(format){
    case VideoFormat::Nv12:case VideoFormat::Yv12:case VideoFormat::I420:
        layout.lines=tight?height:align(height);layout.size=static_cast<std::size_t>(layout.stride)*layout.lines*3/2;break;
    default:layout.size=static_cast<std::size_t>(layout.stride)*height;break;
    }
    return layout;
}
// 解碼器給緊密 NV12；這裡在媒體工作執行緒轉為 sink 配置，不接觸客體記憶體。
inline std::vector<std::uint8_t> convert_video_frame(video::DecodedFrame& frame,const VideoLayout& out){
    const auto source=frame.materialize();const auto width=std::min(frame.width,out.width),height=std::min(frame.height,out.height);
    if(source.size()<static_cast<std::size_t>(frame.width)*frame.height*3/2)throw std::runtime_error("解碼畫面資料不完整");
    const auto* luma=source.data();const auto* chroma=source.data()+static_cast<std::size_t>(frame.width)*frame.height;
    std::vector<std::uint8_t> bytes(out.size);
    switch(out.format){
    case VideoFormat::Nv12:{
        std::fill(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(out.stride)*out.lines),std::uint8_t{16});std::fill(bytes.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(out.stride)*out.lines),bytes.end(),std::uint8_t{128});
        for(std::uint32_t y=0;y<height;++y)std::memcpy(bytes.data()+static_cast<std::size_t>(y)*out.stride,luma+static_cast<std::size_t>(y)*frame.width,width);
        auto* target=bytes.data()+static_cast<std::size_t>(out.stride)*out.lines;
        for(std::uint32_t y=0;y<height/2;++y)std::memcpy(target+static_cast<std::size_t>(y)*out.stride,chroma+static_cast<std::size_t>(y)*frame.width,width);
        break;}
    case VideoFormat::Yv12:case VideoFormat::I420:{
        std::fill(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(out.stride)*out.lines),std::uint8_t{16});std::fill(bytes.begin()+static_cast<std::ptrdiff_t>(static_cast<std::size_t>(out.stride)*out.lines),bytes.end(),std::uint8_t{128});
        for(std::uint32_t y=0;y<height;++y)std::memcpy(bytes.data()+static_cast<std::size_t>(y)*out.stride,luma+static_cast<std::size_t>(y)*frame.width,width);
        const auto half=out.stride/2;auto* first=bytes.data()+static_cast<std::size_t>(out.stride)*out.lines;auto* second=first+static_cast<std::size_t>(half)*(out.lines/2);
        auto* u=out.format==VideoFormat::I420?first:second;auto* v=out.format==VideoFormat::I420?second:first;
        for(std::uint32_t y=0;y<height/2;++y){const auto* row=chroma+static_cast<std::size_t>(y)*frame.width;for(std::uint32_t x=0;x<width/2;++x){u[static_cast<std::size_t>(y)*half+x]=row[x*2];v[static_cast<std::size_t>(y)*half+x]=row[x*2+1];}}
        break;}
    case VideoFormat::Yuy2:case VideoFormat::Uyvy:{
        const bool luma_first=out.format==VideoFormat::Yuy2;
        for(std::uint32_t y=0;y<height;++y){
            const auto* row=luma+static_cast<std::size_t>(y)*frame.width;const auto* uv=chroma+static_cast<std::size_t>(y/2)*frame.width;auto* target=bytes.data()+static_cast<std::size_t>(y)*out.stride;
            for(std::uint32_t x=0;x<width;++x){const auto c=uv[x];target[x*2+(luma_first?0:1)]=row[x];target[x*2+(luma_first?1:0)]=c;}
        }
        break;}
    case VideoFormat::Bgra:{
        // 影像矩陣依解析度約定選 BT.601／BT.709，範圍依解碼器回報；定點係數放大 256 倍。
        const bool hd=frame.height>=720,full=frame.full_range;
        const int y_scale=full?256:298,r_v=full?(hd?403:359):(hd?459:409),g_u=full?(hd?48:88):(hd?55:100),g_v=full?(hd?120:183):(hd?136:208),b_u=full?(hd?475:454):(hd?541:516);
        const auto clamp=[](int value){return static_cast<std::uint8_t>(std::clamp(value>>8,0,255));};
        for(std::uint32_t y=0;y<height;++y){
            const auto* row=luma+static_cast<std::size_t>(y)*frame.width;const auto* uv=chroma+static_cast<std::size_t>(y/2)*frame.width;
            auto* target=bytes.data()+static_cast<std::size_t>(out.bottom_up?out.height-1-y:y)*out.stride;
            for(std::uint32_t x=0;x<width;++x){
                const int l=(static_cast<int>(row[x])-(full?0:16))*y_scale+128,u=static_cast<int>(uv[x&~1U])-128,v=static_cast<int>(uv[x|1U])-128;
                target[x*4]=clamp(l+b_u*u);target[x*4+1]=clamp(l-g_u*u-g_v*v);target[x*4+2]=clamp(l+r_v*v);target[x*4+3]=255;
            }
        }
        break;}
    }
    return bytes;
}
}
