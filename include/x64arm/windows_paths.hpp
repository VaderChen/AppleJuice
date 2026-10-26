#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace x64arm {
// 與 CRT 引數解析互為反向；每個參數獨立加引號，保留內部與尾端反斜線。
inline std::u16string quote_windows_argument(std::u16string_view value){
    std::u16string out=u"\"";std::size_t slashes=0;
    for(auto c:value){if(c==u'\\'){++slashes;continue;}out.append(slashes*(c==u'"'?2:1),u'\\');slashes=0;if(c==u'"')out+=u'\\';out+=c;}
    out.append(slashes*2,u'\\');out+=u'"';return out;
}

// SHLWAPI 的傳統副檔名規則：空白或反斜線使前一個句點失效。
inline std::size_t windows_extension_offset(std::u16string_view path){
    const auto dot=path.find_last_of(u'.'),boundary=path.find_last_of(u"\\ ");
    return dot!=path.npos && (boundary==path.npos || dot>boundary)?dot:path.size();
}
// 客體掛載命名空間的穩定識別，不代表宿主 APFS／磁碟裝置 UUID。
inline std::u16string windows_volume_name(char16_t letter){
    if(letter>=u'a' && letter<=u'z')letter=static_cast<char16_t>(letter-32);
    if(letter<u'A' || letter>u'Z')throw std::invalid_argument("無效虛擬磁碟區");
    auto value=std::u16string(u"\\\\?\\Volume{78363461-726d-4000-8000-000000000000}\\");
    constexpr char16_t digits[]=u"0123456789abcdef";const auto number=letter-u'A'+1;value[45]=digits[number/16];value[46]=digits[number%16];return value;
}
inline std::string windows_volume_path(char letter){
    const auto name=windows_volume_name(static_cast<char16_t>(letter));std::string result;for(auto c:name)result+=static_cast<char>(c);return result;
}
template<class Character> inline char windows_volume_letter(std::basic_string_view<Character> path){
    if(path.size()<49)return 0;
    // 穩定磁碟區識別只有末兩位十六進位數字不同；固定前綴只需核對一次。
    constexpr std::u16string_view pattern=u"\\\\?\\volume{78363461-726d-4000-8000-000000000000}\\";
    const auto folded=[](Character value){auto c=static_cast<char16_t>(value);if(c==u'/')c=u'\\';if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);return c;};
    for(unsigned i=0;i<pattern.size();++i)if(i!=45 && i!=46 && folded(path[i])!=pattern[i])return 0;
    const auto digit=[&](unsigned at){const auto c=folded(path[at]);return c>=u'0' && c<=u'9'?static_cast<int>(c-u'0'):c>=u'a' && c<=u'f'?static_cast<int>(c-u'a'+10):-1;};
    const auto high=digit(45),low=digit(46);if(high<0 || low<0)return 0;
    const auto number=high*16+low;return number>=1 && number<=26?static_cast<char>('A'+number-1):0;
}
inline std::u16string windows_device_name(char16_t letter){
    if(letter>=u'a' && letter<=u'z')letter=static_cast<char16_t>(letter-32);
    if(letter<u'A' || letter>u'Z')throw std::invalid_argument("無效虛擬磁碟機");
    const auto number=std::to_string(letter-u'A'+1);return u"\\Device\\HarddiskVolume"+std::u16string(number.begin(),number.end());
}

