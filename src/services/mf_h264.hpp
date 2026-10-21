#pragma once
#include "x64arm/video_codec.hpp"
#include <algorithm>
namespace x64arm::mf {
inline std::vector<std::uint8_t> h264_annex_header(const std::vector<std::vector<std::uint8_t>>& parameters){
    std::vector<std::uint8_t> bytes;
    for(const auto& nal:parameters){bytes.insert(bytes.end(),{0,0,0,1});bytes.insert(bytes.end(),nal.begin(),nal.end());}
    return bytes;
}
inline std::vector<std::uint8_t> h264_annex_packet(const video::EncodedPacket& packet){
    auto bytes=packet.keyframe?h264_annex_header(packet.parameter_sets):std::vector<std::uint8_t>{};
    for(std::size_t at=0;at<packet.avcc.size();){
        if(packet.avcc.size()-at<4)throw std::invalid_argument("編碼 AVCC 長度欄位截斷");
        std::size_t size=0;for(unsigned n=0;n<4;++n)size=(size<<8)|packet.avcc[at++];
        if(!size || size>packet.avcc.size()-at)throw std::invalid_argument("編碼 AVCC NAL 截斷");
        bytes.insert(bytes.end(),{0,0,0,1});bytes.insert(bytes.end(),packet.avcc.begin()+static_cast<std::ptrdiff_t>(at),packet.avcc.begin()+static_cast<std::ptrdiff_t>(at+size));at+=size;
    }
    return bytes;
}
// 共用 AVCC／Annex B 轉換；保留 NAL 內容，不修改壓縮影像。
inline std::vector<std::vector<std::uint8_t>> annex_nals(std::span<const std::uint8_t> bytes){
    std::vector<std::vector<std::uint8_t>> result;
    const auto start=[&](std::size_t at)->std::size_t {if(at+3<=bytes.size() && !bytes[at] && !bytes[at+1]){if(bytes[at+2]==1)return 3;if(at+4<=bytes.size() && !bytes[at+2] && bytes[at+3]==1)return 4;}return 0;};
    std::size_t at=0;while(at<bytes.size() && !start(at))++at;
    while(at<bytes.size()){const auto begin=at+start(at);auto end=begin;while(end<bytes.size() && !start(end))++end;auto payload=end;while(payload>begin && !bytes[payload-1])--payload;if(payload>begin)result.emplace_back(bytes.begin()+static_cast<std::ptrdiff_t>(begin),bytes.begin()+static_cast<std::ptrdiff_t>(payload));at=end;}return result;
}
inline video::DecoderConfig h264_config(std::span<const std::uint8_t> header){
    video::DecoderConfig config;
    if(header.size()>=7 && header[0]==1){
        config.nal_length_size=(header[4]&3U)+1U;std::size_t at=6;
        const auto collect=[&](unsigned count){for(unsigned n=0;n<count;++n){if(at+2>header.size())throw std::invalid_argument("AVCC 參數集長度欄位截斷");const auto size=static_cast<std::size_t>(header[at])*256+header[at+1];at+=2;if(!size || size>header.size()-at)throw std::invalid_argument("AVCC 參數集截斷");config.parameter_sets.emplace_back(header.begin()+static_cast<std::ptrdiff_t>(at),header.begin()+static_cast<std::ptrdiff_t>(at+size));at+=size;}};
        collect(header[5]&31U);if(at>=header.size())throw std::invalid_argument("AVCC 缺少 PPS 數量");collect(header[at++]);
    }else for(auto& nal:annex_nals(header))if((nal[0]&31U)==7 || (nal[0]&31U)==8)config.parameter_sets.push_back(std::move(nal));
    return config;
}
inline std::vector<std::uint8_t> h264_packet(std::span<const std::uint8_t> bytes,unsigned length_size){
    std::vector<std::vector<std::uint8_t>> nals;
    if(bytes.size()>=3 && !bytes[0] && !bytes[1] && (bytes[2]==1 || (bytes.size()>=4 && !bytes[2] && bytes[3]==1)))nals=annex_nals(bytes);
    else {
        if(!length_size || length_size>4)throw std::invalid_argument("AVCC NAL 長度欄位無效");std::size_t at=0;
        while(at<bytes.size()){if(bytes.size()-at<length_size)throw std::invalid_argument("AVCC sample 截斷");std::size_t size=0;for(unsigned n=0;n<length_size;++n)size=(size<<8)|bytes[at++];if(!size || size>bytes.size()-at)throw std::invalid_argument("AVCC NAL 截斷");nals.emplace_back(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+size));at+=size;}
    }
    std::vector<std::uint8_t> packet;for(const auto& nal:nals){const auto size=nal.size();for(int shift=24;shift>=0;shift-=8)packet.push_back(static_cast<std::uint8_t>(size>>shift));packet.insert(packet.end(),nal.begin(),nal.end());}return packet;
}
}
