#pragma once

#include "pcm_wave_file.hpp"
#include "x64arm/readonly_files.hpp"
#include <algorithm>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace x64arm::cdda {

inline constexpr std::uint32_t sample_rate=44100,sectors_per_second=75;
inline constexpr std::uint32_t samples_per_sector=588,sector_bytes=2352,lead_in=150;

inline void read(HostFile& file,std::uint64_t at,std::span<std::uint8_t> out){
    while(!out.empty()){
        const auto result=file.read(out,at);
        if(result.error || !result.bytes || result.bytes>out.size())throw std::runtime_error("光碟映像讀取失敗");
        at+=result.bytes;
        out=out.subspan(result.bytes);
    }
}

inline std::string uppercase(std::string text){
    for(auto& value:text)if(value>='a' && value<='z')value=static_cast<char>(value-'a'+'A');
    return text;
}

inline std::string token(std::string_view& line){
    const auto first=line.find_first_not_of(" \t");
    if(first==std::string_view::npos){line={};return {};}
    line.remove_prefix(first);
    const bool quoted=line.front()=='"';
    if(quoted)line.remove_prefix(1);
    const auto end=quoted?line.find('"'):line.find_first_of(" \t");
    if(quoted && end==std::string_view::npos)throw std::invalid_argument("CUE 引號未結束");
    std::string value(line.substr(0,end));
    if(end==std::string_view::npos)line={};
    else line.remove_prefix(end+1);
    return value;
}

inline unsigned number(std::string_view text){
    unsigned value;
    const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
    if(result.ec!=std::errc{} || result.ptr!=text.data()+text.size())throw std::invalid_argument("CUE 數字欄位無效");
    return value;
}

inline std::uint32_t timecode(std::string_view value){
    const auto first=value.find(':'),second=first==value.npos?value.npos:value.find(':',first+1);
    if(second==value.npos)throw std::invalid_argument("CUE 時間碼無效");
    const auto minutes=number(value.substr(0,first)),seconds=number(value.substr(first+1,second-first-1));
    const auto frames=number(value.substr(second+1));
    if(minutes>=100 || seconds>=60 || frames>=sectors_per_second)throw std::invalid_argument("CUE 時間碼超出範圍");
    return (minutes*60+seconds)*sectors_per_second+frames;
}

