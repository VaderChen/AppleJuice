#include "x64arm/midi.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <map>
#include <cmath>
#include "directmusic_sequence.hpp"
namespace x64arm::midi {
void SoundBank::set_patch(std::size_t index,std::uint32_t patch){
    if(index>=instruments.size() || (patch&~0x807f7f7fU))throw std::invalid_argument("DLS 樂器編號無效");
    auto& item=instruments[index];if(item.header>bytes.size() || bytes.size()-item.header<12)throw std::invalid_argument("DLS 樂器標頭無效");
    const auto bank=(patch&0x80000000U)|((patch>>8)&0x7f7f),program=patch&127U;
    for(unsigned i=0;i<4;++i){bytes[item.header+4+i]=static_cast<std::uint8_t>(bank>>(8*i));bytes[item.header+8+i]=static_cast<std::uint8_t>(program>>(8*i));}item.patch=patch;
}
SoundBank parse_sound_bank(std::span<const std::uint8_t> bytes){
    using namespace directmusic_file;
    if(bytes.size()>256*1024*1024)throw std::invalid_argument("DLS 音色庫超過大小上限");
    const auto roots=chunks(bytes);if(roots.size()!=1 || roots[0].id!="RIFF" || tag(roots[0].bytes)!="DLS ")throw std::invalid_argument("不是 DLS 音色庫");
    SoundBank bank;bank.bytes.assign(bytes.begin(),bytes.end());unsigned expected=UINT32_MAX;
    for(const auto& c:chunks(roots[0].bytes.subspan(4))){
        if(c.id=="dlid"){if(c.bytes.size()!=16)throw std::invalid_argument("DLS GUID 無效");std::copy(c.bytes.begin(),c.bytes.end(),bank.id.begin());}
        else if(c.id=="colh")expected=static_cast<unsigned>(number(c.bytes,0));
        else if(c.id=="LIST" && tag(c.bytes)=="lins")for(const auto& instrument:chunks(c.bytes.subspan(4))){
            if(instrument.id!="LIST" || tag(instrument.bytes)!="ins ")throw std::invalid_argument("DLS 樂器清單無效");SoundBank::Instrument item;bool found=false;
            for(const auto& part:chunks(instrument.bytes.subspan(4))){
                if(part.id=="insh"){const auto locale=number(part.bytes,4),patch=number(part.bytes,8);if((locale&~UINT64_C(0x80007f7f)) || patch>127)throw std::invalid_argument("DLS 樂器編號無效");item.patch=static_cast<std::uint32_t>((locale&0x80000000U)|((locale&0x7f7f)<<8)|patch);item.header=static_cast<std::size_t>(part.bytes.data()-bytes.data());found=true;}
                else if(part.id=="LIST" && tag(part.bytes)=="INFO")for(const auto& text:chunks(part.bytes.subspan(4)))if(text.id=="INAM")for(auto character:text.bytes){if(!character)break;item.name.push_back(static_cast<char16_t>(character));}
            }if(!found)throw std::invalid_argument("DLS 樂器缺少標頭");bank.instruments.push_back(std::move(item));
        }
    }
    if(bank.instruments.empty() || bank.instruments.size()!=expected)throw std::invalid_argument("DLS 樂器數量不符");
    // Core Audio 會把奇數長度 INFO 字串的 RIFF pad 誤認成下一個 chunk。
    // 將已存在的零 pad 納入字串長度；總長、pool offset、音樂與版權資料均不變。
    std::function<void(Bytes,bool,unsigned)> normalize=[&](Bytes data,bool info,unsigned depth){
        if(depth>16)throw std::invalid_argument("DLS RIFF 巢狀層數過多");
        for(const auto& c:chunks(data)){
            if(c.id=="RIFF" || c.id=="LIST")normalize(c.bytes.subspan(4),tag(c.bytes)=="INFO",depth+1);
            else if(info && (c.bytes.size()&1) && c.bytes.back()==0){const auto offset=static_cast<std::size_t>(c.bytes.data()-bytes.data());if(bytes[offset+c.bytes.size()]==0){const auto size=c.bytes.size()+1;for(unsigned i=0;i<4;++i)bank.bytes[offset-4+i]=static_cast<std::uint8_t>(size>>(8*i));}}
        }
    };normalize(bytes,false,0);return bank;
}
SoundBank SoundBank::select_instrument(std::size_t index,std::span<const NoteRange> ranges) const {
    using namespace directmusic_file;using Buffer=std::vector<std::uint8_t>;
    if(index>=instruments.size())throw std::invalid_argument("DLS 樂器索引無效");
    for(const auto& r:ranges)if(r.low>r.high || r.high>127)throw std::invalid_argument("DLS 音域無效");
    const auto append=[](Buffer& dst,Bytes src){if(src.size()>256*1024*1024-dst.size())throw std::invalid_argument("DLS 音色庫過大");dst.insert(dst.end(),src.begin(),src.end());};
    const auto write=[](Buffer& dst,std::size_t at,std::uint32_t v){if(at>dst.size() || dst.size()-at<4)throw std::invalid_argument("DLS 寫入欄位無效");for(unsigned i=0;i<4;++i)dst[at+i]=static_cast<std::uint8_t>(v>>(8*i));};
    const auto pack=[&](std::string_view id,Bytes data){Buffer out(id.begin(),id.end());out.resize(8);write(out,4,static_cast<std::uint32_t>(data.size()));append(out,data);if(data.size()&1)out.push_back(0);return out;};
    const auto root=chunks(bytes);if(root.size()!=1 || root[0].id!="RIFF" || tag(root[0].bytes)!="DLS ")throw std::invalid_argument("DLS 音色庫根區塊無效");
    const auto top=chunks(root[0].bytes.subspan(4));Bytes selected,pool,waves;
    for(const auto& c:top){
        if(c.id=="LIST" && tag(c.bytes)=="lins"){const auto list=chunks(c.bytes.subspan(4));if(!selected.empty() || index>=list.size())throw std::invalid_argument("DLS 樂器清單無效");selected=list[index].bytes;if(list[index].id!="LIST" || tag(selected)!="ins ")throw std::invalid_argument("DLS 樂器區塊無效");}
        else if(c.id=="ptbl"){if(!pool.empty())throw std::invalid_argument("DLS 重複波形索引");pool=c.bytes;}
        else if(c.id=="LIST" && tag(c.bytes)=="wvpl"){if(!waves.empty())throw std::invalid_argument("DLS 重複波形池");waves=c.bytes.subspan(4);}
    }
    if(selected.empty() || pool.empty())throw std::invalid_argument("DLS 樂器或波形索引缺失");
    const auto pool_header=number(pool,0),cue_count=number(pool,4);
    if(pool_header<8 || pool_header>pool.size() || cue_count>(pool.size()-pool_header)/4)throw std::invalid_argument("DLS 波形索引截斷");
    std::map<std::uint32_t,Bytes> wave_chunks;
    for(const auto& w:chunks(waves)){if(w.id!="LIST" || tag(w.bytes)!="wave")throw std::invalid_argument("DLS 波形區塊無效");wave_chunks.emplace(static_cast<std::uint32_t>(w.bytes.data()-waves.data()-8),w.bytes);}
    Buffer instrument{'i','n','s',' '},wavepool{'w','v','p','l'},regions{'l','r','g','n'};std::vector<std::uint32_t> offsets;std::map<std::uint32_t,std::uint32_t> remap;
    std::uint32_t region_count=0,original_regions=0;std::size_t header_offset=0;bool has_regions=false;
    const auto parts=chunks(selected.subspan(4));
    for(const auto& part:parts)if(part.id=="LIST" && tag(part.bytes)=="lrgn"){
        if(has_regions)throw std::invalid_argument("DLS 重複音域清單");has_regions=true;
        for(const auto& region:chunks(part.bytes.subspan(4))){
            if(region.id!="LIST" || (tag(region.bytes)!="rgn " && tag(region.bytes)!="rgn2"))throw std::invalid_argument("DLS 音域區塊無效");
            const auto fields=chunks(region.bytes.subspan(4));bool include=ranges.empty(),range_found=false,link_found=false;std::uint32_t cue=0;std::size_t link_offset=0;
            for(const auto& f:fields)if(f.id=="rgnh"){
                if(range_found)throw std::invalid_argument("DLS 重複音域標頭");range_found=true;const auto low=number(f.bytes,0,2),high=number(f.bytes,2,2);if(low>high || high>127)throw std::invalid_argument("DLS 音域界線無效");
                for(const auto& r:ranges)include=include || (r.low<=high && r.high>=low);
            }else if(f.id=="wlnk"){
                if(link_found)throw std::invalid_argument("DLS 重複波形連結");link_found=true;cue=static_cast<std::uint32_t>(number(f.bytes,8));link_offset=static_cast<std::size_t>(f.bytes.data()-region.bytes.data())+8;
            }
            ++original_regions;if(!range_found || !link_found || cue>=cue_count)throw std::invalid_argument("DLS 音域連結無效");if(!include)continue;
            const auto offset=static_cast<std::uint32_t>(number(pool,static_cast<std::size_t>(pool_header+cue*4)));const auto wave=wave_chunks.find(offset);if(wave==wave_chunks.end())throw std::invalid_argument("DLS 波形索引未指向波形");
            if(!remap.contains(offset)){remap.emplace(offset,static_cast<std::uint32_t>(offsets.size()));offsets.push_back(static_cast<std::uint32_t>(wavepool.size()-4));append(wavepool,pack("LIST",wave->second));}
            Buffer data(region.bytes.begin(),region.bytes.end());write(data,link_offset,remap.at(offset));append(regions,pack("LIST",data));++region_count;
        }
    }
    for(const auto& part:parts){
        if(part.id=="insh"){
            if(header_offset || number(part.bytes,0)!=original_regions || part.bytes.size()<12)throw std::invalid_argument("DLS 音域數量不符");
            header_offset=instrument.size()+8;Buffer data(part.bytes.begin(),part.bytes.end());write(data,0,region_count);append(instrument,pack(part.id,data));
        }else if(part.id=="LIST" && tag(part.bytes)=="lrgn")append(instrument,pack("LIST",regions));
        else append(instrument,pack(part.id,part.bytes));
    }
    if(!header_offset || !has_regions)throw std::invalid_argument("DLS 樂器缺少標頭或音域");
    Buffer list{'l','i','n','s'};append(list,pack("LIST",instrument));Buffer table(8+offsets.size()*4);write(table,0,8);write(table,4,static_cast<std::uint32_t>(offsets.size()));for(std::size_t i=0;i<offsets.size();++i)write(table,8+i*4,offsets[i]);
    Buffer body{'D','L','S',' '};for(const auto& c:top){
        if(c.id=="colh"){Buffer count(4);write(count,0,1);append(body,pack(c.id,count));}
        else if(c.id=="LIST" && tag(c.bytes)=="lins")append(body,pack("LIST",list));
        else if(c.id=="ptbl")append(body,pack(c.id,table));
        else if(c.id=="LIST" && tag(c.bytes)=="wvpl")append(body,pack("LIST",wavepool));
        else append(body,pack(c.id,c.bytes));
    }
    return parse_sound_bank(pack("RIFF",body));
}
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
    if(bytes.size()>=4 && std::equal(bytes.begin(),bytes.begin()+4,"RIFF"))return parse_directmusic(bytes);
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
