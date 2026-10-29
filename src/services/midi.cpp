#include "x64arm/midi.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <map>
#include <cmath>
namespace x64arm::midi {
namespace {
struct Reader {
    std::span<const std::uint8_t> bytes;std::size_t at{};
    std::uint8_t byte(){if(at==bytes.size())throw std::invalid_argument("truncated MIDI");return bytes[at++];}
    std::uint32_t number(unsigned n){std::uint32_t v=0;while(n--)v=(v<<8)|byte();return v;}
    std::uint32_t vlq(){std::uint32_t v=0;for(unsigned i=0;i<4;++i){const auto c=byte();v=(v<<7)|(c&127U);if(!(c&128))return v;}throw std::invalid_argument("invalid MIDI VLQ");}
    std::span<const std::uint8_t> take(std::size_t n){if(n>bytes.size()-at)throw std::invalid_argument("truncated MIDI chunk");const auto v=bytes.subspan(at,n);at+=n;return v;}
};
struct Raw {std::uint64_t tick;std::uint32_t tempo;std::uint8_t status,first,second;std::vector<std::uint8_t> sysex{};};
}
Sequence parse(std::span<const std::uint8_t> bytes){
    if(bytes.size()>16*1024*1024)throw std::invalid_argument("MIDI exceeds 16 MiB");
    Reader r{bytes};if(r.number(4)!=0x4d546864)throw std::invalid_argument("not a standard MIDI file");
    const auto size=r.number(4);if(size<6)throw std::invalid_argument("invalid MIDI header");Reader h{r.take(size)};
    const auto format=h.number(2),tracks=h.number(2),division=h.number(2);
    if(format>1 || !tracks || tracks>128 || (format==0 && tracks!=1) || !division || (division&0x8000))throw std::invalid_argument("unsupported MIDI format/division");
    std::vector<Raw> raw;std::vector<TimeSignature> signatures;std::uint64_t end=0;unsigned event_count=0;
    for(unsigned track=0;track<tracks;++track){
        if(r.number(4)!=0x4d54726b)throw std::invalid_argument("missing MIDI track");const auto length=r.number(4);Reader t{r.take(length)};
        std::uint64_t tick=0;std::uint8_t running=0;bool ended=false;std::vector<std::uint8_t> pending_sysex;
        while(t.at<t.bytes.size()){
            tick+=t.vlq();if(tick>0x7fffffffU)throw std::invalid_argument("MIDI tick limit");
            auto status=t.byte();if(status<0x80){if(!running)throw std::invalid_argument("missing running status");--t.at;status=running;}
            if(++event_count>200000)throw std::invalid_argument("MIDI event limit");
            if(status==0xff){
                running=0;const auto type=t.byte();const auto n=t.vlq();const auto data=t.take(n);
                if(type==0x2f){if(n || t.at!=t.bytes.size())throw std::invalid_argument("invalid end of track");ended=true;break;}
                if(type==0x51){if(n!=3)throw std::invalid_argument("invalid tempo");const auto tempo=(std::uint32_t{data[0]}<<16)|(std::uint32_t{data[1]}<<8)|data[2];if(!tempo)throw std::invalid_argument("zero tempo");raw.push_back({tick,tempo,0,0,0});}
                if(type==0x58){if(n!=4 || !data[0] || data[1]>7)throw std::invalid_argument("invalid MIDI time signature");signatures.push_back({static_cast<std::uint32_t>(tick*768/division),data[0],static_cast<std::uint8_t>(1U<<data[1]),4});}
            }else if(status==0xf0 || status==0xf7){
                running=0;const auto data=t.take(t.vlq());
                if(status==0xf0){if(!pending_sysex.empty())throw std::invalid_argument("unfinished MIDI SysEx");pending_sysex.push_back(0xf0);}
                else if(pending_sysex.empty() && (data.empty() || data.front()!=0xf0))throw std::invalid_argument("unsupported MIDI escape event");
                pending_sysex.insert(pending_sysex.end(),data.begin(),data.end());
                if(pending_sysex.back()==0xf7){if(pending_sysex.size()<2 || pending_sysex.front()!=0xf0 || std::any_of(pending_sysex.begin()+1,pending_sysex.end()-1,[](auto value){return value>=0x80;}))throw std::invalid_argument("invalid MIDI SysEx data");raw.push_back({tick,0,0xf0,0,0,std::move(pending_sysex)});pending_sysex.clear();}
            }else if(status>=0xf0){throw std::invalid_argument("unsupported MIDI system event");}
            else {
                if(!pending_sysex.empty())throw std::invalid_argument("channel event interrupts MIDI SysEx");
                running=status;const auto a=t.byte();const auto b=(status&0xe0)==0xc0?std::uint8_t{0}:t.byte();
                if(a>=128 || b>=128)throw std::invalid_argument("invalid MIDI data byte");raw.push_back({tick,0,status,a,b});
            }
            if(raw.size()>200000)throw std::invalid_argument("MIDI event limit");
        }
        if(!ended)throw std::invalid_argument("missing end of track");if(!pending_sysex.empty())throw std::invalid_argument("unfinished MIDI SysEx");end=std::max(end,tick);
    }
    if(r.at!=bytes.size())throw std::invalid_argument("trailing MIDI data");
    std::stable_sort(raw.begin(),raw.end(),[](const auto& a,const auto& b){return a.tick<b.tick;});
    Sequence seq;std::uint64_t previous=0,scaled=0;std::uint32_t tempo=500000;
    const auto frame=[&](std::uint64_t tick){scaled+=(tick-previous)*tempo;previous=tick;const auto f=scaled*48/(static_cast<std::uint64_t>(division)*1000);if(f>48000*600ULL)throw std::invalid_argument("MIDI exceeds ten minutes");return f;};
    for(auto& e:raw){const auto at=frame(e.tick);if(e.tempo){tempo=e.tempo;seq.tempos.push_back({static_cast<std::uint32_t>(e.tick*768/division),at,60000000.0/tempo});}else seq.events.push_back({at,e.status,e.first,e.second,std::move(e.sysex)});}
    std::stable_sort(signatures.begin(),signatures.end(),[](const auto& a,const auto& b){return a.music<b.music;});seq.signatures.insert(seq.signatures.end(),signatures.begin(),signatures.end());
    seq.frames=std::max<std::uint64_t>(1,frame(end));const auto music=end*768/division;if(music>INT32_MAX)throw std::invalid_argument("MUSIC_TIME overflow");seq.music_length=static_cast<std::uint32_t>(music);return seq;
}
std::uint64_t Sequence::frame_at(std::uint32_t music)const{
    const Tempo* point=&tempos.front();for(const auto& t:tempos){if(t.music>music)break;point=&t;}
    return point->frame+static_cast<std::uint64_t>(static_cast<double>(music-point->music)*48000*60/(768*point->bpm));
}
std::uint32_t Sequence::music_at(std::uint64_t frame)const{
    const Tempo* point=&tempos.front();for(const auto& t:tempos){if(t.frame>frame)break;point=&t;}
    return point->music+static_cast<std::uint32_t>(static_cast<double>(frame-point->frame)*768*point->bpm/(48000*60));
}
std::vector<Event> chase(const Sequence& sequence,std::uint64_t frame){
    std::map<unsigned,Event> controls;std::array<std::array<std::uint8_t,128>,16> notes{};std::vector<Event> result;
    for(const auto& e:sequence.events){if(e.frame>=frame)break;const auto c=e.status&15U,kind=e.status&0xf0U;
        if(!e.sysex.empty()){result.push_back(e);result.back().frame=0;}
        else if(kind==0x90)notes[c][e.first]=e.second;else if(kind==0x80)notes[c][e.first]=0;
        else {controls[(static_cast<unsigned>(e.status)<<8)|(kind==0xb0?e.first:0U)]=e;if(kind==0xb0 && (e.first==120 || e.first==123))notes[c].fill(0);}
    }
    for(const auto& [key,e]:controls){(void)key;result.push_back({0,e.status,e.first,e.second});}
    for(unsigned c=0;c<16;++c)for(unsigned n=0;n<128;++n)if(notes[c][n])result.push_back({0,static_cast<std::uint8_t>(0x90+c),static_cast<std::uint8_t>(n),notes[c][n]});return result;
}
bool Stream::valid_division(std::uint32_t value){
    value&=0xffff;
    if(!(value&0x8000))return value!=0;
    const auto rate=256U-(value>>8);
    return (value&255U) && (rate==24 || rate==25 || rate==29 || rate==30);
}
std::vector<Stream::Event> Stream::parse(std::span<const std::uint8_t> source,std::uint8_t& running){
    if(source.empty() || source.size()>=65536)throw std::invalid_argument("MIDI 串流緩衝區大小無效");
    std::vector<Event> result;auto status=running;
    const auto load=[&](std::size_t at){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=static_cast<std::uint32_t>(source[at+i])<<(8*i);return value;};
    for(std::size_t at=0;at<source.size();){
        if(source.size()-at<12 || load(at+4))throw std::invalid_argument("MIDI 串流事件標頭無效");
        Event event;event.delta=load(at);event.word=load(at+8);event.offset=static_cast<std::uint32_t>(at);event.bytes=12;
        const auto type=(event.word>>24)&~0x40U,param=event.word&0xffffffU;
        if(type&0x80U){
            const auto padded=(static_cast<std::size_t>(param)+3)&~std::size_t{3};
            if(padded>source.size()-at-12 || (type!=0x80 && type!=0x82 && type!=0x84))throw std::invalid_argument("MIDI 串流長事件無效");
            if(type==0x80){const auto data=source.subspan(at+12,param);event.data.assign(data.begin(),data.end());status=0;}
            event.bytes+=static_cast<std::uint32_t>(padded);
        }else if(type==0){
            auto s=static_cast<std::uint8_t>(param),a=static_cast<std::uint8_t>(param>>8),b=static_cast<std::uint8_t>(param>>16);
            if(s<0x80){b=a;a=s;s=status;}
            unsigned count=s<0xf0?((s&0xe0)==0xc0?1:2):(s==0xf2?2:(s==0xf1 || s==0xf3?1:0));
            if(s<0x80 || s==0xf0 || s==0xf4 || s==0xf5 || s==0xf7 || s==0xf9 || s==0xfd || (count && a>=128) || (count==2 && b>=128))throw std::invalid_argument("MIDI 串流短事件無效");
            event.word=(event.word&0xff000000U)|s|(static_cast<std::uint32_t>(count?a:0)<<8)|(static_cast<std::uint32_t>(count==2?b:0)<<16);
            if(s<0xf0)status=s;else if(s<0xf8)status=0;
        }else if(type==1){if(!param)throw std::invalid_argument("MIDI 速度不可為零");}
        else if(type!=2)throw std::invalid_argument("未知的 MIDI 串流事件");
        at+=event.bytes;result.push_back(std::move(event));
    }
    running=status;return result;
}
void Stream::advance(unsigned count,Playback::Send send,void* context,Playback::SendSysex sysex){
    if(!running)return;
    unsigned offset=0;
    while(offset<count){
        long double rate;
        if(division&0x8000){const auto fps=256U-((division>>8)&255U);rate=(fps==29?30000.L/1001:fps)*(division&255U)/48000.L;}
        else rate=static_cast<long double>(division)*1000000.L/(static_cast<long double>(tempo)*48000.L);
        auto found=std::find_if(buffers.begin(),buffers.end(),[](const auto& buffer){return !buffer->done;});
        if(found==buffers.end()){ticks+=rate*(count-offset);frames+=count-offset;break;}
        auto& buffer=**found;const auto& event=buffer.events[buffer.next];
        if(!armed){event_tick+=event.delta;armed=true;}
        if(event_tick>ticks){
            const auto needed=std::ceil((event_tick-ticks)/rate-1e-12L);
            const auto step=static_cast<unsigned>(std::min<long double>(count-offset,std::max(needed,1.L)));
            ticks+=rate*step;frames+=step;offset+=step;if(offset==count)break;
        }
        const auto type=(event.word>>24)&~0x40U;
        if(type==0){
            const auto status=static_cast<std::uint8_t>(event.word),a=static_cast<std::uint8_t>(event.word>>8),b=static_cast<std::uint8_t>(event.word>>16);
            if((status&0xf0)==0x90)notes[status&15][a]=b!=0;
            else if((status&0xf0)==0x80)notes[status&15][a]=false;
            else if((status&0xf0)==0xb0 && (a==120 || a==123))notes[status&15].fill(false);
            send(context,status,a,b,offset);
        }else if(type==1)tempo=event.word&0xffffffU;
        else if(type==0x80 && sysex && !event.data.empty())sysex(context,event.data);
        bytes+=event.bytes;armed=false;
        if(++buffer.next==buffer.events.size())buffer.done=true;
    }
}
void Playback::advance(unsigned count,Send send,void* context,SendSysex sysex){
    const auto silence=[&](unsigned offset){for(unsigned c=0;c<16;++c){send(context,static_cast<std::uint8_t>(0xb0+c),120,0,offset);send(context,static_cast<std::uint8_t>(0xb0+c),121,0,offset);send(context,static_cast<std::uint8_t>(0xc0+c),0,0,offset);}};
    if(reset){silence(0);for(auto& channel:note_owners)channel.fill(0);for(auto& channel:direct_notes)channel.fill({});active_notes=0;reset=false;}
    if(stream)stream->advance(count,send,context,sysex);
    const auto direct=[&](const Event& e,unsigned frame){
        if(!e.sysex.empty()){if(sysex)sysex(context,e.sysex);return;}
        if(voice_limit){
            const auto channel=e.status&15U,type=e.status&0xf0U;auto& note=direct_notes[channel][e.first];
            if(type==0x90 && e.second && !note.active){
                if(active_notes>=voice_limit){
                    unsigned victim_channel=16,victim_note=0;std::uint32_t priority=UINT32_MAX;std::uint64_t order=UINT64_MAX;
                    for(unsigned c=0;c<16;++c)for(unsigned n=0;n<128;++n)if(direct_notes[c][n].active && (channel_priorities[c]<priority || (channel_priorities[c]==priority && direct_notes[c][n].order<order))){victim_channel=c;victim_note=n;priority=channel_priorities[c];order=direct_notes[c][n].order;}
                    ++lost_notes;
                    if(victim_channel==16 || channel_priorities[channel]<priority)return;
                    send(context,static_cast<std::uint8_t>(0x80U+victim_channel),static_cast<std::uint8_t>(victim_note),0,frame);direct_notes[victim_channel][victim_note].active=false;--active_notes;
                }
                note.active=true;note.order=++note_serial;++active_notes;
            }else if((type==0x80 || (type==0x90 && !e.second)) && note.active){note.active=false;--active_notes;}
            else if(type==0xb0 && (e.first==120 || e.first==123))for(auto& n:direct_notes[channel])if(n.active){n.active=false;--active_notes;}
        }
        send(context,e.status,e.first,e.second,frame);
    };
    std::stable_sort(messages.begin(),messages.end(),[](const auto& a,const auto& b){return a.frame<b.frame;});
    std::size_t consumed=0;
    for(unsigned i=0;i<count;++i){
        const auto clock=clock_frames+i;
        while(consumed<messages.size() && messages[consumed].frame<=clock){
            const auto& message=messages[consumed++];
            for(const auto& e:message.events)direct(e,i);
            if(!message.sysex.empty() && sysex)sysex(context,message.sysex);
            if(message.tempo>0)tempo_override=message.tempo;
        }
        for(const auto& voice:voices){auto& v=*voice;if(v.finished || !v.sequence)continue;
            const auto emit=[&](const Event& e){
                if(!e.sysex.empty()){if(sysex)sysex(context,e.sysex);return;}
                const auto c=e.status&15U,kind=e.status&0xf0U;const auto n=e.first;
                if(kind==0x90 && e.second){if(!v.notes[c][n]){v.notes[c][n]=true;++note_owners[c][n];}}
                else if(kind==0x80 || (kind==0x90 && !e.second)){
                    if(v.notes[c][n]){v.notes[c][n]=false;if(note_owners[c][n])--note_owners[c][n];}
                    if(note_owners[c][n])return;
                }
                send(context,e.status,e.first,e.second,i);
            };
            const auto notes_off=[&]{for(unsigned c=0;c<16;++c)for(unsigned n=0;n<128;++n)if(v.notes[c][n])emit({0,static_cast<std::uint8_t>(0x80+c),static_cast<std::uint8_t>(n),0});};
            if(!v.playing || !active || clock>=v.stop){notes_off();v.playing=false;v.finished=true;v.aborted=true;continue;}
            if(clock<v.start)continue;
            if(!v.chased){for(const auto& e:v.initial_chase)emit(e);v.chased=true;}v.started=true;
            while(v.next<v.sequence->events.size() && static_cast<double>(v.sequence->events[v.next].frame)<=v.position)emit(v.sequence->events[v.next++]);
            auto speed=tempo_scale;
            if(tempo_override>0){const Tempo* current=&v.sequence->tempos.front();for(const auto& t:v.sequence->tempos){if(static_cast<double>(t.frame)>v.position)break;current=&t;}speed*=tempo_override/current->bpm;}
            v.position+=speed;
            const auto end=v.repeats && v.loop_end?v.loop_end:v.sequence->frames;
            if(v.position>=static_cast<double>(end)){if(end==v.sequence->frames)while(v.next<v.sequence->events.size() && v.sequence->events[v.next].frame<=end){const auto& e=v.sequence->events[v.next++];if(!e.sysex.empty())emit(e);}notes_off();if(v.repeats){if(v.repeats!=UINT32_MAX)--v.repeats;++v.loops;v.position=static_cast<double>(v.loop_start);v.next=static_cast<std::size_t>(std::lower_bound(v.sequence->events.begin(),v.sequence->events.end(),v.loop_start,[](const Event& e,std::uint64_t f){return e.frame<f;})-v.sequence->events.begin());v.initial_chase=v.loop_chase;v.chased=false;}
                else {v.playing=false;v.finished=true;}}
        }
    }
    messages.erase(messages.begin(),messages.begin()+static_cast<std::ptrdiff_t>(consumed));
    std::erase_if(voices,[](const auto& v){return v->finished;});
    clock_frames+=count;
    unsigned offset=0;
    while(playing && sequence && offset<count){
        const auto remaining=sequence->frames-position;
        const auto step=static_cast<unsigned>(std::min<std::uint64_t>(remaining,count-offset));
        while(next<sequence->events.size() && sequence->events[next].frame<position+step){const auto& e=sequence->events[next++];if(!e.sysex.empty()){if(sysex)sysex(context,e.sysex);}else send(context,e.status,e.first,e.second,offset+static_cast<unsigned>(e.frame-position));}
        position+=step;elapsed+=step;offset+=step;
        if(position==sequence->frames){
            while(next<sequence->events.size()){const auto& e=sequence->events[next++];if(!e.sysex.empty() && sysex)sysex(context,e.sysex);}
            // Events exactly at the end (typically note-off) are covered by silence.
            if(offset==count){reset=true;}else silence(offset);
            if(repeats){if(repeats!=UINT32_MAX)--repeats;position=0;next=0;}
            else {playing=false;position=0;}
        }
    }
}
}
