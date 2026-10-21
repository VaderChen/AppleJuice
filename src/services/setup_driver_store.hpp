#pragma once
#include "x64arm/readonly_files.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_guid.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/windows_version.hpp"
#include <algorithm>
#include <chrono>
#include <set>

namespace x64arm::driver_store {
struct Error {std::uint32_t code;};
inline constexpr std::u16string_view inf_directory=u"C:\\Windows\\INF";
inline constexpr std::u16string_view repository=u"C:\\Windows\\System32\\DriverStore\\FileRepository";
inline std::u16string fold(std::u16string value){for(auto& c:value)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return value;}
inline std::u16string trim(std::u16string value){const auto begin=value.find_first_not_of(u" \t\r\n");if(begin==value.npos)return {};return value.substr(begin,value.find_last_not_of(u" \t\r\n")-begin+1);}
inline std::vector<std::u16string> fields(std::u16string_view text,char16_t separator=u','){
    std::vector<std::u16string> result;std::u16string value;bool quoted=false;
    for(std::size_t i=0;i<text.size();++i){const auto c=text[i];if(c==u'"'){
        if(quoted && i+1<text.size() && text[i+1]==u'"'){value+=c;++i;}else quoted=!quoted;
    }else if(c==separator && !quoted){result.push_back(trim(std::move(value)));value.clear();}else value+=c;}
    result.push_back(trim(std::move(value)));return result;
}
inline std::optional<std::uint32_t> number(std::u16string_view text){
    if(text.empty())return 0;unsigned radix=10;std::size_t i=0;if(text.starts_with(u"0x") || text.starts_with(u"0X")){radix=16;i=2;}
    std::uint64_t value=0;for(;i<text.size();++i){auto c=text[i];const auto digit=c>=u'0' && c<=u'9'?c-u'0':c>=u'a' && c<=u'f'?c-u'a'+10:c>=u'A' && c<=u'F'?c-u'A'+10:99;
        if(static_cast<unsigned>(digit)>=radix || value>(UINT32_MAX-static_cast<unsigned>(digit))/radix)return {};value=value*radix+static_cast<unsigned>(digit);}return static_cast<std::uint32_t>(value);
}
inline std::shared_ptr<const std::vector<std::uint8_t>> read_file(ReadOnlyFiles& fs,const std::u16string& path){
    const auto name=utf16_to_utf8(path);const auto info=fs.information(name);if(info.error)throw Error{info.error};
    if(info.metadata.attributes&0x10U)throw Error{2};if(info.metadata.size>4*1024*1024)throw Error{8};
    const auto file=fs.open(name);if(file.error)throw Error{file.error};return file.bytes;
}
inline std::vector<std::u16string> inf_files(ReadOnlyFiles& fs,const std::u16string& directory){
    const auto list=fs.list(utf16_to_utf8(directory));if(list.error){if(list.error==2 || list.error==3)return {};throw Error{list.error};}
    std::vector<std::u16string> files;for(const auto& item:list.entries){
        if(item.metadata.attributes&0x10U)continue;auto name=utf8_to_utf16(item.name);if(!fold(name).ends_with(u".inf"))continue;
        if(files.size()>=4096)throw Error{8};files.push_back(directory+u"\\"+name);
    }std::sort(files.begin(),files.end());return files;
}
struct Model {
    std::u16string description,manufacturer,provider,filename,section;
    WindowsGuid class_guid{};std::vector<std::u16string> ids;
    std::uint64_t date{},version{},token{};bool excluded{};
};
using Lines=std::vector<std::pair<std::u16string,std::u16string>>;
using Sections=std::map<std::u16string,Lines>;
inline Sections parse(std::span<const std::uint8_t> bytes){
    if(bytes.empty())return {};
    std::u16string text;if(bytes.size()>=2 && bytes[0]==255 && bytes[1]==254){
        if(bytes.size()%2)return {};for(std::size_t i=2;i<bytes.size();i+=2)text+=static_cast<char16_t>(bytes[i]|(std::uint16_t{bytes[i+1]}<<8));
    }else{
        const auto skip=bytes.size()>=3 && bytes[0]==239 && bytes[1]==187 && bytes[2]==191?3U:0U;
        const std::string encoded(reinterpret_cast<const char*>(bytes.data()+skip),bytes.size()-skip);
        try{text=utf8_to_utf16(encoded);}catch(const std::invalid_argument&){if(skip)return {};text=cp1252_to_utf16(encoded);}
    }
    Sections sections;std::u16string current,pending;
    for(std::size_t at=0;at<text.size();){const auto stop=text.find(u'\n',at),end=stop==text.npos?text.size():stop;auto line=text.substr(at,end-at);at=end+1;
        bool quote=false;for(std::size_t i=0;i<line.size();++i){if(line[i]==u'"')quote=!quote;else if(line[i]==u';' && !quote){line.resize(i);break;}}
        line=trim(std::move(line));if(line.empty())continue;
        if(line.back()==u'\\'){line.pop_back();pending+=line;continue;}line=pending+line;pending.clear();
        if(line.front()==u'[' && line.back()==u']'){current=fold(trim(line.substr(1,line.size()-2)));continue;}if(current.empty())continue;
        const auto equal=line.find(u'=');auto key=trim(line.substr(0,equal));auto value=equal==line.npos?std::u16string{}:trim(line.substr(equal+1));sections[current].emplace_back(std::move(key),std::move(value));
    }return sections;
}
inline std::u16string value(const Sections& sections,std::u16string_view section,std::u16string_view key){
    const auto found=sections.find(std::u16string(section));if(found!=sections.end())for(const auto& [name,text]:found->second)if(fold(name)==key)return text;return {};
}
inline std::u16string expand(const Sections& sections,std::u16string text){
    const auto unquote=[](std::u16string value){value=trim(std::move(value));if(value.size()>=2 && value.front()==u'"' && value.back()==u'"')value=value.substr(1,value.size()-2);return value;};
    text=unquote(std::move(text));for(unsigned depth=0;depth<16;++depth){std::u16string result;bool changed=false;
        for(std::size_t i=0;i<text.size();){if(text[i]!=u'%'){result+=text[i++];continue;}const auto end=text.find(u'%',i+1);if(end==text.npos){result+=text.substr(i);break;}
            if(end==i+1){result+=u'%';i=end+1;continue;}const auto replacement=value(sections,u"strings",fold(text.substr(i+1,end-i-1)));
            if(replacement.empty())result+=text.substr(i,end-i+1);else {result+=unquote(replacement);changed=true;}i=end+1;
        }if(result.size()>65536)throw Error{8};text=std::move(result);if(!changed)break;
    }return text;
}
inline std::optional<std::array<std::uint32_t,6>> decoration(std::u16string text,bool x86){
    const auto parts=fields(fold(std::move(text)),u'.');if(parts.empty() || parts.size()>6)return {};
    if(parts[0]!=(x86?u"ntx86":u"ntamd64") && !(x86 && parts[0]==u"nt"))return {};
    std::array<std::uint32_t,6> score{};score[0]=parts[0]==u"nt"?1U:2U;
    for(std::size_t i=1;i<parts.size();++i){const auto n=number(parts[i]);if(!n)return {};score[i]=*n;}
    if(std::pair{score[1],score[2]}>std::pair{windows_version_major,windows_version_minor})return {};
    if(score[3] && score[3]!=1)return {};if(score[4]&~0x100U)return {};
    if(score[1]==windows_version_major && score[2]==windows_version_minor && score[5]>windows_version_build)return {};return score;
}
inline std::vector<Model> models(ReadOnlyFiles& fs,const std::u16string& path,bool x86){
    const auto bytes=read_file(fs,path);const auto inf=parse(*bytes);const auto signature=fold(expand(inf,value(inf,u"version",u"signature")));
    if(signature!=u"$windows nt$" && signature!=u"$chicago$")return {};
    const auto guid=parse_windows_guid(expand(inf,value(inf,u"version",u"classguid")),true);if(!guid)return {};
    const auto manufacturers=inf.find(u"manufacturer");if(manufacturers==inf.end())return {};
    Model base;base.filename=path;base.class_guid=*guid;base.provider=expand(inf,value(inf,u"version",u"provider"));
    const auto version=fields(value(inf,u"version",u"driverver"));if(version.size()==2){
        const auto date=fields(version[0],u'/'),parts=fields(version[1],u'.');
        if(date.size()==3){const auto month=number(date[0]),day=number(date[1]),year=number(date[2]);if(month && day && year && *year>=1601 && *year<=9999){
            const auto ymd=std::chrono::year(static_cast<int>(*year))/std::chrono::month(*month)/std::chrono::day(*day);
            if(ymd.ok())base.date=static_cast<std::uint64_t>(std::chrono::sys_days(ymd).time_since_epoch().count()+134774)*UINT64_C(864000000000);
        }}
        if(parts.size()==4)for(const auto& part:parts){const auto n=number(part);if(!n || *n>65535){base.version=0;break;}base.version=(base.version<<16)|*n;}
    }
    std::set<std::u16string> excluded;for(const auto& [key,list]:inf.contains(u"controlflags")?inf.at(u"controlflags"):Lines{})if(fold(key)==u"excludefromselect")for(auto item:fields(list))excluded.insert(fold(std::move(item)));
    std::vector<Model> result;
    for(const auto& [manufacturer,raw]:manufacturers->second){
        const auto parts=fields(raw.empty()?manufacturer:raw);std::u16string section;std::array<std::uint32_t,6> best{};
        if(parts.size()==1 && x86)section=fold(parts[0]);
        for(std::size_t i=1;i<parts.size();++i){const auto score=decoration(parts[i],x86);const auto candidate=fold(parts[0]+u"."+parts[i]);if(score && *score>best && inf.contains(candidate)){best=*score;section=candidate;}}
        const auto entries=inf.find(section);if(entries==inf.end())continue;
        for(const auto& [description,raw_model]:entries->second){const auto data=fields(raw_model);if(data.size()<2 || data[0].empty())continue;
            auto model=base;model.description=expand(inf,description);model.manufacturer=expand(inf,manufacturer);model.section=expand(inf,data[0]);
            for(std::size_t i=1;i<data.size();++i)if(!data[i].empty())model.ids.push_back(fold(expand(inf,data[i])));if(model.ids.empty())continue;
            model.excluded=excluded.contains(u"*") || std::any_of(model.ids.begin(),model.ids.end(),[&](const auto& id){return excluded.contains(id);});
            if(result.size()>=4096)throw Error{8};result.push_back(std::move(model));
        }
    }return result;
}
inline std::u16string stored_inf(ReadOnlyFiles& fs,std::u16string file,bool x86){
    if(file.empty())throw Error{87};try{file=normalized_windows_path(std::move(file),inf_directory);}catch(const std::invalid_argument&){throw Error{87};}
    const auto path=fold(file),published=fold(std::u16string(inf_directory)+u"\\"),store=fold(std::u16string(repository)+u"\\");
    if(!path.ends_with(u".inf"))throw Error{87};
    if(path.starts_with(store)){(void)read_file(fs,file);return file;}
    if(!path.starts_with(published) || path.find(u'\\',published.size())!=path.npos)throw Error{87};
    const auto source=read_file(fs,file);const auto packages=fs.list(utf16_to_utf8(repository));if(packages.error)throw Error{packages.error==3?2U:packages.error};
    // 只映射已發佈的 INF 與 DriverStore 內的實際副本，不解析 INF 安裝內容，
    // 也不把任意來源目錄當成已安裝的 package。
    std::vector<std::u16string> matches;std::size_t examined=0;
    for(const auto& package:packages.entries){if(!(package.metadata.attributes&0x10U) || package.name=="." || package.name=="..")continue;
        const auto name=fold(utf8_to_utf16(package.name));if((name.find(u"_amd64_")!=name.npos && x86) || (name.find(u"_x86_")!=name.npos && !x86) || name.find(u"_arm")!=name.npos)continue;
        for(const auto& candidate:inf_files(fs,std::u16string(repository)+u"\\"+utf8_to_utf16(package.name))){
            if(++examined>4096)throw Error{8};const auto info=fs.information(utf16_to_utf8(candidate));if(info.error || info.metadata.size!=source->size())continue;
            if(*read_file(fs,candidate)==*source)matches.push_back(candidate);
        }
    }
    if(matches.empty())throw Error{2};std::sort(matches.begin(),matches.end());return matches.front();
}
// 驅動套件寫入：SetupCopyOEMInf 將 INF 及其 [SourceDisksFiles]、目錄檔放入 sandbox 的 DriverStore，
// 再發佈為 %windir%\INF\oemN.inf；SetupUninstallOEMInf 反向移除。一律在 sandbox 的 Windows 磁碟內完成。
inline std::u16string parent_of(const std::u16string& path){const auto at=path.find_last_of(u'\\');return at==path.npos?std::u16string{}:path.substr(0,at);}
inline std::u16string leaf_of(const std::u16string& path){const auto at=path.find_last_of(u'\\');return at==path.npos?path:path.substr(at+1);}
inline void make_directories(ReadOnlyFiles& fs,const std::u16string& path,std::vector<std::u16string>* created=nullptr){
    for(std::size_t at=path.find(u'\\',3);;at=path.find(u'\\',at+1)){
        const auto part=path.substr(0,at==path.npos?path.size():at);const auto error=fs.create_directory(utf16_to_utf8(part));
        if(error && error!=183)throw Error{error};if(!error && created)created->push_back(part);if(at==path.npos)break;
    }
}
inline void remove_tree(ReadOnlyFiles& fs,const std::u16string& directory){
    const auto list=fs.list(utf16_to_utf8(directory));if(list.error)throw Error{list.error};
    for(const auto& item:list.entries){if(item.name=="." || item.name=="..")continue;const auto path=directory+u"\\"+utf8_to_utf16(item.name);
        if(item.metadata.attributes&0x10U)remove_tree(fs,path);else if(const auto error=fs.remove(utf16_to_utf8(path),false))throw Error{error};}
    if(const auto error=fs.remove(utf16_to_utf8(directory),true))throw Error{error};
}
inline bool same_file(ReadOnlyFiles& fs,const std::u16string& path,const std::vector<std::uint8_t>& bytes){
    const auto info=fs.information(utf16_to_utf8(path));return !info.error && !(info.metadata.attributes&0x10U) && info.metadata.size==bytes.size() && *read_file(fs,path)==bytes;
}
// 已發佈且內容相同的 oemN.inf。
inline std::optional<std::u16string> published_copy(ReadOnlyFiles& fs,const std::vector<std::uint8_t>& bytes){
    for(const auto& file:inf_files(fs,std::u16string(inf_directory)))if(fold(leaf_of(file)).starts_with(u"oem") && same_file(fs,file,bytes))return file;
    return std::nullopt;
}
// 套件內容的相對路徑：INF 本身以外的來源檔與目錄檔；拒絕跳出套件目錄的路徑。
inline std::vector<std::u16string> package_files(const Sections& inf){
    std::map<std::u16string,std::u16string> disks;
    for(const auto* section:{u"sourcedisksnames",u"sourcedisksnames.amd64"})if(const auto found=inf.find(section);found!=inf.end())
        for(const auto& [disk,raw]:found->second){const auto parts=fields(expand(inf,raw));disks[fold(disk)]=parts.size()>3?parts[3]:std::u16string{};}
    std::vector<std::u16string> files;
    const auto add=[&](std::u16string relative){
        std::replace(relative.begin(),relative.end(),u'/',u'\\');while(relative.starts_with(u"\\"))relative.erase(0,1);
        std::size_t start=0;for(;;){const auto end=relative.find(u'\\',start);const auto part=relative.substr(start,end==relative.npos?relative.npos:end-start);
            if(part.empty() || part==u"." || part==u".." || part.find(u':')!=part.npos)throw Error{87};if(end==relative.npos)break;start=end+1;}
        if(std::none_of(files.begin(),files.end(),[&](const auto& item){return fold(item)==fold(relative);})){if(files.size()>=4096)throw Error{8};files.push_back(std::move(relative));}
    };
    for(const auto* section:{u"sourcedisksfiles",u"sourcedisksfiles.amd64"})if(const auto found=inf.find(section);found!=inf.end())
        for(const auto& [name,raw]:found->second){
            const auto parts=fields(expand(inf,raw));const auto disk=disks.find(fold(parts.empty()?std::u16string{}:parts[0]));if(disk==disks.end())throw Error{0xe0000102U}; // ERROR_LINE_NOT_FOUND
            std::u16string relative=disk->second;if(parts.size()>1 && !parts[1].empty())relative+=u"\\"+parts[1];relative+=u"\\"+expand(inf,name);add(std::move(relative));
        }
    for(const auto* key:{u"catalogfile.ntamd64",u"catalogfile.nt",u"catalogfile"}){const auto catalog=expand(inf,value(inf,u"version",key));if(!catalog.empty()){add(catalog);break;}}
    return files;
}
struct StagedPackage {std::u16string published;bool existed{};};
inline StagedPackage stage_package(ReadOnlyFiles& fs,const std::u16string& source,std::uint32_t copy_style){
    const auto bytes=read_file(fs,source);const auto inf=parse(*bytes);const auto signature=fold(expand(inf,value(inf,u"version",u"signature")));
    if(signature!=u"$windows nt$" && signature!=u"$chicago$")throw Error{0xe0000100U}; // ERROR_WRONG_INF_STYLE
    if(!parse_windows_guid(expand(inf,value(inf,u"version",u"classguid")),true))throw Error{0xe0000206U}; // ERROR_INVALID_CLASS
    if(const auto existing=published_copy(fs,*bytes))return {*existing,true};
    if(copy_style&2U)throw Error{2}; // SP_COPY_REPLACEONLY：尚未發佈。
    const auto files=package_files(inf);
    std::uint64_t hash=1469598103934665603ULL;for(const auto byte:*bytes){hash^=byte;hash*=1099511628211ULL;}
    std::u16string digits;for(int shift=60;shift>=0;shift-=4)digits.push_back(u"0123456789abcdef"[(hash>>shift)&15U]);
    const auto name=leaf_of(source),folder=std::u16string(repository)+u"\\"+fold(name)+u"_amd64_"+digits,directory=parent_of(source);
    std::vector<std::u16string> created,copied;
    try{
        const bool staged=same_file(fs,folder+u"\\"+name,*bytes);
        if(!staged){
            make_directories(fs,folder,&created);
            for(const auto& relative:files){const auto target=folder+u"\\"+relative;make_directories(fs,parent_of(target),&created);
                if(const auto error=fs.copy(utf16_to_utf8(directory+u"\\"+relative),utf16_to_utf8(target),false))throw Error{error};copied.push_back(target);}
            if(const auto error=fs.copy(utf16_to_utf8(source),utf16_to_utf8(folder+u"\\"+name),false))throw Error{error};copied.push_back(folder+u"\\"+name);
        }
        make_directories(fs,std::u16string(inf_directory),&created);
        for(unsigned number=0;number<10000;++number){
            const auto candidate=std::u16string(inf_directory)+u"\\oem"+utf8_to_utf16(std::to_string(number))+u".inf";
            const auto info=fs.information(utf16_to_utf8(candidate));if(!info.error)continue;if(info.error!=2)throw Error{info.error};
            if(const auto error=fs.copy(utf16_to_utf8(folder+u"\\"+name),utf16_to_utf8(candidate),true))throw Error{error};
            if(copy_style&1U)(void)fs.remove(utf16_to_utf8(source),false); // SP_COPY_DELETESOURCE
            return {candidate,false};
        }
        throw Error{8};
    }catch(const Error&){
        for(auto it=copied.rbegin();it!=copied.rend();++it)(void)fs.remove(utf16_to_utf8(*it),false);
        for(auto it=created.rbegin();it!=created.rend();++it)(void)fs.remove(utf16_to_utf8(*it),true);
        throw;
    }
}
// 移除已發佈 INF 與其 DriverStore 套件；DriverStore 副本以內容比對找出。
inline void remove_package(ReadOnlyFiles& fs,const std::u16string& published){
    const auto store=stored_inf(fs,published,false);
    if(const auto error=fs.remove(utf16_to_utf8(published),false))throw Error{error};
    const auto pnf=published.substr(0,published.size()-4)+u".pnf";if(!fs.information(utf16_to_utf8(pnf)).error)(void)fs.remove(utf16_to_utf8(pnf),false);
    remove_tree(fs,parent_of(store));
}
}
