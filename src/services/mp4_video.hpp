#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace x64arm::mp4 {
// 容器只處理 ISO BMFF 結構；編碼與客體 ABI 留在各自共用流程。
struct Sample {std::uint64_t offset{};std::uint32_t size{},duration{};std::int64_t pts{},dts{};bool key{};};
struct Boxes {
    std::vector<std::uint8_t> bytes;
    void number(std::uint64_t value,unsigned width=4){for(unsigned i=width;i;--i)bytes.push_back(static_cast<std::uint8_t>(value>>(8*(i-1))));}
    void zeros(std::size_t count){bytes.insert(bytes.end(),count,0);}
    void text(std::string_view value){bytes.insert(bytes.end(),value.begin(),value.end());}
    void append(std::span<const std::uint8_t> value){bytes.insert(bytes.end(),value.begin(),value.end());}
    std::size_t begin(std::string_view type){const auto at=bytes.size();number(0);text(type);return at;}
    void end(std::size_t at){const auto size=bytes.size()-at;if(size>UINT32_MAX)throw std::invalid_argument("MP4 metadata 過大");for(unsigned n=0;n<4;++n)bytes[at+n]=static_cast<std::uint8_t>(size>>(24-8*n));}
    void matrix(){for(const auto value:std::array<std::uint32_t,9>{0x10000,0,0,0,0x10000,0,0,0,0x40000000})number(value);}
};
inline std::vector<std::uint8_t> header(){Boxes b;auto box=b.begin("ftyp");b.text("isom");b.number(512);b.text("isomiso2avc1mp41");b.end(box);b.number(1);b.text("mdat");b.number(16,8);return b.bytes;}
inline std::vector<std::uint8_t> movie(unsigned width,unsigned height,const std::vector<std::vector<std::uint8_t>>& parameters,std::span<const Sample> samples){
    if(!width || !height || width>65535 || height>65535 || samples.empty() || samples.size()>10000000)throw std::invalid_argument("MP4 影片範圍無效");
    std::vector<std::span<const std::uint8_t>> sps,pps;
    for(const auto& nal:parameters){if(nal.empty() || nal.size()>65535)throw std::invalid_argument("MP4 AVC 參數集無效");if((nal[0]&31)==7)sps.push_back(nal);else if((nal[0]&31)==8)pps.push_back(nal);}
    if(sps.empty() || pps.empty() || sps.size()>31 || pps.size()>255 || sps.front().size()<4)throw std::invalid_argument("MP4 缺少 AVC 參數集");
    const auto total=static_cast<__int128>(samples.back().dts)-samples.front().dts+samples.back().duration;
    if(total<=0 || total>UINT64_MAX)throw std::invalid_argument("MP4 時間長度溢位");
    Boxes b;const auto moov=b.begin("moov"),mvhd=b.begin("mvhd");b.number(0x1000000);b.zeros(16);b.number(10000000);b.number(static_cast<std::uint64_t>(total),8);b.number(0x10000);b.number(0x100,2);b.zeros(10);b.matrix();b.zeros(24);b.number(2);b.end(mvhd);
    const auto trak=b.begin("trak"),tkhd=b.begin("tkhd");b.number(0x1000007);b.zeros(16);b.number(1);b.number(0);b.number(static_cast<std::uint64_t>(total),8);b.zeros(16);b.matrix();b.number(width<<16);b.number(height<<16);b.end(tkhd);
    const auto mdia=b.begin("mdia"),mdhd=b.begin("mdhd");b.number(0x1000000);b.zeros(16);b.number(10000000);b.number(static_cast<std::uint64_t>(total),8);b.number(0x55c4,2);b.number(0,2);b.end(mdhd);
    const auto hdlr=b.begin("hdlr");b.zeros(8);b.text("vide");b.zeros(12);b.text("VideoHandler");b.number(0,1);b.end(hdlr);
    const auto minf=b.begin("minf"),vmhd=b.begin("vmhd");b.number(1);b.zeros(8);b.end(vmhd);
    const auto dinf=b.begin("dinf"),dref=b.begin("dref");b.number(0);b.number(1);const auto url=b.begin("url ");b.number(1);b.end(url);b.end(dref);b.end(dinf);
    const auto stbl=b.begin("stbl"),stsd=b.begin("stsd");b.number(0);b.number(1);const auto avc1=b.begin("avc1");b.zeros(6);b.number(1,2);b.zeros(16);b.number(width,2);b.number(height,2);b.number(0x480000);b.number(0x480000);b.number(0);b.number(1,2);b.zeros(32);b.number(24,2);b.number(0xffff,2);
    const auto avcc=b.begin("avcC");b.number(1,1);b.number(sps.front()[1],1);b.number(sps.front()[2],1);b.number(sps.front()[3],1);b.number(0xff,1);b.number(0xe0|sps.size(),1);
    for(const auto nal:sps){b.number(nal.size(),2);b.append(nal);}b.number(pps.size(),1);for(const auto nal:pps){b.number(nal.size(),2);b.append(nal);}b.end(avcc);b.end(avc1);b.end(stsd);
    const auto stts=b.begin("stts");b.number(0);b.number(samples.size());
    for(std::size_t i=0;i<samples.size();++i){const auto duration=i+1<samples.size()?static_cast<__int128>(samples[i+1].dts)-samples[i].dts:samples[i].duration;if(duration<=0 || duration>UINT32_MAX)throw std::invalid_argument("MP4 解碼時間必須遞增");b.number(1);b.number(static_cast<std::uint32_t>(duration));}b.end(stts);
    const auto ctts=b.begin("ctts");b.number(0x1000000);b.number(samples.size());for(const auto& sample:samples){const auto offset=static_cast<__int128>(sample.pts)-sample.dts;if(offset<INT32_MIN || offset>INT32_MAX)throw std::invalid_argument("MP4 顯示時間差溢位");b.number(1);b.number(static_cast<std::uint32_t>(offset));}b.end(ctts);
    const auto stsc=b.begin("stsc");b.number(0);b.number(1);b.number(1);b.number(1);b.number(1);b.end(stsc);
    const auto stsz=b.begin("stsz");b.number(0);b.number(0);b.number(samples.size());for(const auto& sample:samples)b.number(sample.size);b.end(stsz);
    const auto co64=b.begin("co64");b.number(0);b.number(samples.size());for(const auto& sample:samples)b.number(sample.offset,8);b.end(co64);
    const auto stss=b.begin("stss");b.number(0);std::uint32_t keys=0;for(const auto& sample:samples)keys+=sample.key;b.number(keys);for(std::size_t i=0;i<samples.size();++i)if(samples[i].key)b.number(i+1);b.end(stss);
    b.end(stbl);b.end(minf);b.end(mdia);b.end(trak);b.end(moov);return b.bytes;
}
}
