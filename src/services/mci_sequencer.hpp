#pragma once

#include "mci_wave.hpp"
#include "x64arm/midi.hpp"
#include <algorithm>
#include <array>

namespace x64arm::mci {
inline constexpr unsigned sequencer_device=523,seq_no_port=65533,seq_mapper_port=65535;
inline constexpr unsigned seq_division_base=1216,seq_division_incompatible=336,seq_port_missing=338,seq_port_mapping=339;
inline constexpr unsigned seq_tempo=0x10000,seq_port=0x20000,seq_slave=0x40000,seq_master=0x80000,
    seq_offset=0x1000000;
inline constexpr unsigned seq_smpte_24=4,seq_smpte_25=5,seq_smpte_30=6,seq_smpte_drop=7,seq_song_pointer=0x4001;
inline constexpr unsigned midi_render_rate=48000;

inline unsigned smpte_rate(unsigned format){
    constexpr std::array rates{24U,25U,30U,30U};
    return rates.at(format-seq_smpte_24);
}

inline unsigned sequencer_division(std::uint16_t division){
    if(!(division&0x8000))return seq_division_base;
    switch(256U-(division>>8)){
    case 24:return seq_division_base+1;
    case 25:return seq_division_base+2;
    case 29:return seq_division_base+3;
    default:return seq_division_base+4;
    }
}

struct MidiSequence : DeviceControl {
    std::u16string file,alias;
    std::shared_ptr<const midi::Sequence> sequence;
    std::shared_ptr<midi::Playback> playback;
    std::shared_ptr<midi::Playback::Voice> voice;
    std::unique_ptr<midi::Output> output;
    std::vector<std::uint64_t> waiters;
    Mode mode{Mode::Stopped};
    unsigned format{},port{seq_mapper_port},tempo{},offset{};
    bool left{true},right{true},auto_close{};
    double position{};
    std::uint64_t end{},anchor{},notify_window{};

    std::uint64_t length()const { return sequence?sequence->frames:0; }
    unsigned status_mode()const { return mode==Mode::Stopped?525U:mode==Mode::Playing?526U:529U; }

    bool has_smpte_division()const { return sequence && (sequence->division&0x8000); }
    unsigned offset_format()const {
        const auto code=sequencer_division(sequence->division)-seq_division_base;
        constexpr std::array formats{0U,seq_smpte_24,seq_smpte_25,seq_smpte_drop,seq_smpte_30};
        return formats.at(code);
    }
    static std::uint64_t from_smpte(unsigned format,std::uint32_t value) {
        const unsigned hours=value&255U,minutes=(value>>8)&255U,seconds=(value>>16)&255U,index=value>>24;
        const auto rate=smpte_rate(format);
        if(minutes>=60 || seconds>=60 || index>=rate)throw Error(out_of_range);
        const auto total_minutes=hours*60U+minutes;
        const auto nominal=(total_minutes*60U+seconds)*rate+index;
        if(format!=seq_smpte_drop)return std::uint64_t{nominal}*midi_render_rate/rate;
        if(minutes%10 && !seconds && index<2)throw Error(out_of_range);
        const auto frames=nominal-2*(total_minutes-total_minutes/10);
        return (std::uint64_t{frames}*midi_render_rate*1001+29999)/30000;
    }
    std::uint64_t from_time(std::uint32_t value)const {
        if(!format)return std::uint64_t{value}*48;
        if(format==seq_song_pointer){
            if(!sequence || value>sequence->music_length/192)throw Error(out_of_range);
            return sequence->frame_at(value*192);
        }
        const auto absolute=from_smpte(format,value);
        const auto base=offset?from_smpte(offset_format(),offset):0;
        if(absolute<base)throw Error(out_of_range);
        return absolute-base;
    }
    std::uint32_t as_time(std::uint64_t frames,bool position_value=false)const {
        if(!format){
            if(frames/48>UINT32_MAX)throw Error(out_of_range);
            return static_cast<std::uint32_t>(frames/48);
        }
        if(format==seq_song_pointer)return sequence?sequence->music_at(frames)/192:0;
        if(position_value && offset)frames+=from_smpte(offset_format(),offset);
        const auto rate=smpte_rate(format);
        auto nominal=frames*rate/midi_render_rate;
        if(format==seq_smpte_drop){
            const auto counted=frames*30000/(midi_render_rate*1001ULL);
            const auto rem=counted%17982;
            nominal=counted+18*(counted/17982)+2*(rem>=2?(rem-2)/1798:0);
        }
        const auto seconds=nominal/rate,hours=seconds/3600;
        if(hours>255)throw Error(out_of_range);
        return static_cast<std::uint32_t>(hours|((seconds/60)%60)<<8|(seconds%60)<<16|(nominal%rate)<<24);
    }

