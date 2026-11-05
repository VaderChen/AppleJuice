#pragma once
#include "mp4_video.hpp"
#include <algorithm>
#include <bit>
#include <optional>

namespace x64arm::mp4 {
struct MediaSample:Sample {unsigned description{1};};
struct Description {
    std::uint32_t codec{};unsigned width{},height{},channels{},rate{},bits{};
    std::vector<std::uint8_t> bytes,configuration;
};
struct Track {
    unsigned id{},timescale{};bool video{},audio{};std::uint64_t duration{};
    std::vector<Description> descriptions;std::vector<MediaSample> samples;
};
namespace reader_detail {
inline void require(bool value){if(!value)throw std::invalid_argument("MP4 容器、樣本表或範圍無效");}
constexpr std::uint32_t code(std::string_view text){return (std::uint32_t{static_cast<unsigned char>(text[0])}<<24)|(std::uint32_t{static_cast<unsigned char>(text[1])}<<16)|(std::uint32_t{static_cast<unsigned char>(text[2])}<<8)|static_cast<unsigned char>(text[3]);}
inline std::uint64_t number(std::span<const std::uint8_t> bytes,std::size_t at,unsigned width=4){require(at<=bytes.size() && width<=bytes.size()-at);std::uint64_t value=0;for(unsigned i=0;i<width;++i)value=(value<<8)|bytes[at+i];return value;}
struct Box {std::uint32_t type{};std::span<const std::uint8_t> data,whole;};
inline std::vector<Box> boxes(std::span<const std::uint8_t> bytes){
    std::vector<Box> result;
    while(!bytes.empty()){
        require(bytes.size()>=8);auto size=number(bytes,0);const auto type=static_cast<std::uint32_t>(number(bytes,4));unsigned header=8;
        if(size==1){size=number(bytes,8,8);header=16;}else if(!size)size=bytes.size();
        require(size>=header && size<=bytes.size());result.push_back({type,bytes.subspan(header,static_cast<std::size_t>(size)-header),bytes.first(static_cast<std::size_t>(size))});
        bytes=bytes.subspan(static_cast<std::size_t>(size));require(result.size()<=1000000);
    }return result;
}
inline std::optional<Box> find(std::span<const std::uint8_t> parent,std::string_view type){
    std::optional<Box> result;for(const auto& box:boxes(parent))if(box.type==code(type)){require(!result);result=box;}return result;
}
inline Box child(std::span<const std::uint8_t> parent,std::string_view type){const auto result=find(parent,type);require(result.has_value());return *result;}
inline std::size_t entries(std::span<const std::uint8_t> data,unsigned stride,unsigned start=8){const auto count=number(data,4);require(start<=data.size() && count<=(data.size()-start)/stride);return static_cast<std::size_t>(count);}
inline Description description(Box box,bool video,bool audio){
    Description result;result.codec=box.type;result.bytes.assign(box.whole.begin(),box.whole.end());require(number(box.data,6,2)==1); // 不載入外部資料引用。
    std::size_t extensions=box.data.size();
    if(video){require(box.data.size()>=78);result.width=static_cast<unsigned>(number(box.data,24,2));result.height=static_cast<unsigned>(number(box.data,26,2));require(result.width && result.height);extensions=78;}
    if(audio){require(box.data.size()>=28);const auto version=number(box.data,8,2);require(version<=1);result.channels=static_cast<unsigned>(number(box.data,16,2));result.bits=static_cast<unsigned>(number(box.data,18,2));result.rate=static_cast<unsigned>(number(box.data,24)>>16);extensions=version?44:28;require(extensions<=box.data.size() && result.channels && result.rate);}
    if(const auto avcc=find(box.data.subspan(extensions),"avcC"))result.configuration.assign(avcc->data.begin(),avcc->data.end());
    else if(const auto esds=find(box.data.subspan(extensions),"esds"))result.configuration.assign(esds->data.begin(),esds->data.end());
    return result;
}
}
// 僅保留 moov 與樣本索引；mdat payload 由來源經已授權檔案按需讀取。
// 不重寫或解碼樣本；壓縮格式仍交由共用 codec 後台處理。
template<class Read> std::vector<Track> read_movie(std::uint64_t size,Read read){
    using namespace reader_detail;require(size<=INT64_MAX);
    std::vector<std::uint8_t> metadata;std::vector<std::pair<std::uint64_t,std::uint64_t>> media;bool have_movie=false;
    for(std::uint64_t at=0;at<size;){
        require(size-at>=8);std::array<std::uint8_t,16> header{};read(at,std::span(header).first(8));auto length=number(header,0);const auto type=number(header,4);unsigned header_bytes=8;
        if(length==1){require(size-at>=16);read(at+8,std::span(header).subspan(8));length=number(header,8,8);header_bytes=16;}else if(!length)length=size-at;
        require(length>=header_bytes && length<=size-at);
        if(type==code("moov")){require(!have_movie && length-header_bytes<=64U*1024U*1024U);have_movie=true;metadata.resize(static_cast<std::size_t>(length-header_bytes));read(at+header_bytes,std::span(metadata));}
        else if(type==code("mdat"))media.emplace_back(at+header_bytes,at+length);
        else require(type!=code("moof") && type!=code("cmov"));
        at+=length;
    }
    require(!metadata.empty() && !media.empty());const auto mvhd=child(metadata,"mvhd").data;const auto movie_version=number(mvhd,0,1);require(movie_version<=1);const auto movie_scale=number(mvhd,movie_version?20:12);require(movie_scale);
    std::vector<Track> tracks;
    for(const auto& box:boxes(metadata))if(box.type==code("trak")){
        const auto mdia=child(box.data,"mdia").data,handler=child(mdia,"hdlr").data;const auto kind=number(handler,8);
        if(kind!=code("vide") && kind!=code("soun"))continue;
        Track track;track.video=kind==code("vide");track.audio=kind==code("soun");
        const auto tkhd=child(box.data,"tkhd").data;const auto track_version=number(tkhd,0,1);require(track_version<=1);track.id=static_cast<unsigned>(number(tkhd,track_version?20:12));require(track.id && std::none_of(tracks.begin(),tracks.end(),[&](const auto& t){return t.id==track.id;}));
        const auto mdhd=child(mdia,"mdhd").data;const auto version=number(mdhd,0,1);require(version<=1);track.timescale=static_cast<unsigned>(number(mdhd,version?20:12));track.duration=number(mdhd,version?24:16,version?8:4);require(track.timescale && track.timescale<=INT32_MAX);
        const auto minf=child(mdia,"minf").data,stbl=child(minf,"stbl").data,stsd=child(stbl,"stsd").data;
        require(number(stsd,0)==0);const auto descriptions=boxes(stsd.subspan(8));require(descriptions.size()==number(stsd,4) && !descriptions.empty() && descriptions.size()<=256);
        for(const auto& entry:descriptions)track.descriptions.push_back(description(entry,track.video,track.audio));
        const auto dref=child(child(minf,"dinf").data,"dref").data;require(number(dref,4)>=1);const auto references=boxes(dref.subspan(8));require(!references.empty() && references[0].type==code("url ") && (number(references[0].data,0)&1));
        const auto stsz=child(stbl,"stsz").data;require(number(stsz,0)==0);const auto constant=number(stsz,4),count=number(stsz,8);require(count && count<=10000000);if(!constant)require(count<=(stsz.size()-12)/4);track.samples.resize(static_cast<std::size_t>(count));
        for(std::size_t n=0;n<count;++n)track.samples[n].size=static_cast<std::uint32_t>(constant?constant:number(stsz,12+n*4));
        const auto stts=child(stbl,"stts").data;require(number(stts,0)==0);std::size_t sample=0;std::uint64_t ticks=0;
        for(std::size_t n=0,rows=entries(stts,8);n<rows;++n){const auto run=number(stts,8+n*8),duration=number(stts,12+n*8);require(run && run<=count-sample && duration);
            for(std::size_t j=0;j<run;++j){require(ticks<=INT64_MAX-duration);auto& s=track.samples[sample++];s.dts=s.pts=static_cast<std::int64_t>(ticks);s.duration=static_cast<std::uint32_t>(duration);ticks+=duration;}}
        require(sample==count);track.duration=ticks;
        if(const auto found=find(stbl,"ctts")){
            const auto data=found->data;const auto v=number(data,0,1);require(v<=1 && (number(data,0)&0xffffffU)==0);sample=0;
            for(std::size_t n=0,rows=entries(data,8);n<rows;++n){const auto run=number(data,8+n*8),raw=number(data,12+n*8);const std::int64_t offset=v?static_cast<std::int32_t>(raw):static_cast<std::int64_t>(raw);require(run && run<=count-sample);
                for(std::size_t j=0;j<run;++j){auto& s=track.samples[sample++];require(offset<=0 || s.dts<=INT64_MAX-offset);s.pts=s.dts+offset;}}
            require(sample==count);
        }
        const auto co64=find(stbl,"co64"),stco=find(stbl,"stco");require(bool(co64)!=bool(stco));const auto offsets=(co64?co64:stco)->data;require(number(offsets,0)==0);const unsigned width=co64?8U:4U;const auto chunks=entries(offsets,width);require(chunks);
        const auto stsc=child(stbl,"stsc").data;require(number(stsc,0)==0);const auto groups=entries(stsc,12);require(groups && number(stsc,8)==1);sample=0;
        for(std::size_t group=0;group<groups;++group){const auto first=number(stsc,8+group*12),per=number(stsc,12+group*12),desc=number(stsc,16+group*12),end=group+1<groups?number(stsc,20+group*12):chunks+1;
            require(first && first<end && end<=chunks+1 && per && desc && desc<=track.descriptions.size());
            for(std::uint64_t chunk=first;chunk<end;++chunk){auto at=number(offsets,8+(chunk-1)*width,width);require(per<=count-sample);
                for(std::size_t n=0;n<per;++n){auto& s=track.samples[sample++];require(s.size && at<=size && s.size<=size-at && std::any_of(media.begin(),media.end(),[&](const auto& m){return at>=m.first && at<=m.second && s.size<=m.second-at;}));s.offset=at;s.description=static_cast<unsigned>(desc);at+=s.size;}}
        }require(sample==count);
        if(const auto sync=find(stbl,"stss")){require(number(sync->data,0)==0);std::uint64_t previous=0;for(std::size_t n=0,rows=entries(sync->data,4);n<rows;++n){const auto key=number(sync->data,8+n*4);require(key>previous && key<=count);track.samples[key-1].key=true;previous=key;}}
        else for(auto& s:track.samples)s.key=true;
        // 一般修剪與前置空白；不把非 1.0 播放速度誤當正常時間軸。
        if(const auto edts=find(box.data,"edts")){
            const auto edits=child(edts->data,"elst").data;const auto v=number(edits,0,1);require(v<=1);const unsigned stride=v?20U:12U,w=v?8U:4U;const auto rows=entries(edits,stride);std::uint64_t empty=0;bool content=false;std::int64_t shift=0;
            for(std::size_t n=0;n<rows;++n){const auto at=8+n*stride;const auto duration=number(edits,at,w),raw=number(edits,at+w,w);const auto time=v?std::bit_cast<std::int64_t>(raw):static_cast<std::int64_t>(static_cast<std::int32_t>(raw));require(number(edits,at+2*w)==0x10000);
                if(time==-1){require(!content && duration<=UINT64_MAX-empty);empty+=duration;}else {require(!content && time>=0);const auto prefix=static_cast<__uint128_t>(empty)*track.timescale/movie_scale;require(prefix<=INT64_MAX);shift=static_cast<std::int64_t>(prefix)-time;content=true;}}
            if(content)for(auto& s:track.samples){require((shift<=0 || s.pts<=INT64_MAX-shift) && (shift>=0 || s.pts>=INT64_MIN-shift));s.pts+=shift;require((shift<=0 || s.dts<=INT64_MAX-shift) && (shift>=0 || s.dts>=INT64_MIN-shift));s.dts+=shift;}
        }
        tracks.push_back(std::move(track));require(tracks.size()<=64);
    }
    require(!tracks.empty());return tracks;
}
}
