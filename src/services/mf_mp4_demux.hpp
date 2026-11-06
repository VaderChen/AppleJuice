#pragma once
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace x64arm::mf {
// ISO BMFF 解多工：只解析容器與取樣表，時間一律換算為 100 ns；解碼與客體 ABI 留在共用流程。
struct Mp4Sample {std::uint64_t offset{};std::uint32_t size{},duration{};std::int64_t time{};bool key{};};
struct Mp4Track {
    std::uint32_t id{},codec{},timescale{};
    bool video{},audio{};
    std::uint32_t width{},height{},par_num{1},par_den{1},rate_num{},rate_den{};
    std::vector<std::uint8_t> avcc,audio_config;
    std::uint32_t channels{},sample_rate{},bits{16},object_type{};
    std::int64_t duration{};
    std::uint64_t total_bytes{};
    std::vector<Mp4Sample> samples;         // 解碼順序。
    std::vector<std::int64_t> presentation; // 排序後的顯示時間，供重排序與跳轉。
};
struct Mp4Movie {std::vector<Mp4Track> tracks;std::int64_t duration{};std::uint64_t file_size{};};
constexpr std::uint32_t fourcc(char a,char b,char c,char d){return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a))<<24)|(static_cast<std::uint32_t>(static_cast<std::uint8_t>(b))<<16)|(static_cast<std::uint32_t>(static_cast<std::uint8_t>(c))<<8)|static_cast<std::uint8_t>(d);}
namespace mp4_detail {
[[noreturn]] inline void bad(const char* what){throw std::invalid_argument(what);}
inline std::uint64_t be(std::span<const std::uint8_t> bytes,std::size_t at,unsigned width){
    if(at>bytes.size() || width>bytes.size()-at)bad("MP4 欄位截斷");
    std::uint64_t value=0;for(unsigned n=0;n<width;++n)value=(value<<8)|bytes[at+n];return value;
}
template<class F> void each_box(std::span<const std::uint8_t> bytes,F&& visit){
    for(std::size_t at=0;bytes.size()-at>=8;){
        std::uint64_t size=be(bytes,at,4);const auto type=static_cast<std::uint32_t>(be(bytes,at+4,4));std::size_t header=8;
        if(size==1){size=be(bytes,at+8,8);header=16;}else if(!size)size=bytes.size()-at;
        if(size<header || size>bytes.size()-at)bad("MP4 box 大小無效");
        visit(type,bytes.subspan(at+header,static_cast<std::size_t>(size)-header));at+=static_cast<std::size_t>(size);
    }
}
// MPEG-4 descriptor 的可變長度欄位。
inline std::size_t descriptor(std::span<const std::uint8_t> bytes,std::size_t& at,std::uint8_t expected){
    if(at>=bytes.size() || bytes[at]!=expected)return SIZE_MAX;++at;std::size_t size=0;
    for(unsigned n=0;n<4;++n){if(at>=bytes.size())bad("MP4 descriptor 截斷");const auto part=bytes[at++];size=(size<<7)|(part&0x7fU);if(!(part&0x80U))break;}
    if(size>bytes.size()-at)bad("MP4 descriptor 長度無效");return size;
}
inline void parse_esds(std::span<const std::uint8_t> body,Mp4Track& track){
    std::size_t at=4;if(descriptor(body,at,3)==SIZE_MAX)bad("esds 缺少 ES descriptor");
    if(body.size()-at<3)bad("esds 截斷");const auto flags=body[at+2];at+=3;
    if(flags&0x80U)at+=2;if(flags&0x40U){if(at>=body.size())bad("esds 截斷");at+=1U+body[at];}if(flags&0x20U)at+=2;
    if(at>=body.size() || descriptor(body,at,4)==SIZE_MAX)bad("esds 缺少解碼設定");
    if(body.size()-at<13)bad("esds 解碼設定截斷");track.object_type=body[at];at+=13;
    if(at<body.size()){const auto size=descriptor(body,at,5);if(size!=SIZE_MAX)track.audio_config.assign(body.begin()+static_cast<std::ptrdiff_t>(at),body.begin()+static_cast<std::ptrdiff_t>(at+size));}
}
inline void parse_sample_entry(std::uint32_t type,std::span<const std::uint8_t> entry,Mp4Track& track){
    track.codec=type;
    if(track.video){
        if(entry.size()<78)bad("MP4 視訊取樣項截斷");track.width=static_cast<std::uint32_t>(be(entry,24,2));track.height=static_cast<std::uint32_t>(be(entry,26,2));
        each_box(entry.subspan(78),[&](std::uint32_t child,std::span<const std::uint8_t> body){
            if(child==fourcc('a','v','c','C'))track.avcc.assign(body.begin(),body.end());
            else if(child==fourcc('p','a','s','p') && body.size()>=8){const auto num=be(body,0,4),den=be(body,4,4);if(num && den){track.par_num=static_cast<std::uint32_t>(num);track.par_den=static_cast<std::uint32_t>(den);}}
        });
    }else if(track.audio){
        if(entry.size()<28)bad("MP4 音訊取樣項截斷");const auto version=be(entry,8,2);std::size_t children=28;
        track.channels=static_cast<std::uint32_t>(be(entry,16,2));track.bits=static_cast<std::uint32_t>(be(entry,18,2));track.sample_rate=static_cast<std::uint32_t>(be(entry,24,4)>>16);
        if(version==1)children+=16;else if(version==2)children+=36;
        if(children>entry.size())bad("MP4 音訊取樣項截斷");
        each_box(entry.subspan(children),[&](std::uint32_t child,std::span<const std::uint8_t> body){if(child==fourcc('e','s','d','s'))parse_esds(body,track);});
        // AudioSpecificConfig 比取樣項可靠；取樣項常固定寫 16-bit／雙聲道。
        if(track.audio_config.size()>=2 && (track.audio_config[0]>>3)!=31){
            constexpr std::uint32_t rates[]{96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350},layouts[]{0,1,2,3,4,5,6,8};
            const auto index=((track.audio_config[0]&7U)<<1)|(track.audio_config[1]>>7),layout=(track.audio_config[1]>>3)&15U;
            if(index<std::size(rates))track.sample_rate=rates[index];if(layout>=1 && layout<std::size(layouts))track.channels=layouts[layout];
        }
    }
}
struct SampleTables {std::span<const std::uint8_t> stts,ctts,stsc,stsz,stz2,stco,co64,stss;};
inline void build_samples(const SampleTables& t,Mp4Track& track,std::uint64_t file_size,std::int64_t media_offset,std::int64_t start_delay){
    if(t.stsz.size()<12 && t.stz2.size()<12)bad("MP4 缺少 stsz");if(t.stts.size()<8 || t.stsc.size()<8 || (t.stco.size()<8 && t.co64.size()<8))bad("MP4 取樣表不完整");
    const auto& sizes=t.stsz.empty()?t.stz2:t.stsz;const auto count=be(sizes,8,4);if(count>20000000)bad("MP4 取樣數過多");
    std::uint32_t fixed=0;unsigned field=32;if(!t.stsz.empty())fixed=static_cast<std::uint32_t>(be(sizes,4,4));else {field=sizes[7];if(field!=4 && field!=8 && field!=16)bad("MP4 stz2 欄位無效");}
    const auto sample_size=[&](std::uint64_t n)->std::uint32_t{
        if(fixed)return fixed;if(field==32)return static_cast<std::uint32_t>(be(sizes,12+n*4,4));if(field==16)return static_cast<std::uint32_t>(be(sizes,12+n*2,2));if(field==8)return static_cast<std::uint32_t>(be(sizes,12+n,1));
        const auto byte=be(sizes,12+n/2,1);return static_cast<std::uint32_t>(n&1?byte&15:byte>>4);
    };
    const bool wide=t.stco.empty();const auto& chunks=wide?t.co64:t.stco;const auto chunk_count=be(chunks,4,4);
    const auto chunk_offset=[&](std::uint64_t n){return wide?be(chunks,8+n*8,8):be(chunks,8+n*4,4);};
    track.samples.reserve(static_cast<std::size_t>(count));
    const auto stsc_entries=be(t.stsc,4,4);std::uint64_t stsc_at=0,produced=0;
    for(std::uint64_t chunk=0;chunk<chunk_count && produced<count;++chunk){
        while(stsc_at+1<stsc_entries && be(t.stsc,8+(stsc_at+1)*12,4)<=chunk+1)++stsc_at;
        if(!stsc_entries)bad("MP4 stsc 為空");
        const auto per_chunk=be(t.stsc,8+stsc_at*12+4,4);auto offset=chunk_offset(chunk);
        for(std::uint64_t n=0;n<per_chunk && produced<count;++n,++produced){
            const auto size=sample_size(produced);
            if(offset>file_size || size>file_size-offset)bad("MP4 取樣超出檔案範圍");
            track.samples.push_back({offset,size,0,0,t.stss.empty()});offset+=size;track.total_bytes+=size;
        }
    }
    if(produced!=count)bad("MP4 stsc 與 stsz 數量不符");
    if(!track.timescale)bad("MP4 timescale 為零");
    const auto scaled=[&](__int128 value){return static_cast<std::int64_t>(value*10000000/track.timescale);};
    const auto stts_entries=be(t.stts,4,4);__int128 dts=0;std::size_t index=0;std::uint64_t first_delta=0;
    std::size_t ctts_index=0;std::uint64_t ctts_left=0;std::int64_t ctts_value=0;const auto ctts_entries=t.ctts.size()>=8?be(t.ctts,4,4):0;
    for(std::uint64_t entry=0;entry<stts_entries && index<track.samples.size();++entry){
        const auto repeat=be(t.stts,8+entry*8,4),delta=be(t.stts,12+entry*8,4);if(!entry)first_delta=delta;
        for(std::uint64_t n=0;n<repeat && index<track.samples.size();++n,++index){
            while(!ctts_left && ctts_index<ctts_entries){ctts_left=be(t.ctts,8+ctts_index*8,4);ctts_value=static_cast<std::int32_t>(be(t.ctts,12+ctts_index*8,4));++ctts_index;}
            const auto composition=dts+(ctts_left?ctts_value:0);if(ctts_left)--ctts_left;
            auto& sample=track.samples[index];sample.time=scaled(composition-media_offset)+start_delay;sample.duration=static_cast<std::uint32_t>(std::clamp<std::int64_t>(scaled(delta),0,UINT32_MAX));dts+=delta;
        }
    }
    if(index!=track.samples.size())bad("MP4 stts 與 stsz 數量不符");
    if(track.samples.size()>1 && !track.samples.back().duration)track.samples.back().duration=track.samples[track.samples.size()-2].duration;
    if(!t.stss.empty()){const auto entries=be(t.stss,4,4);for(std::uint64_t n=0;n<entries;++n){const auto number=be(t.stss,8+n*4,4);if(number && number<=track.samples.size())track.samples[static_cast<std::size_t>(number-1)].key=true;}}
    track.presentation.reserve(track.samples.size());std::int64_t end=0;
    for(const auto& sample:track.samples){track.presentation.push_back(sample.time);end=std::max(end,sample.time+static_cast<std::int64_t>(sample.duration));}
    std::sort(track.presentation.begin(),track.presentation.end());track.duration=end;
    if(track.video && first_delta){const auto divisor=std::gcd<std::uint64_t,std::uint64_t>(track.timescale,first_delta);track.rate_num=static_cast<std::uint32_t>(track.timescale/divisor);track.rate_den=static_cast<std::uint32_t>(first_delta/divisor);}
}
inline std::optional<Mp4Track> parse_track(std::span<const std::uint8_t> trak,std::uint32_t movie_scale,std::uint64_t file_size){
    Mp4Track track;SampleTables tables;std::int64_t media_time=0,delay_units=0;std::span<const std::uint8_t> entries;std::uint32_t handler=0;
    each_box(trak,[&](std::uint32_t type,std::span<const std::uint8_t> body){
        if(type==fourcc('t','k','h','d')){if(body.empty())bad("tkhd 截斷");track.id=static_cast<std::uint32_t>(be(body,body[0]?20:12,4));}
        else if(type==fourcc('e','d','t','s'))each_box(body,[&](std::uint32_t child,std::span<const std::uint8_t> edit){
            if(child!=fourcc('e','l','s','t') || edit.size()<8)return;const bool large=edit[0]!=0;const auto count=be(edit,4,4);const unsigned step=large?20:12;
            for(std::uint64_t n=0;n<count;++n){
                const auto at=8+n*step;const auto duration=be(edit,at,large?8:4);
                const auto time=large?static_cast<std::int64_t>(be(edit,at+8,8)):static_cast<std::int32_t>(be(edit,at+4,4));
                if(time<0){delay_units+=static_cast<std::int64_t>(duration);continue;}media_time=time;break;
            }
        });
        else if(type==fourcc('m','d','i','a'))each_box(body,[&](std::uint32_t child,std::span<const std::uint8_t> media){
            if(child==fourcc('m','d','h','d')){if(media.empty())bad("mdhd 截斷");track.timescale=static_cast<std::uint32_t>(be(media,media[0]?20:12,4));}
            else if(child==fourcc('h','d','l','r'))handler=static_cast<std::uint32_t>(be(media,8,4));
            else if(child==fourcc('m','i','n','f'))each_box(media,[&](std::uint32_t leaf,std::span<const std::uint8_t> info){
                if(leaf!=fourcc('s','t','b','l'))return;
                each_box(info,[&](std::uint32_t table,std::span<const std::uint8_t> data){
                    if(table==fourcc('s','t','s','d'))entries=data;else if(table==fourcc('s','t','t','s'))tables.stts=data;else if(table==fourcc('c','t','t','s'))tables.ctts=data;
                    else if(table==fourcc('s','t','s','c'))tables.stsc=data;else if(table==fourcc('s','t','s','z'))tables.stsz=data;else if(table==fourcc('s','t','z','2'))tables.stz2=data;
                    else if(table==fourcc('s','t','c','o'))tables.stco=data;else if(table==fourcc('c','o','6','4'))tables.co64=data;else if(table==fourcc('s','t','s','s'))tables.stss=data;
                });
            });
        });
    });
    track.video=handler==fourcc('v','i','d','e');track.audio=handler==fourcc('s','o','u','n');
    if((!track.video && !track.audio) || entries.size()<16)return std::nullopt;
    bool first=true;each_box(entries.subspan(8),[&](std::uint32_t type,std::span<const std::uint8_t> entry){if(first){first=false;parse_sample_entry(type,entry,track);}});
    const auto delay=movie_scale?static_cast<std::int64_t>(static_cast<__int128>(delay_units)*10000000/movie_scale):0;
    build_samples(tables,track,file_size,media_time,delay);
    if(track.samples.empty())return std::nullopt;
    return track;
}
}
// 頭 8 bytes 足以與 RIFF／WAVE 區分；檔尾 moov 的檔案仍以 ftyp、mdat 或填充 box 開頭。
inline bool mp4_signature(std::span<const std::uint8_t,8> head){
    const auto type=(std::uint32_t{head[4]}<<24)|(std::uint32_t{head[5]}<<16)|(std::uint32_t{head[6]}<<8)|head[7];
    return type==fourcc('f','t','y','p') || type==fourcc('m','o','o','v') || type==fourcc('m','d','a','t') || type==fourcc('w','i','d','e') || type==fourcc('f','r','e','e');
}
inline std::shared_ptr<const Mp4Movie> parse_mp4_moov(std::span<const std::uint8_t> moov,std::uint64_t file_size){
    using namespace mp4_detail;
    auto movie=std::make_shared<Mp4Movie>();movie->file_size=file_size;std::uint32_t scale=0;bool fragmented=false;
    each_box(moov,[&](std::uint32_t type,std::span<const std::uint8_t>body){
        if(type==fourcc('m','v','h','d')){if(body.empty())bad("mvhd 截斷");scale=static_cast<std::uint32_t>(be(body,body[0]?20:12,4));}
        else if(type==fourcc('m','v','e','x'))fragmented=true;
    });
    if(fragmented)bad("尚未支援 fragmented MP4");
    each_box(moov,[&](std::uint32_t type,std::span<const std::uint8_t>body){
        if(type!=fourcc('t','r','a','k'))return;
        if(auto track=parse_track(body,scale,file_size)){movie->duration=std::max(movie->duration,track->duration);movie->tracks.push_back(std::move(*track));}
    });
    if(movie->tracks.empty())bad("MP4 沒有可播放軌道");
    return movie;
}
// 逐一走訪頂層 box 找到 moov；同步與客體 byte stream 共用同一份流程。
template<class Read> service_callback::Task<std::shared_ptr<const Mp4Movie>> read_mp4_movie_async(std::uint64_t size,Read read){
    for(std::uint64_t at=0;size-at>=8;){
        std::array<std::uint8_t,16> head{};co_await read(at,std::span<std::uint8_t>(head).first(8));
        auto box=mp4_detail::be(head,0,4);const auto type=static_cast<std::uint32_t>(mp4_detail::be(head,4,4));std::uint64_t header=8;
        if(box==1){if(size-at<16)mp4_detail::bad("MP4 box 截斷");co_await read(at+8,std::span<std::uint8_t>(head).subspan(8,8));box=mp4_detail::be(head,8,8);header=16;}
        else if(!box)box=size-at;
        if(box<header || box>size-at)mp4_detail::bad("MP4 頂層 box 大小無效");
        if(type==fourcc('m','o','o','v')){
            if(box-header>256U*1024U*1024U)mp4_detail::bad("MP4 moov 過大");
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(box-header));if(!bytes.empty())co_await read(at+header,std::span<std::uint8_t>(bytes));
            co_return parse_mp4_moov(bytes,size);
        }
        if(type==fourcc('m','o','o','f'))mp4_detail::bad("尚未支援 fragmented MP4");
        at+=box;
    }
    mp4_detail::bad("MP4 缺少 moov");
}
struct Mp4ReadReady {
    bool await_ready()const noexcept{return true;}
    void await_suspend(std::coroutine_handle<>)const noexcept{}
    void await_resume()const noexcept{}
};
template<class Read> std::shared_ptr<const Mp4Movie> read_mp4_movie(std::uint64_t size,Read read){
    auto task=read_mp4_movie_async(size,[&](std::uint64_t at,std::span<std::uint8_t> bytes){read(at,bytes);return Mp4ReadReady{};});
    task.address().resume();return task.take();
}
}
