#pragma once
#include "x64arm/windows_paths.hpp"

namespace x64arm::detail {
struct VolumeDevicePath {char volume{};bool drive_alias{};};
// GLOBALROOT 指向客體物件管理員的卷身分；經既有 GUID 掛載解析器，
// 不依賴可改綁的 DOS 磁碟代號，也不把裝置名稱當成宿主絕對路徑。
template<class C> std::basic_string<C> globalroot_file_path(std::basic_string_view<C> path,bool native=false){
    const auto prefix=[](std::basic_string_view<C> text,std::u16string_view expected){
        if(text.size()<expected.size())return false;
        for(std::size_t i=0;i<expected.size();++i){auto c=static_cast<char16_t>(text[i]);if(c==u'/')c=u'\\';if(windows_path_upper(c)!=windows_path_upper(expected[i]))return false;}
        return true;
    };
    if(prefix(path,u"\\\\?\\") || prefix(path,u"\\\\.\\") || prefix(path,u"\\??\\")){
        path.remove_prefix(4);if(!prefix(path,u"GLOBALROOT\\"))return {};path.remove_prefix(10);
    }else if(!native)return {};
    constexpr std::u16string_view device=u"\\Device\\HarddiskVolume";
    if(!prefix(path,device))return {};path.remove_prefix(device.size());
    if(path.empty() || path[0]<C('1') || path[0]>C('9'))return {};
    unsigned number=0;std::size_t at=0;
    while(at<path.size() && path[at]>=C('0') && path[at]<=C('9')){number=number*10+static_cast<unsigned>(path[at]-C('0'));if(number>26)return {};++at;}
    if(at==path.size() || (path[at]!=C('\\') && path[at]!=C('/')))return {};
    const auto volume=windows_volume_name(static_cast<char16_t>(u'A'+number-1));
    std::basic_string<C> result(volume.begin(),volume.end());result.append(path.substr(at+1));return result;
}
inline VolumeDevicePath volume_device_path(std::u16string_view path){
    if(path.size()==6 && (path.starts_with(u"\\\\.\\") || path.starts_with(u"\\\\?\\")) && path[5]==u':'){
        const auto letter=windows_path_upper(path[4]);if(letter>=u'A' && letter<=u'Z')return {static_cast<char>(letter),true};
    }
    // 無尾斜線才是卷裝置；有尾斜線沿用檔案系統根目錄的契約。
    if(path.empty() || windows_path_separator(path.back()))return {};
    auto root=std::u16string(path)+u'\\';
    if(root.starts_with(u"\\\\.\\"))root[2]=u'?';
    if(root.size()==49)if(const auto volume=windows_volume_letter(std::u16string_view(root)))return {volume,false};
    const auto resolved=globalroot_file_path(std::u16string_view(root),true);
    if(resolved.size()==49)return {windows_volume_letter(std::u16string_view(resolved)),false};
    return {};
}
}
