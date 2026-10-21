#pragma once
#include <algorithm>
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
    // path 在組裝結果前維持不變，元件借用其儲存空間即可。
    std::vector<std::u16string_view> parts;const std::u16string_view view(path);
    for(std::size_t at=0;at<view.size();){auto end=view.find(u'\\',at);if(end==view.npos)end=view.size();const auto part=view.substr(at,end-at);at=end+1;if(part==u".."){if(!parts.empty())parts.pop_back();}else if(!part.empty() && part!=u".")parts.push_back(part);}
    std::u16string result{drive,u':',u'\\'};result.reserve(path.size()+3);for(const auto part:parts){if(result.back()!=u'\\')result+=u'\\';result+=part;}return result;
}
}