class Disc {
public:
    struct Track {unsigned number{};std::uint32_t sector{};bool audio{};};
    static std::shared_ptr<const Disc> open(ReadOnlyFiles& files,const std::string& path){
        const auto cue=files.open_reader(path);
        if(cue.error || !cue.file)return {};
        if(!cue.metadata.size || cue.metadata.size>1024U*1024U)throw std::invalid_argument("CUE 檔案大小無效");
        std::vector<std::uint8_t> input(static_cast<std::size_t>(cue.metadata.size));
        read(*cue.file,0,input);
        if(std::find(input.begin(),input.end(),0)!=input.end())throw std::invalid_argument("CUE 含無效字元");
        auto disc=std::make_shared<Disc>();
        disc->parse(files,path,{reinterpret_cast<const char*>(input.data()),input.size()});
        return disc;
    }
    static bool cue_name(std::string_view name){
        return name.size()>=4 && uppercase(std::string(name.substr(name.size()-4)))==".CUE";
    }
    const std::vector<Track>& tracks()const{return tracks_;}
    std::uint32_t sectors()const{return sectors_;}
    const std::string& catalog()const{return catalog_;}
    const Track& track_at(std::uint32_t sector)const{
        const auto after=std::upper_bound(tracks_.begin(),tracks_.end(),sector,
            [](std::uint32_t at,const Track& track){return at<track.sector;});
        return after==tracks_.begin()?tracks_.front():*std::prev(after);
    }
    std::uint64_t identity()const{
        std::uint64_t value=14695981039346656037ULL;
        const auto add=[&](std::uint32_t field){
            for(unsigned shift=0;shift<32;shift+=8){value^=(field>>shift)&255U;value*=1099511628211ULL;}
        };
        for(const auto& track:tracks_){add(track.number);add(track.sector);add(track.audio?0U:4U);}
        add(sectors_);
        return value;
    }
    // PCM 索引以每聲道 frame 計，對應完整光碟時間軸。資料軌與虛擬間隔的音訊為靜音。
    std::vector<std::uint8_t> audio(std::uint64_t begin,std::size_t count)const{
        const auto total=std::uint64_t{sectors_}*samples_per_sector;
        if(begin>total || count>total-begin || count>65536)throw std::invalid_argument("CD 音訊範圍無效");
        std::vector<std::uint8_t> bytes(count*4);
        const auto end=begin+count;
        for(const auto& extent:extents_){
            const auto start=std::uint64_t{extent.begin}*samples_per_sector;
            const auto stop=std::uint64_t{extent.end}*samples_per_sector;
            if(start>=end)break;
            if(stop<=begin || !extent.audio)continue;
            const auto first=std::max(start,begin),last=std::min(stop,end);
            const auto& source=files_[extent.file];
            const auto at=source.offset+std::uint64_t{extent.source}*sector_bytes+(first-start)*4;
            auto out=std::span(bytes).subspan(static_cast<std::size_t>(first-begin)*4,static_cast<std::size_t>(last-first)*4);
            read(*source.file,at,out);
            if(source.big_endian)for(std::size_t i=0;i<out.size();i+=2)std::swap(out[i],out[i+1]);
        }
        return bytes;
    }
private:
    struct Source {
        std::shared_ptr<HostFile> file;
        std::uint64_t offset{},bytes{};
        unsigned size{};
        bool wave{},big_endian{};
    };
    struct Section {
        std::size_t file{};
        unsigned number{};
        std::optional<std::uint32_t> index0,index1;
        std::uint32_t pregap{},postgap{};
        bool audio{};
    };
    struct Extent {std::size_t file{};std::uint32_t begin{},end{},source{};bool audio{};};
    void parse(ReadOnlyFiles& fs,const std::string& path,std::string_view text){
        if(text.starts_with("\xef\xbb\xbf"))text.remove_prefix(3);
        std::vector<Section> sections;
        const auto slash=path.find_last_of("/\\");
        const auto folder=slash==std::string::npos?std::string{}:path.substr(0,slash+1);
        while(!text.empty()){
            const auto end=text.find('\n');
            auto line=text.substr(0,end);
            if(end==text.npos)text={};else text.remove_prefix(end+1);
            if(!line.empty() && line.back()=='\r')line.remove_suffix(1);
            const auto command=uppercase(token(line));
            if(command=="FILE"){
                const auto name=token(line),kind=uppercase(token(line));
                if(name.empty() || (kind!="WAVE" && kind!="BINARY" && kind!="MOTOROLA") || files_.size()>=99)
                    throw std::invalid_argument("CUE 檔案類型無效");
                const bool absolute=name.starts_with('/') || name.starts_with('\\') || (name.size()>1 && name[1]==':');
                const auto item=fs.open_reader(absolute?name:folder+name);
                if(item.error || !item.file)throw std::runtime_error("CUE 音軌檔案無法開啟");
                Source source{item.file,0,item.metadata.size,0,kind=="WAVE",kind=="MOTOROLA"};
                if(source.wave){
                    const auto wave=parse_pcm_wave_stream(source.bytes,[&](std::uint64_t at,std::span<std::uint8_t> bytes){read(*source.file,at,bytes);});
                    if(!wave || wave->format.channels!=2 || wave->format.rate!=sample_rate ||
                       wave->format.bits!=16 || wave->format.floating || wave->format.align!=4)
                        throw std::invalid_argument("CD WAVE 必須為 44.1 kHz 立體聲 PCM16");
                    source.offset=wave->offset;
                    source.bytes=wave->bytes;
                }
                files_.push_back(std::move(source));
            }else if(command=="TRACK"){
                const auto track=number(token(line));
                const auto kind=uppercase(token(line));
                if(files_.empty() || track<1 || track>99 || (!sections.empty() && track!=sections.back().number+1))
                    throw std::invalid_argument("CUE 音軌編號無效");
                const bool audio=kind=="AUDIO";
                const auto size=audio || kind=="MODE1/2352" || kind=="MODE2/2352"?2352U:
                    kind=="MODE1/2048" || kind=="MODE2/2048"?2048U:kind=="MODE2/2336"?2336U:0U;
                auto& file=files_.back();
                if(!size || (file.wave && !audio) || (file.size && file.size!=size))
                    throw std::invalid_argument("CUE 音軌磁區格式無效");
                file.size=size;
                sections.push_back({files_.size()-1,track,{},{},0,0,audio});
            }else if(command=="INDEX" || command=="PREGAP" || command=="POSTGAP"){
                if(sections.empty() || sections.back().file+1!=files_.size())throw std::invalid_argument("CUE 缺少音軌");
                auto& track=sections.back();
                if(command=="INDEX"){
                    const auto index=number(token(line));
                    const auto at=timecode(token(line));
                    if(index<=1){
                        auto& target=index?track.index1:track.index0;
                        if(target)throw std::invalid_argument("CUE 音軌索引重複");
                        target=at;
                    }
                }else{
                    auto& gap=command=="PREGAP"?track.pregap:track.postgap;
                    if(gap)throw std::invalid_argument("CUE 音軌間隔重複");
                    gap=timecode(token(line));
                }
            }else if(command=="CATALOG"){
                catalog_=token(line);
                if(catalog_.size()!=13 || !std::all_of(catalog_.begin(),catalog_.end(),[](char c){return c>='0' && c<='9';}))
                    throw std::invalid_argument("CUE 光碟目錄號無效");
            }
        }
        build_extents(sections);
    }
    void build_extents(const std::vector<Section>& sections){
        if(sections.empty() || std::any_of(files_.begin(),files_.end(),[](const Source& file){
            return !file.size || !file.bytes || file.bytes%file.size || file.bytes/file.size>=450000;
        }))throw std::invalid_argument("CUE 磁區資料不完整");
        for(std::size_t i=0;i<sections.size();++i){
            const auto& current=sections[i];
            if(!current.index1)throw std::invalid_argument("CUE 音軌缺少 INDEX 01");
            const auto begin=current.index0.value_or(*current.index1);
            const auto& file=files_[current.file];
            const auto file_sectors=static_cast<std::uint32_t>(file.bytes/file.size);
            auto end=file_sectors;
            if(i+1<sections.size() && sections[i+1].file==current.file){
                if(!sections[i+1].index1)throw std::invalid_argument("CUE 音軌缺少 INDEX 01");
                end=sections[i+1].index0.value_or(*sections[i+1].index1);
            }
            if(begin>*current.index1 || end<=*current.index1 || end>file_sectors)
                throw std::invalid_argument("CUE 音軌起訖無效");
            sectors_+=current.pregap;
            tracks_.push_back({current.number,sectors_+*current.index1-begin,current.audio});
            extents_.push_back({current.file,sectors_,sectors_+end-begin,begin,current.audio});
            sectors_+=end-begin+current.postgap;
            if(sectors_>=450000)throw std::invalid_argument("CUE 光碟長度超出範圍");
        }
    }
    std::vector<Source> files_;
    std::vector<Extent> extents_;
    std::vector<Track> tracks_;
    std::string catalog_;
    std::uint32_t sectors_{};
};

struct CueLookup {std::string path;bool ambiguous{};};

inline CueLookup volume_cue(ReadOnlyFiles& files,const std::string& root){
    CueLookup result;
    const auto entries=files.list(root,Disc::cue_name);
    if(entries.error)return result;
    for(const auto& entry:entries.entries){
        if(entry.metadata.attributes&0x10U)continue;
        if(!result.path.empty())return {{},true};
        result.path=root+entry.name;
    }
    return result;
}

}
