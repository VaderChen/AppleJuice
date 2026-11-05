#pragma once
#include "pcm_wave_format.hpp"
#include "service_callback_task.hpp"
#include "x64arm/riff_chunk.hpp"
#include <vector>

namespace x64arm {
struct PcmWaveFile {PcmWaveFormat format;std::span<const std::uint8_t> samples;};
struct PcmWaveStream {PcmWaveFormat format;std::uint64_t offset{},bytes{};};
// 檔案來源沿用相同格式與 RIFF 規則，只讀 metadata；音訊依需求從既有 HostFile 取樣。
template<class Read> service_callback::Task<std::optional<PcmWaveStream>> parse_pcm_wave_stream_async(std::uint64_t size,Read read){
    if(size<12)co_return std::optional<PcmWaveStream>{};std::array<std::uint8_t,12> header{};co_await read(0,std::span(header));
    if(riff::word(header)!=riff::riff_id || riff::word(std::span(header).subspan(8))!=0x45564157)co_return std::optional<PcmWaveStream>{};
    const auto end=std::uint64_t{riff::word(std::span(header).subspan(4))}+8;if(end<12 || end>size)co_return std::optional<PcmWaveStream>{};
    std::optional<PcmWaveFormat> format;std::optional<PcmWaveStream> stream;
    for(std::uint64_t at=12;at<end;){
        if(end-at<8)co_return std::optional<PcmWaveStream>{};std::array<std::uint8_t,8> chunk{};co_await read(at,std::span(chunk));
        riff::Chunk part;try{part=riff::chunk(chunk,at,end);}catch(const std::invalid_argument&){co_return std::optional<PcmWaveStream>{};}
        const auto id=part.id,count=part.size;const auto payload=part.data;
        if(id==0x20746d66){
            if(format || count<16 || count>274)co_return std::optional<PcmWaveStream>{};std::array<std::uint8_t,274> bytes{};auto data=std::span(bytes).first(count);co_await read(payload,data);
            format=parse_pcm_wave_format(data,true,true,100,384000,{64,32,false,true});if(!format)co_return std::optional<PcmWaveStream>{};
        }else if(id==0x61746164){if(stream)co_return std::optional<PcmWaveStream>{};stream=PcmWaveStream{{},payload,count};}
        at=part.next;
    }
    if(!format || !stream || stream->bytes%format->align)co_return std::optional<PcmWaveStream>{};stream->format=*format;co_return stream;
}
struct PcmWaveReadReady {
    bool await_ready()const noexcept{return true;}
    void await_suspend(std::coroutine_handle<>)const noexcept{}
    void await_resume()const noexcept{}
};
template<class Read> std::optional<PcmWaveStream> parse_pcm_wave_stream(std::uint64_t size,Read read){
    auto task=parse_pcm_wave_stream_async(size,[&](std::uint64_t at,std::span<std::uint8_t> bytes){read(at,bytes);return PcmWaveReadReady{};});
    task.address().resume();return task.take();
}
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
