#pragma once
#include "x64arm/midi.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace x64arm::midi {
namespace directmusic_file {
using Bytes=std::span<const std::uint8_t>;
inline std::uint64_t number(Bytes b,std::size_t offset,unsigned size=4){
    if(offset>b.size() || size>b.size()-offset)throw std::invalid_argument("DirectMusic RIFF 欄位截斷");
    std::uint64_t v=0;for(unsigned i=0;i<size;++i)v|=std::uint64_t{b[offset+i]}<<(8*i);return v;
}
inline std::int32_t time(Bytes b,std::size_t offset=0){return static_cast<std::int32_t>(number(b,offset));}
inline std::string_view tag(Bytes b,std::size_t offset=0){number(b,offset);return {reinterpret_cast<const char*>(b.data()+offset),4};}
struct Chunk {std::string_view id;Bytes bytes;};
inline std::vector<Chunk> chunks(Bytes b){
    std::vector<Chunk> out;
    for(std::size_t at=0;at<b.size();){
        const auto id=tag(b,at);const auto n=number(b,at+4);at+=8;
        if(n>b.size()-at || (n&1)>b.size()-at-n)throw std::invalid_argument("DirectMusic RIFF 區塊長度無效");
        out.push_back({id,b.subspan(at,static_cast<std::size_t>(n))});at+=static_cast<std::size_t>(n+(n&1));
        if(out.size()>200000)throw std::invalid_argument("DirectMusic RIFF 區塊過多");
    }return out;
}
inline void records(Bytes b,unsigned minimum,const std::function<void(Bytes)>& read){
    const auto size=number(b,0);if(size<minimum || size>4096 || (b.size()-4)%size)throw std::invalid_argument("DirectMusic 紀錄大小無效");
    for(std::size_t at=4;at<b.size();at+=static_cast<std::size_t>(size))read(b.subspan(at,static_cast<std::size_t>(size)));
}
struct Timed {std::int64_t tick{};unsigned priority{};Event event;};
struct Curve {std::int64_t at{},duration{},reset_duration{};unsigned channel{},type{},shape{},cc{},flags{},parameter{};int from{},to{},reset{};};
}
inline Sequence parse_directmusic(std::span<const std::uint8_t> bytes){
    using namespace directmusic_file;
    const auto roots=chunks(bytes);if(roots.size()!=1 || roots[0].id!="RIFF" || tag(roots[0].bytes)!="DMSG")throw std::invalid_argument("不是 DirectMusic 樂段");
    Sequence seq;seq.segment.present=true;std::vector<Timed> events;std::vector<Curve> curves;bool header=false;
    const auto emit=[&](std::int64_t tick,unsigned priority,std::uint8_t status,std::uint8_t a,std::uint8_t b){
        if(a>127 || b>127 || status<0x80 || status>=0xf0)throw std::invalid_argument("DirectMusic MIDI 事件無效");
        if(events.size()>=400000)throw std::invalid_argument("DirectMusic 事件過多");
        events.push_back({std::max<std::int64_t>(0,tick),priority,{0,status,a,b}});
    };
    const auto channel=[](Bytes b,unsigned offset){const auto c=number(b,offset);if(c>=16)throw std::invalid_argument("DirectMusic PChannel 超出裝置範圍");return static_cast<std::uint8_t>(c);};
    std::function<void(Bytes,std::int64_t,unsigned)> walk;
    walk=[&](Bytes data,std::int64_t band_time,unsigned depth){
        if(depth>16)throw std::invalid_argument("DirectMusic RIFF 巢狀層數過多");
        for(const auto& c:chunks(data)){
            const auto b=c.bytes;
            if(c.id=="RIFF" || c.id=="LIST"){
                const auto form=tag(b);
                if(form=="UNFO" || form=="INFO" || form=="sgdl")continue;
                // Chord 軌只供互動式編曲使用；固定音高的 sequence 不做和弦轉調。
                if(form=="cord")continue;
                if(form=="DMRF"){
                    Sequence::Segment::Bank bank;
                    for(const auto& r:chunks(b.subspan(4))){if(r.id=="guid"){if(r.bytes.size()!=16)throw std::invalid_argument("DLS GUID 長度無效");std::copy(r.bytes.begin(),r.bytes.end(),bank.id.begin());}
                        else if(r.id=="file"){if(r.bytes.size()%2 || r.bytes.size()>520)throw std::invalid_argument("DLS 檔名長度無效");for(std::size_t i=0;i<r.bytes.size();i+=2){const auto ch=static_cast<char16_t>(number(r.bytes,i,2));if(!ch)break;bank.filename.push_back(ch);}}}
                    if(bank.filename.empty())throw std::invalid_argument("DLS 參照缺少檔名");
                    if(std::none_of(seq.segment.banks.begin(),seq.segment.banks.end(),[&](const auto& x){return x.id==bank.id && x.filename==bank.filename;}))seq.segment.banks.push_back(std::move(bank));
                }else if(form=="trkl" || form=="DMTK" || form=="DMBT" || form=="lbdl" || form=="lbnd" || form=="DMBD" || form=="lbil" || form=="lbin")walk(b.subspan(4),band_time,depth+1);
                else throw std::invalid_argument("未支援的 DirectMusic 樂軌："+std::string(form));
            }else if(c.id=="segh"){
                if(header || b.size()<24)throw std::invalid_argument("DirectMusic 樂段標頭無效");header=true;
                auto& s=seq.segment;s.repeats=static_cast<std::uint32_t>(number(b,0));seq.music_length=static_cast<std::uint32_t>(number(b,4));s.start=static_cast<std::uint32_t>(number(b,8));s.loop_start=static_cast<std::uint32_t>(number(b,12));s.loop_end=static_cast<std::uint32_t>(number(b,16));s.resolution=static_cast<std::uint32_t>(number(b,20));
                if(!seq.music_length || seq.music_length>INT32_MAX || s.start>=seq.music_length || s.loop_end>seq.music_length || (s.loop_end && s.loop_start>=s.loop_end))throw std::invalid_argument("DirectMusic 樂段時間範圍無效");
                if(b.size()>=40 && number(b,32))throw std::invalid_argument("尚未支援參考時鐘樂段");
            }else if(c.id=="seqt")walk(b,band_time,depth+1);
            else if(c.id=="evtl")records(b,17,[&](Bytes r){
                const auto at=std::int64_t{time(r)}+static_cast<std::int16_t>(number(r,12,2));const auto duration=time(r,4);const auto status=static_cast<std::uint8_t>((number(r,14,1)&0xf0)|channel(r,8));const auto a=static_cast<std::uint8_t>(number(r,15,1)),v=static_cast<std::uint8_t>(number(r,16,1));
                if(duration<0)throw std::invalid_argument("DirectMusic 音符時長無效");
                if((status&0xf0)==0x90 && v){if(at+duration<=0)return;emit(at,2,status,a,v);emit(at+duration,1,static_cast<std::uint8_t>(0x80|(status&15)),a,0);}
                else emit(at,1,status,a,v);
            });
            else if(c.id=="curl")records(b,28,[&](Bytes r){
                Curve v;v.at=std::int64_t{time(r)}+static_cast<std::int16_t>(number(r,16,2));v.duration=time(r,4);v.reset_duration=time(r,8);v.channel=channel(r,12);v.from=static_cast<std::int16_t>(number(r,18,2));v.to=static_cast<std::int16_t>(number(r,20,2));v.reset=static_cast<std::int16_t>(number(r,22,2));v.type=static_cast<unsigned>(number(r,24,1));v.shape=static_cast<unsigned>(number(r,25,1));v.cc=static_cast<unsigned>(number(r,26,1));v.flags=static_cast<unsigned>(number(r,27,1));if(r.size()>=30)v.parameter=static_cast<unsigned>(number(r,28,2));
                if(v.duration<0 || v.reset_duration<0 || v.type<3 || v.type>8 || v.shape>4 || v.cc>127 || (v.flags&~3U))throw std::invalid_argument("DirectMusic 控制曲線無效");curves.push_back(v);
            });
            else if(c.id=="tetr")records(b,16,[&](Bytes r){const auto at=time(r);const auto bpm=std::bit_cast<double>(number(r,8,8));if(at<0 || !std::isfinite(bpm) || bpm<1 || bpm>1000)throw std::invalid_argument("DirectMusic 節拍速度無效");seq.tempos.push_back({static_cast<std::uint32_t>(at),0,bpm});});
            else if(c.id=="tims")records(b,8,[&](Bytes r){const auto at=time(r);const auto beats=number(r,4,1),beat=number(r,5,1),grids=number(r,6,2);if(at<0 || !beats || !beat || (beat&(beat-1)) || grids>3072/beat)throw std::invalid_argument("DirectMusic 拍號無效");seq.signatures.push_back({static_cast<std::uint32_t>(at),static_cast<std::uint8_t>(beats),static_cast<std::uint8_t>(beat),static_cast<std::uint16_t>(grids?grids:4)});});
            else if(c.id=="syex"){
                for(std::size_t at=0;at<b.size();){const auto when=time(b,at);channel(b,static_cast<unsigned>(at+4));const auto length=number(b,at+8);at+=12;if(length<2 || length>b.size()-at)throw std::invalid_argument("DirectMusic SysEx 長度無效");const auto msg=b.subspan(at,static_cast<std::size_t>(length));if(msg.front()!=0xf0 || msg.back()!=0xf7 || std::any_of(msg.begin()+1,msg.end()-1,[](auto x){return x>=128;}))throw std::invalid_argument("DirectMusic SysEx 無效");events.push_back({std::max<std::int64_t>(0,when),0,{0,0xf0,0,0,{msg.begin(),msg.end()}}});at+=static_cast<std::size_t>(length);}
            }else if(c.id=="bdih")band_time=time(b);
            else if(c.id=="bd2h")band_time=time(b,4);
            else if(c.id=="bins"){
                const auto ch=channel(b,24);const auto flags=number(b,28),patch=number(b,(flags&8)?4:0);const auto cc=static_cast<std::uint8_t>(0xb0|ch);
                if(flags&2){emit(band_time,0,cc,0,static_cast<std::uint8_t>((patch>>16)&127));emit(band_time,0,cc,32,static_cast<std::uint8_t>((patch>>8)&127));}
                if(flags&(1|8))emit(band_time,0,static_cast<std::uint8_t>(0xc0|ch),static_cast<std::uint8_t>(patch&127),0);
                if(flags&0x20)emit(band_time,0,cc,10,static_cast<std::uint8_t>(number(b,32,1)));
                if(flags&0x40)emit(band_time,0,cc,7,static_cast<std::uint8_t>(number(b,33,1)));
                if(flags&0x80){const auto transpose=static_cast<std::int16_t>(number(b,34,2));if(transpose< -64 || transpose>63)throw std::invalid_argument("DirectMusic 移調超出範圍");if(transpose){emit(band_time,0,cc,101,0);emit(band_time,0,cc,100,2);emit(band_time,0,cc,6,static_cast<std::uint8_t>(transpose+64));emit(band_time,0,cc,101,127);emit(band_time,0,cc,100,127);}}
                if(flags&0x2000){const auto range=number(b,40,2);if(range>127)throw std::invalid_argument("DirectMusic 音高滑輪範圍無效");emit(band_time,0,cc,101,0);emit(band_time,0,cc,100,0);emit(band_time,0,cc,6,static_cast<std::uint8_t>(range));emit(band_time,0,cc,101,127);emit(band_time,0,cc,100,127);}
            }else if(c.id!="trkh" && c.id!="trkx" && c.id!="guid" && c.id!="vers" && c.id!="bdth" && c.id!="bdih" && c.id!="sgdh")throw std::invalid_argument("未支援的 DirectMusic 區塊："+std::string(c.id));
        }
    };
    walk(roots[0].bytes.subspan(4),0,0);if(!header)throw std::invalid_argument("DirectMusic 樂段缺少標頭");
    std::stable_sort(seq.tempos.begin(),seq.tempos.end(),[](const auto& a,const auto& b){return a.music<b.music;});
    for(std::size_t i=1;i<seq.tempos.size();++i){const auto& prev=seq.tempos[i-1];seq.tempos[i].frame=prev.frame+static_cast<std::uint64_t>(double(seq.tempos[i].music-prev.music)*48000*60/(768*prev.bpm));}
    std::stable_sort(seq.signatures.begin(),seq.signatures.end(),[](const auto& a,const auto& b){return a.music<b.music;});
    seq.frames=seq.frame_at(seq.music_length);if(!seq.frames || seq.frames>48000*600ULL)throw std::invalid_argument("DirectMusic 樂段超過十分鐘");
    std::stable_sort(curves.begin(),curves.end(),[](const auto& a,const auto& b){return a.at<b.at;});
    for(const auto& c:curves){
        const auto max=c.type==3 || c.type>=7?16383:127;
        if(c.from<0 || c.from>max || c.to<0 || c.to>max || c.reset<0 || c.reset>max)throw std::invalid_argument("DirectMusic 控制值超出範圍");
        const auto write=[&](std::int64_t at,int value){const auto ch=static_cast<std::uint8_t>(c.channel),cc=static_cast<std::uint8_t>(0xb0|ch);if(c.type==3)emit(at,1,static_cast<std::uint8_t>(0xe0|ch),value&127,(value>>7)&127);else if(c.type==4)emit(at,1,cc,static_cast<std::uint8_t>(c.cc),static_cast<std::uint8_t>(value));else if(c.type==5)emit(at,1,static_cast<std::uint8_t>(0xd0|ch),static_cast<std::uint8_t>(value),0);else if(c.type==6)emit(at,1,static_cast<std::uint8_t>(0xa0|ch),static_cast<std::uint8_t>(c.cc),static_cast<std::uint8_t>(value));else {emit(at,1,cc,c.type==7?101:99,(c.parameter>>7)&127);emit(at,1,cc,c.type==7?100:98,c.parameter&127);emit(at,1,cc,6,(value>>7)&127);emit(at,1,cc,38,value&127);}};
        if(c.flags&2)throw std::invalid_argument("尚未支援由即時控制值開始的 DirectMusic 曲線");
        if(c.shape==1 || !c.duration || c.from==c.to)write(c.at,c.to);
        else {int previous=-1;for(std::int64_t t=0;;t=std::min(c.duration,t+16)){double x=double(t)/double(c.duration);if(c.shape==2)x=x*x;else if(c.shape==3)x=std::sqrt(x);else if(c.shape==4)x=(1-std::cos(std::numbers::pi*x))*.5;const auto value=static_cast<int>(std::lround(c.from+(c.to-c.from)*x));if(value!=previous){write(c.at+t,value);previous=value;}if(t==c.duration)break;}}
        if(c.flags&1)write(c.at+c.reset_duration,c.reset);
    }
    std::stable_sort(events.begin(),events.end(),[](const auto& a,const auto& b){return a.tick==b.tick?a.priority<b.priority:a.tick<b.tick;});
    for(auto& e:events)if(e.tick<=seq.music_length){e.event.frame=seq.frame_at(static_cast<std::uint32_t>(e.tick));seq.events.push_back(std::move(e.event));}
    return seq;
}
}
