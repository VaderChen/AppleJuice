#pragma once

#include "avi_preview.hpp"
#include "mci_wave.hpp"

namespace x64arm::mci {
inline constexpr unsigned avi_device=520,time_frames=3,video_reverse=0x20000,video_repeat=0x10000;
inline constexpr unsigned video_window=0x841,video_put=0x842,video_where=0x843,video_update=0x854,
    video_step=0x80e,video_set_audio=0x873;
inline constexpr unsigned video_open_style=0x10000,video_open_parent=0x20000,video_open_nostatic=0x40000;
struct AviDevice : DeviceControl {
    explicit AviDevice(std::unique_ptr<VideoPreview> movie):clip(std::move(movie)){
        if(auto track=clip->take_audio()){
            wave.format=track->format;
            wave.pcm=std::move(track->pcm);
        }
        source={0,0,static_cast<std::int32_t>(clip->width()),static_cast<std::int32_t>(clip->height())};
        target=source;
    }
    unsigned format{time_frames},speed{1000},left{1000},right{1000};
    bool reverse{},loop{},shareable{},auto_close{},quartz{};
    Mode mode{Mode::Stopped};
    std::u16string file,alias;
    std::uint64_t position{},anchor{},low{},high{},window{},notify_window{};
    GuiRect source{},target{};
    bool fixed_target{};
    std::uint32_t presentation{};
    std::uint64_t presented_frame{UINT64_MAX};
    bool paint_pending{};
    std::vector<std::uint64_t> waiters;
    Wave wave;
    std::unique_ptr<VideoPreview> clip;
    std::uint64_t length()const{return clip->frames();}
    bool audible()const{return !wave.pcm.empty();}
    unsigned status_mode()const{return mode==Mode::Stopped?525U:mode==Mode::Playing?526U:529U;}
    std::uint64_t from_time(std::uint32_t value)const{
        if(format==time_frames)return value;
        const auto ticks=std::uint64_t{value}*10000;
        if(!quartz)return ticks/clip->interval();
        return ticks>clip->time_at(length())?length()+1:clip->frame_at(ticks);
    }
    std::uint32_t as_time(std::uint64_t value)const{
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(
            format==time_frames?value:clip->time_at(value)/10000,UINT32_MAX));
    }
    std::uint64_t elapsed_ticks(std::uint64_t now)const{
        if(now<=anchor)return 0;
        return static_cast<std::uint64_t>(std::min<__int128>(static_cast<__int128>(now-anchor)*speed/1000,UINT64_MAX));
    }
    std::uint64_t elapsed_frames(std::uint64_t now)const{
        if(now<=anchor)return 0;
        const auto ticks=now-anchor,divisor=1000*clip->interval();
        return (ticks/divisor)*speed+((ticks%divisor)*speed)/divisor;
    }
    std::uint64_t current(std::uint64_t now)const {
        if(mode!=Mode::Playing || now<=anchor)return position;
        if(quartz && wave.buffer){
            std::lock_guard lock(wave.buffer->mutex);
            if(!wave.buffer->playing)return reverse?low:high;
            const auto ticks=static_cast<std::uint64_t>(wave.buffer->cursor*10000000.0/wave.format.rate);
            const auto begin=clip->time_at(low),end=clip->time_at(high);
            return clip->frame_at(reverse?end-1-std::min(ticks,end-begin-1):std::min(begin+ticks,end-1));
        }
        if(quartz && !reverse){
            const auto first=clip->time_at(position),last=clip->time_at(high),begin=clip->time_at(low);
            const auto elapsed=elapsed_ticks(now);
            if(elapsed<last-first)return clip->frame_at(first+elapsed);
            if(loop && last>begin)return clip->frame_at(begin+(elapsed-(last-first))%(last-begin));
            return high;
        }
        const auto count=elapsed_frames(now);
        if(reverse){
            if(count<=position-low)return position-count;
            if(loop && high>low)return high-1-(count-(position-low)-1)%(high-low);
            return low;
        }
        if(count<high-position)return position+count;
        if(loop && high>low)return low+(count+position-low)%(high-low);
        return high;
    }
    bool ended(std::uint64_t now)const {
        if(loop || mode!=Mode::Playing)return false;
        if(quartz && wave.buffer){
            std::lock_guard lock(wave.buffer->mutex);
            return !wave.buffer->playing;
        }
        if(quartz && !reverse)return elapsed_ticks(now)>=clip->time_at(high)-clip->time_at(position);
        return elapsed_frames(now)>=(reverse?position-low+1:high-position);
    }
    void stop(std::uint64_t now){
        position=current(now);
        wave.finish_transport();
        mode=Mode::Stopped;
    }
    void pause(std::uint64_t now){
        position=current(now);
        if(wave.buffer){std::lock_guard lock(wave.buffer->mutex);wave.buffer->playing=false;}
        mode=Mode::Paused;
    }
    void resume(std::uint64_t now){
        anchor=now;
        if(wave.buffer){std::lock_guard lock(wave.buffer->mutex);wave.buffer->playing=true;}
        mode=Mode::Playing;
    }
    void play(std::uint64_t first,std::uint64_t last,bool backwards,bool repeat,std::uint64_t now){
        std::shared_ptr<audio::Buffer> buffer;
        std::unique_ptr<audio::Output> output;
        if(audible() && first<last){
            const auto sample=[&](std::uint64_t frame){
                constexpr std::uint64_t second=10000000;
                const auto ticks=clip->time_at(frame);
                const auto samples=(ticks/second)*wave.format.rate+((ticks%second)*wave.format.rate)/second;
                return samples;
            };
            const auto start=std::min(wave.frames(),sample(first)),end=std::min(wave.frames(),sample(last));
            const auto required=quartz?sample(last)-sample(first):end-start;
            if(required){
                constexpr std::uint64_t maximum_playback=128U*1024U*1024U;
                if(quartz && required>maximum_playback/wave.format.align)throw Error(memory);
                buffer=wave.playback(start,end);
                if(quartz)buffer->pcm.resize(static_cast<std::size_t>(required)*wave.format.align,wave.format.bits==8?128:0);
                buffer->frequency=std::max(1U,wave.format.rate*speed/1000U);
                buffer->left=static_cast<float>(left)/1000.F;
                buffer->right=static_cast<float>(right)/1000.F;
                buffer->looping=repeat;
                if(backwards){
                    const auto alignment=wave.format.align;
                    for(std::size_t l=0,r=buffer->pcm.size()-alignment;l<r;l+=alignment,r-=alignment)
                        std::swap_ranges(buffer->pcm.begin()+static_cast<std::ptrdiff_t>(l),
                                         buffer->pcm.begin()+static_cast<std::ptrdiff_t>(l+alignment),
                                         buffer->pcm.begin()+static_cast<std::ptrdiff_t>(r));
                }
                try{output=audio::open_output(buffer);}
                catch(const std::runtime_error&){throw Error(hardware);}
                if(!output)throw Error(hardware);
            }
        }
        wave.finish_transport();
        wave.buffer=std::move(buffer);
        wave.output=std::move(output);
        wave.mode=wave.buffer?Mode::Playing:Mode::Stopped;
        if(wave.buffer){std::lock_guard lock(wave.buffer->mutex);wave.buffer->playing=true;}
        low=first;
        high=last;
        position=backwards && last>first?last-1:first;
        reverse=backwards;
        loop=repeat && first!=last;
        anchor=now;
        mode=first==last?Mode::Stopped:Mode::Playing;
    }
};
}