enum class WindowsDosPathType : std::uint32_t {
    Unknown,UncAbsolute,DriveAbsolute,DriveRelative,Rooted,Relative,LocalDevice,RootLocalDevice
};
inline bool windows_path_separator(char16_t c){return c==u'\\' || c==u'/';}
inline char16_t windows_path_upper(char16_t c){return c>=u'a' && c<=u'z'?static_cast<char16_t>(c-32):c;}
inline WindowsDosPathType windows_dos_path_type(std::u16string_view path){
    using T=WindowsDosPathType;
    const auto at=[&](std::size_t i){return i<path.size()?path[i]:char16_t{};};
    if(windows_path_separator(at(0))){
        if(!windows_path_separator(at(1)))return T::Rooted;
        if((at(2)==u'.' || at(2)==u'?') && !at(3))return T::RootLocalDevice;
        if((at(2)==u'.' || at(2)==u'?') && windows_path_separator(at(3)))return T::LocalDevice;
        return T::UncAbsolute;
    }
    if(at(0) && at(1)==u':')return windows_path_separator(at(2))?T::DriveAbsolute:T::DriveRelative;
    return T::Relative;
}
// RtlIsDosDeviceName_U 的長度與位移皆以位元組編碼；命名空間前綴不能當一般檔名處理。
inline std::uint32_t windows_dos_device(std::u16string_view path){
    const auto type=windows_dos_path_type(path);
    if(type==WindowsDosPathType::UncAbsolute)return 0;
    if(type==WindowsDosPathType::LocalDevice){
        return path.size()==7 && path.substr(0,4)==u"\\\\.\\" &&
            windows_path_upper(path[4])==u'C' && windows_path_upper(path[5])==u'O' && windows_path_upper(path[6])==u'N'?0x00080006U:0;
    }
    std::size_t first=(type==WindowsDosPathType::DriveAbsolute || type==WindowsDosPathType::DriveRelative)?2:0;
    for(std::size_t i=first;i<path.size();++i)if(windows_path_separator(path[i]))first=i+1;
    auto last=path.find_first_of(u".:",first);if(last==path.npos)last=path.size();
    while(last>first && path[last-1]==u' ')--last;
    std::u16string name(path.substr(first,last-first));for(auto& c:name)c=windows_path_upper(c);
    bool device=name==u"CON" || name==u"PRN" || name==u"AUX" || name==u"NUL" || name==u"CONIN$" || name==u"CONOUT$";
    if(name.size()==4 && (name.starts_with(u"COM") || name.starts_with(u"LPT"))){
        const auto digit=name[3];device=(digit>=u'1' && digit<=u'9') || digit==u'\u00b9' || digit==u'\u00b2' || digit==u'\u00b3';
    }
    return device && first<=32767?static_cast<std::uint32_t>((first*2<<16)|(name.size()*2)):0;
}
inline std::size_t windows_unc_root_length(std::u16string_view path){
    std::size_t at=2;
    for(unsigned part=0;part<2;++part){while(at<path.size() && path[at]==u'\\')++at;while(at<path.size() && path[at]!=u'\\')++at;}
    return at;
}
// DOS NUL 與明確裝置名稱共用判定；延伸磁碟路徑中的同名一般檔案不轉成裝置。
inline bool windows_null_device(std::u16string_view path){
    const auto nul=[](std::u16string_view name){return name.size()==3 && windows_path_upper(name[0])==u'N' && windows_path_upper(name[1])==u'U' && windows_path_upper(name[2])==u'L';};
    if(path.size()==7 && (path.starts_with(u"\\\\.\\") || path.starts_with(u"\\\\?\\")))return nul(path.substr(4));
    const auto device=windows_dos_device(path);
    return device && nul(path.substr((device>>16)/2,(device&65535)/2));
}
// 共用元件折疊；純字串 Win32／NT 轉換與掛載層的尾字元政策分開。
inline std::u16string windows_collapse_path(std::u16string path,std::size_t root,bool trim_suffix,bool keep_separator){
    std::replace(path.begin(),path.end(),u'/',u'\\');
    std::vector<std::u16string_view> parts;const std::u16string_view view(path);
    for(std::size_t at=root;at<view.size();){
        auto end=view.find(u'\\',at);if(end==view.npos)end=view.size();
        auto part=view.substr(at,end-at);at=end+1;
        if(part==u".."){if(!parts.empty())parts.pop_back();}
        else if(!part.empty() && part!=u"."){
            if(trim_suffix && end<view.size() && part.back()==u'.' && part.find_first_not_of(u'.')!=part.npos)part.remove_suffix(1);
            parts.push_back(part);
        }
    }
    std::u16string result(path.substr(0,root));result.reserve(path.size());
    for(auto part:parts){if(!result.empty() && result.back()!=u'\\')result+=u'\\';result+=part;}
    if(keep_separator && !path.empty() && path.back()==u'\\' && result.back()!=u'\\')result+=u'\\';
    if(trim_suffix)while(result.size()>root && (result.back()==u' ' || result.back()==u'.'))result.pop_back();
    return result;
}
// GetFullPathName 明確要求正規化，連 \\?\ 前綴也適用；NT 的字面轉換由呼叫端先辨識。
inline std::u16string full_windows_dos_path(std::u16string path,std::u16string_view current,std::u16string_view drive_current={}){
    using T=WindowsDosPathType;
    if(path.empty() || path.find_first_not_of(u' ')==path.npos || path.find(u'\0')!=path.npos)throw std::invalid_argument("無效 DOS 路徑");
    if(const auto device=windows_dos_device(path))return u"\\\\.\\"+path.substr((device>>16)/2,(device&65535)/2);
    std::replace(path.begin(),path.end(),u'/',u'\\');
    if(path.size()>2){std::size_t out=2;for(std::size_t i=2;i<path.size();++i)if(path[i]!=u'\\' || path[out-1]!=u'\\')path[out++]=path[i];path.resize(out);}
    auto type=windows_dos_path_type(path);std::u16string base(current);std::replace(base.begin(),base.end(),u'/',u'\\');
    if(type==T::DriveRelative){
        const auto drive=windows_path_upper(path[0]);
        if(drive<u'A' || drive>u'Z')throw std::invalid_argument("無效磁碟機代號");
        if(base.size()<2 || windows_path_upper(base[0])!=drive || base[1]!=u':'){
            base=drive_current;
            if(windows_dos_path_type(base)!=T::DriveAbsolute || windows_path_upper(base[0])!=drive || base.find(u'\0')!=base.npos)base=std::u16string{path[0],u':',u'\\'};
        }
        path.erase(0,2);type=T::Relative;
    }
    if(type==T::Relative){
        if(base.empty() || (windows_dos_path_type(base)!=T::DriveAbsolute && windows_dos_path_type(base)!=T::UncAbsolute))throw std::invalid_argument("目前目錄不是絕對路徑");
        if(!windows_path_separator(base.back()) && !path.empty())base+=u'\\';
        path=base+path;type=windows_dos_path_type(path);
    }else if(type==T::Rooted){
        const auto base_type=windows_dos_path_type(base);
        if(base_type==T::DriveAbsolute)path=base.substr(0,2)+path;
        else if(base_type==T::UncAbsolute)path=base.substr(0,windows_unc_root_length(base))+path;
        else throw std::invalid_argument("目前目錄沒有根路徑");
        type=windows_dos_path_type(path);
    }else if(type==T::RootLocalDevice){path=u"\\\\.\\";type=T::LocalDevice;}
    const auto root=type==T::UncAbsolute?windows_unc_root_length(path):type==T::LocalDevice?4U:3U;
    if(type==T::DriveAbsolute && (windows_path_upper(path[0])<u'A' || windows_path_upper(path[0])>u'Z'))throw std::invalid_argument("無效磁碟機代號");
    return windows_collapse_path(std::move(path),root,true,true);
}
inline std::size_t windows_path_file_part(std::u16string_view path,bool device=false){
    const auto slash=path.find_last_of(u'\\');
    return device || slash==path.npos || slash<2 || slash+1==path.size()?path.npos:slash+1;
}
template<class Environment> inline std::u16string full_windows_environment_path(std::u16string path,std::u16string_view current,const Environment& environment){
    std::u16string_view drive_current;
    if(windows_dos_path_type(path)==WindowsDosPathType::DriveRelative){
        const auto key=std::u16string{u'=',windows_path_upper(path[0]),u':'};
        if(const auto value=environment.find(key);value!=environment.end())drive_current=value->second.second;
    }
    return full_windows_dos_path(std::move(path),current,drive_current);
}