    double smpte_tempo()const {
        const auto rate=256U-(sequence->division>>8);
        return rate==29?30000.0/1001:rate;
    }
    unsigned current_tempo(double frame)const {
        if(tempo)return tempo;
        if(!sequence)return 120;
        if(has_smpte_division()){
            const auto rate=256U-(sequence->division>>8);
            return rate==29?30:rate;
        }
        return static_cast<unsigned>(std::lround(sequence->tempo_at(frame)));
    }
    void apply_audio(midi::Playback& target)const {
        target.left_gain=left?1:0;
        target.right_gain=right?1:0;
        target.tempo_override=has_smpte_division()?0:tempo;
        target.tempo_scale=tempo && has_smpte_division()?tempo/smpte_tempo():1;
    }
    void update_audio(){
        if(playback){std::lock_guard lock(playback->mutex);apply_audio(*playback);}
    }
    double silent_position(std::uint64_t now)const {
        if(now<=anchor)return position;
        double remaining=static_cast<double>(now-anchor)*midi_render_rate/10000000;
        if(!tempo)return std::min(static_cast<double>(end),position+remaining);
        if(has_smpte_division())
            return std::min(static_cast<double>(end),position+remaining*tempo/smpte_tempo());
        double frame=position;
        auto point=std::upper_bound(sequence->tempos.begin(),sequence->tempos.end(),frame,
            [](double value,const midi::Tempo& item){return value<static_cast<double>(item.frame);});
        double bpm=point==sequence->tempos.begin()?120:std::prev(point)->bpm;
        while(remaining>0 && frame<static_cast<double>(end)){
            const auto next=point==sequence->tempos.end()?end:std::min(end,point->frame);
            const auto step=(static_cast<double>(next)-frame)*bpm/tempo;
            if(remaining<=step)return frame+remaining*tempo/bpm;
            remaining-=step;
            frame=static_cast<double>(next);
            if(point!=sequence->tempos.end()){bpm=point->bpm;++point;}
        }
        return std::min(frame,static_cast<double>(end));
    }
    double current(std::uint64_t now)const {
        if(mode!=Mode::Playing)return position;
        if(!playback)return silent_position(now);
        std::lock_guard lock(playback->mutex);
        return std::min(voice->position,static_cast<double>(end));
    }
    std::uint32_t completion(std::uint64_t now)const {
        if(mode!=Mode::Playing)return 0;
        if(!playback)return silent_position(now)>=static_cast<double>(end)?1:0;
        std::lock_guard lock(playback->mutex);
        return playback->failed?8:voice->finished?1:0;
    }
    struct Transport {
        std::shared_ptr<midi::Playback> playback;
        std::shared_ptr<midi::Playback::Voice> voice;
        std::unique_ptr<midi::Output> output;
    };
    Transport prepare(double first,std::uint64_t last,unsigned requested_port,bool external_io)const {
        if(first>=static_cast<double>(last) || requested_port==seq_no_port)return {};
        if(!external_io || !midi::output_available())throw Error(seq_port_mapping);
        Transport next;
        next.playback=std::make_shared<midi::Playback>();
        next.voice=std::make_shared<midi::Playback::Voice>();
        next.voice->sequence=sequence;
        next.voice->position=first;
        next.voice->end_frame=last;
        next.voice->start=UINT64_MAX;
        const auto start=static_cast<std::uint64_t>(std::ceil(first));
        auto at=std::lower_bound(sequence->events.begin(),sequence->events.end(),start,
            [](const midi::Event& event,std::uint64_t frame){return event.frame<frame;});
        next.voice->next=static_cast<std::size_t>(at-sequence->events.begin());
        if(start)next.voice->initial_chase=midi::chase(*sequence,start);
        next.playback->voices.push_back(next.voice);
        apply_audio(*next.playback);
        try{next.output=midi::open_output(next.playback);}
        catch(const std::runtime_error&){throw Error(seq_port_mapping);}
        return next;
    }
    void start(Transport&& next,double first,std::uint64_t last,std::uint64_t now) {
        output=std::move(next.output);
        voice=std::move(next.voice);
        playback=std::move(next.playback);
        position=first;
        end=last;
        anchor=now;
        mode=first<static_cast<double>(last)?Mode::Playing:Mode::Stopped;
        if(playback){
            std::lock_guard lock(playback->mutex);
            voice->start=playback->clock_frames;
        }
    }
    void finish(std::uint64_t now,Mode final_mode=Mode::Stopped){
        position=current(now);
        output.reset();
        voice.reset();
        playback.reset();
        mode=final_mode;
    }
};
}
