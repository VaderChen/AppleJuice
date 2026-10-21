#pragma once
#include "pcm_wave_format.hpp"
#include "x64arm/riff_chunk.hpp"
#include <vector>

namespace x64arm {
struct PcmWaveFile {PcmWaveFormat format;std::span<const std::uint8_t> samples;};
// 所有音效來源共用 RIFF 界線、偶數對齊、格式與完整 frame 驗證。
inline std::optional<PcmWaveFile> parse_pcm_wave_file(std::span<const std::uint8_t> bytes,bool allow_empty=false){
    if(bytes.size()<12 || riff::word(bytes)!=riff::riff_id || riff::word(bytes.subspan(8))!=0x45564157)return {};
    const auto end=std::uint64_t{riff::word(bytes.subspan(4))}+8;if(end<12 || end>bytes.size())return {};
    std::optional<PcmWaveFormat> format;std::optional<std::span<const std::uint8_t>> samples;
    try{for(std::uint64_t at=12;at<end;){
        const auto chunk=riff::chunk(bytes.subspan(static_cast<std::size_t>(at)),at,end);
        const auto data=bytes.subspan(static_cast<std::size_t>(chunk.data),chunk.size);
        if(chunk.id==0x20746d66){if(format)return {};format=parse_pcm_wave_format(data,true,true);if(!format)return {};}
        else if(chunk.id==0x61746164){if(samples)return {};samples=data;}
        at=chunk.next;
    }}catch(const std::invalid_argument&){return {};}
    if(!format || !samples || (!allow_empty && samples->empty()) || samples->size()%format->align)return {};
    return PcmWaveFile{*format,*samples};
}
inline std::optional<std::vector<std::uint8_t>> encode_pcm_wave_file(const PcmWaveFormat& format,std::span<const std::uint8_t> samples){
    const unsigned format_size=format.floating?18:16,header_size=format.floating?58:44;
    if(!format.align || samples.size()%format.align || samples.size()>UINT32_MAX-header_size)return {};
    const auto pad=samples.size()&1U;std::vector<std::uint8_t> bytes(header_size+samples.size()+pad);
    const auto put=[&](unsigned at,std::uint32_t value,unsigned width=4){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    put(0,0x46464952);put(4,static_cast<std::uint32_t>(bytes.size()-8));put(8,0x45564157);put(12,0x20746d66);put(16,format_size);
    put(20,format.floating?3:1,2);put(22,format.channels,2);put(24,format.rate);put(28,format.rate*format.align);put(32,format.align,2);put(34,format.bits,2);
    if(!parse_pcm_wave_format(std::span(bytes).subspan(20,format_size),false,true))return {};
    if(format.floating){put(38,0x74636166);put(42,4);put(46,static_cast<std::uint32_t>(samples.size()/format.align));}
    put(header_size-8,0x61746164);put(header_size-4,static_cast<std::uint32_t>(samples.size()));std::copy(samples.begin(),samples.end(),bytes.begin()+header_size);return bytes;
}
}