inline std::u16string normalized_windows_path(std::u16string path,std::u16string_view current=u"C:\\") {
    std::replace(path.begin(),path.end(),u'/',u'\\');
    if(path.starts_with(u"\\\\"))throw std::invalid_argument("UNC／裝置路徑尚未映射");
    const auto drive_letter=[](char16_t letter){
        if(letter>=u'a' && letter<=u'z')letter=static_cast<char16_t>(letter-32);
        if(letter<u'A' || letter>u'Z')throw std::invalid_argument("無效磁碟機代號");
        return letter;
    };
    std::u16string base(current);std::replace(base.begin(),base.end(),u'/',u'\\');
    if(base.size()<3 || base[1]!=u':' || base[2]!=u'\\')throw std::invalid_argument("目前目錄不是絕對磁碟機路徑");
    const auto current_drive=drive_letter(base[0]);auto drive=current_drive;
    bool rooted=!path.empty() && path.front()==u'\\';
    if(path.size()>=2 && path[1]==u':'){
        drive=drive_letter(path[0]);
        path.erase(0,2);rooted=!path.empty() && path.front()==u'\\';
    }
    if(path.find(u':')!=path.npos || path.find(u'\0')!=path.npos)throw std::invalid_argument("路徑包含非法分隔符");
    // 同磁碟機相對路徑沿用目前目錄；未提供其他磁碟機的目前目錄時從根目錄解析。
    if(!rooted && drive==current_drive)path=base.substr(2)+u"\\"+path;
    return windows_collapse_path(std::u16string{drive,u':',u'\\'}+path,3,false,false);
}
}
