#pragma once

#include "cd_audio_disc.hpp"
#include "mci_wave.hpp"

namespace x64arm::mci {

inline constexpr unsigned cd_device=516,cd_msf=2,cd_tmsf=10,cd_track_type=0x4001;
inline constexpr unsigned cd_track_audio=1088,cd_track_data=1089,cd_no_identity=350;

struct CdTray {
    std::string cue;
    std::shared_ptr<const cdda::Disc> disc;
    bool open{};
};

struct CdaDevice : DeviceControl {
    std::shared_ptr<CdTray> tray;
    std::u16string file,alias;
    bool shareable{},auto_close{},left{true},right{true};
    unsigned format{cd_msf};
    Mode mode{Mode::Stopped};
    std::vector<std::uint64_t> waiters;
    std::uint64_t position{},read_position{},queue_start{},play_end{},notify_window{};
    std::shared_ptr<audio::Buffer> buffer;
    std::unique_ptr<audio::Output> output;
    static constexpr std::size_t refill_frames=16384;

    bool media_present()const{return tray && !tray->open && tray->disc;}
    unsigned status_mode()const{
        if(!media_present())return tray && tray->open?530U:524U;
        return mode==Mode::Paused?529U:mode==Mode::Playing?526U:525U;
    }
    const cdda::Disc& disc()const{
        if(!media_present())throw Error(not_ready);
        return *tray->disc;
    }
    const cdda::Disc::Track& track(unsigned number)const{
        const auto& tracks=disc().tracks();
        if(number<tracks.front().number || number>tracks.back().number)throw Error(out_of_range);
        return tracks[number-tracks.front().number];
    }
    std::uint32_t next_track(unsigned number)const{
        const auto& tracks=disc().tracks();
        return number==tracks.back().number?disc().sectors():track(number+1).sector;
    }
    std::uint32_t from_time(std::uint32_t value)const{
        const auto& media=disc();
        if(!format){
            if(!value)return media.tracks().front().sector;
            value=static_cast<std::uint32_t>((std::uint64_t{value-1}*cdda::sectors_per_second+500)/1000);
        }else{
            const auto track_number=value&255U;
            if(format==cd_tmsf)value>>=8;
            else if(value>>24)throw Error(bad_time);
            const auto minutes=value&255U,seconds=(value>>8)&255U,frames=(value>>16)&255U;
            if(seconds>=60 || frames>=cdda::sectors_per_second)throw Error(bad_time);
            value=(minutes*60+seconds)*cdda::sectors_per_second+frames;
            if(format==cd_tmsf){
                const auto& begin=track(track_number);
                if(value>next_track(begin.number)-begin.sector)throw Error(out_of_range);
                return begin.sector+value;
            }
        }
        if(value<cdda::lead_in || value-cdda::lead_in>media.sectors())throw Error(out_of_range);
        return value-cdda::lead_in;
    }
    static std::uint32_t pack_msf(std::uint32_t sectors){
        const auto seconds=sectors/cdda::sectors_per_second;
        return seconds/60|((seconds%60)<<8)|((sectors%cdda::sectors_per_second)<<16);
    }
    std::uint32_t to_time(std::uint32_t sector,bool duration=false)const{
        const auto absolute=duration?sector:sector+cdda::lead_in;
        if(!format)return absolute*1000/cdda::sectors_per_second+1;
        if(duration || format==cd_msf)return pack_msf(absolute);
        const auto& selected=disc().track_at(sector);
        return selected.number|(pack_msf(sector-selected.sector)<<8);
    }
    std::uint32_t next_audio(std::uint32_t sector)const{
        const auto& media=disc();
        if(sector>=media.sectors())return media.sectors();
        const auto& begin=media.track_at(sector);
        if(begin.audio)return std::max(sector,begin.sector);
        const auto following=std::find_if(media.tracks().begin(),media.tracks().end(),
            [&](const auto& candidate){return candidate.sector>sector && candidate.audio;});
        return following==media.tracks().end()?media.sectors():following->sector;
    }
    std::uint64_t current()const{
        if(!buffer)return position;
        std::lock_guard lock(buffer->mutex);
        return std::min(play_end,queue_start+static_cast<std::uint64_t>(buffer->cursor));
    }
    std::uint32_t current_sector()const{return static_cast<std::uint32_t>(current()/cdda::samples_per_sector);}
    void update_volume(){
        if(!buffer)return;
        std::lock_guard lock(buffer->mutex);
        buffer->left=left?1:0;
        buffer->right=right?1:0;
    }
    void finish(){
        position=current();
        if(buffer){std::lock_guard lock(buffer->mutex);buffer->stop();}
        output.reset();
        buffer.reset();
        mode=Mode::Stopped;
    }
    void pause(){
        if(mode!=Mode::Playing || !buffer)return;
        std::lock_guard lock(buffer->mutex);
        buffer->playing=false;
        mode=Mode::Paused;
    }
    void resume(){
        if(mode!=Mode::Paused || !buffer)return;
        std::lock_guard lock(buffer->mutex);
        buffer->playing=true;
        mode=Mode::Playing;
    }
    struct Transport {
        std::shared_ptr<audio::Buffer> buffer;
        std::unique_ptr<audio::Output> output;
        std::uint64_t begin{},end{},next{};
    };
    Transport prepare(std::uint64_t begin,std::uint64_t end,bool external_io)const{
        Transport next{{},{},begin,end,begin};
        if(begin==end)return next;
        if(!external_io || !audio::output_available())throw Error(hardware);
        auto queue=std::make_shared<audio::Buffer>();
        queue->rate=queue->frequency=cdda::sample_rate;
        queue->streaming=true;
        queue->left=left?1:0;
        queue->right=right?1:0;
        const auto frames=static_cast<std::size_t>(std::min<std::uint64_t>(end-begin,refill_frames*2));
        try{queue->pcm=disc().audio(begin,frames);}catch(const std::runtime_error&){throw Error(file_read);}
        queue->pcm.reserve(refill_frames*8+refill_frames*4);
        try{next.output=audio::open_output(queue);}catch(const std::runtime_error&){throw Error(hardware);}
        if(!next.output)throw Error(hardware);
        next.buffer=std::move(queue);
        next.next+=frames;
        return next;
    }
    void start(Transport&& source){
        output=std::move(source.output);
        buffer=std::move(source.buffer);
        position=queue_start=source.begin;
        play_end=source.end;
        read_position=source.next;
        mode=buffer?Mode::Playing:Mode::Stopped;
        if(buffer){std::lock_guard lock(buffer->mutex);buffer->playing=true;}
    }
    unsigned pump(){
        if(mode!=Mode::Playing || !buffer)return 0;
        const auto frame=current();
        if(frame>=play_end)return 1;
        if(read_position>=play_end || read_position-frame>refill_frames)return 0;
        const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(play_end-read_position,refill_frames));
        std::vector<std::uint8_t> next;
        try{next=disc().audio(read_position,count);}catch(const std::exception&){return 8;}
        {
            std::lock_guard lock(buffer->mutex);
            const auto used=static_cast<std::size_t>(buffer->cursor);
            buffer->pcm.erase(buffer->pcm.begin(),buffer->pcm.begin()+static_cast<std::ptrdiff_t>(used*4));
            buffer->cursor-=static_cast<double>(used);
            queue_start+=used;
            buffer->pcm.insert(buffer->pcm.end(),next.begin(),next.end());
        }
        read_position+=count;
        return 0;
    }
};

}
