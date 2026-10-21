#pragma once
#include "x64arm/audio.hpp"
#include "pcm_wave_file.hpp"
#include <map>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace x64arm::mci {
inline constexpr std::uint32_t notify=1,wait=2,from=4,to=8;
inline constexpr std::uint32_t open=0x803,close=0x804,play=0x806,seek=0x807,stop=0x808,pause=0x809,info=0x80a,getcaps=0x80b,set=0x80d,record=0x80f,sysinfo=0x810,save=0x813,status=0x814,cue=0x830,load=0x850,resume=0x855,erase=0x856;
inline constexpr std::uint32_t wave_device=522,all_devices=UINT32_MAX;
inline constexpr std::uint32_t invalid_device=257,unrecognized=261,hardware=262,memory=264,missing_parameter=273,unsupported=274,file_missing=275,not_ready=276,cannot_all=279,out_of_range=282,conflicting_flags=284,duplicate_alias=289,bad_constant=290,bad_time=293,invalid_file=296,null_parameters=297,type_required=287,not_installed=306,file_read=348,file_write=349;
inline constexpr std::size_t maximum_pcm=64U*1024U*1024U;
struct Error:std::runtime_error {std::uint32_t code;explicit Error(std::uint32_t value):std::runtime_error("MCI 命令參數或裝置狀態無效"),code(value){}};
enum class Mode {Stopped,Playing,Recording,Paused};
inline bool time_format(std::uint32_t value){return value==0 || value==8 || value==9;}
inline std::uint64_t frame(std::uint32_t value,std::uint32_t format,const PcmWaveFormat& pcm){
    if(!pcm.rate || !pcm.align)throw Error(invalid_file);
    if(format==0)return std::uint64_t{value}*pcm.rate/1000;
    if(format==9)return value;
    if(format==8){if(value%pcm.align)throw Error(out_of_range);return value/pcm.align;}
    throw Error(bad_time);
}
inline std::uint32_t time(std::uint64_t frames,std::uint32_t format,const PcmWaveFormat& pcm){
    if(!pcm.rate || !pcm.align)throw Error(invalid_file);std::uint64_t value=0;
    if(format==0){if(frames>UINT64_MAX/1000)throw Error(out_of_range);value=frames*1000/pcm.rate;}
    else if(format==9)value=frames;
    else if(format==8){if(frames>UINT64_MAX/pcm.align)throw Error(out_of_range);value=frames*pcm.align;}
    else throw Error(bad_time);
    if(value>UINT32_MAX)throw Error(out_of_range);return static_cast<std::uint32_t>(value);
}
struct Wave {
    PcmWaveFormat format{2,44100,16,4,false};
    std::vector<std::uint8_t> pcm;
    std::u16string file,alias;
    std::uint64_t creator{},position{},begin{},end{},captured{},notify_window{};
    std::uint32_t time_format{},input_id{UINT32_MAX},output_id{UINT32_MAX},buffer_seconds{1};
    Mode mode{Mode::Stopped},paused_mode{Mode::Playing};
    float left{1},right{1};bool shareable{},dirty{},insert_record{},input_set{},output_set{};
    std::shared_ptr<audio::Buffer> buffer;
    std::unique_ptr<audio::Output> output;
    std::unique_ptr<audio::Input> input;
    std::vector<std::uint8_t> record_tail;
    std::vector<std::uint64_t> waiters;
    std::uint64_t frames()const{return pcm.size()/format.align;}
    std::uint32_t status_mode()const{return mode==Mode::Stopped?525U:mode==Mode::Playing?526U:mode==Mode::Recording?527U:529U;}
    void load(std::span<const std::uint8_t> bytes){
        if(bytes.size()>maximum_pcm+65536)throw Error(memory);const auto decoded=parse_pcm_wave_file(bytes,true);if(!decoded)throw Error(invalid_file);
        if(decoded->samples.size()>maximum_pcm)throw Error(memory);
        auto samples=std::vector<std::uint8_t>(decoded->samples.begin(),decoded->samples.end());pcm=std::move(samples);format=decoded->format;position=0;dirty=false;
    }
    std::uint64_t current()const{
        if(!buffer)return position;std::lock_guard lock(buffer->mutex);
        if(mode==Mode::Recording || (mode==Mode::Paused && paused_mode==Mode::Recording))return begin+std::min(end-begin,buffer->captured_frames-captured);
        if(mode==Mode::Playing || mode==Mode::Paused)return buffer->playing || mode==Mode::Paused?begin+static_cast<std::uint64_t>(buffer->cursor):end;
        return position;
    }
    std::uint64_t media_frames()const{
        if(mode!=Mode::Recording && !(mode==Mode::Paused && paused_mode==Mode::Recording))return frames();
        const auto cursor=current();return insert_record?frames()+cursor-begin:std::max(frames(),cursor);
    }
    std::shared_ptr<audio::Buffer> playback(std::uint64_t first,std::uint64_t last)const{
        if(first>last || last>frames())throw Error(out_of_range);auto value=std::make_shared<audio::Buffer>();
        value->channels=format.channels;value->bits=format.bits;value->rate=value->frequency=format.rate;value->floating=format.floating;value->left=left;value->right=right;
        value->pcm.assign(pcm.begin()+static_cast<std::ptrdiff_t>(first*format.align),pcm.begin()+static_cast<std::ptrdiff_t>(last*format.align));return value;
    }
    std::uint32_t level()const{
        if(!buffer || mode==Mode::Stopped || mode==Mode::Paused)return 0;std::lock_guard lock(buffer->mutex);
        if(buffer->pcm.empty())return 0;const bool recording=mode==Mode::Recording;
        const auto count=recording?buffer->captured_frames-captured:static_cast<std::uint64_t>(buffer->cursor);
        if(!count)return 0;const auto sample=std::min<std::uint64_t>(count-1,buffer->pcm.size()/format.align-1)*format.align;
        const auto channel=[&](unsigned c){const auto at=sample+c*(format.bits/8);double value=0;
            if(format.floating){std::uint32_t bits=0;for(unsigned i=0;i<4;++i)bits|=std::uint32_t{buffer->pcm[at+i]}<<(8*i);value=std::abs(std::bit_cast<float>(bits))*32768;}
            else if(format.bits==8)value=std::abs(int{buffer->pcm[at]}-128);
            else value=std::abs(static_cast<int>(std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(buffer->pcm[at]|(unsigned{buffer->pcm[at+1]}<<8)))));
            if(!recording)value*=c?right:left;if(!std::isfinite(value))value=0;return static_cast<std::uint32_t>(std::clamp(value,0.0,format.bits==8?128.0:32768.0));
        };
        const auto left_level=channel(0);return format.channels==1?left_level:(left_level<<16)|channel(1);
    }
    void finish_transport(){
        if(!buffer){mode=Mode::Stopped;return;}
        std::uint64_t current_position=position;
        {
            std::lock_guard lock(buffer->mutex);
            if(mode==Mode::Recording || (mode==Mode::Paused && paused_mode==Mode::Recording)){
                const auto count=std::min(end-begin,buffer->captured_frames-captured),first=begin*format.align,size=count*format.align;
                auto replacement=pcm;
                if(insert_record){replacement.resize(static_cast<std::size_t>(first+size));std::copy_n(buffer->pcm.begin(),static_cast<std::size_t>(size),replacement.begin()+static_cast<std::ptrdiff_t>(first));replacement.insert(replacement.end(),record_tail.begin(),record_tail.end());}
                else{if(first+size>replacement.size())replacement.resize(static_cast<std::size_t>(first+size));std::copy_n(buffer->pcm.begin(),static_cast<std::size_t>(size),replacement.begin()+static_cast<std::ptrdiff_t>(first));}
                pcm.swap(replacement);
                current_position=begin+count;dirty|=count!=0;
            }else if(mode==Mode::Playing || mode==Mode::Paused)current_position=buffer->playing || mode==Mode::Paused?begin+static_cast<std::uint64_t>(buffer->cursor):end;
            buffer->stop();
        }
        output.reset();input.reset();buffer.reset();record_tail.clear();position=current_position;mode=Mode::Stopped;
    }
    ~Wave(){if(buffer){std::lock_guard lock(buffer->mutex);buffer->stop();}}
};
}
namespace x64arm {
struct MciState {
    std::map<std::uint32_t,std::shared_ptr<mci::Wave>> devices;
    struct Wait {std::uint32_t device{},error{};std::uint64_t owner{};};
    std::map<std::uint64_t,Wait> waits;
    std::uint32_t next_device{1};
};
}
