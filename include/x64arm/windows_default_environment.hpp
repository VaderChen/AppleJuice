#pragma once
#include <map>
#include <string>
#include <array>
#include <optional>
#include "x64arm/unicode.hpp"
namespace x64arm {
using GuestEnvironment=std::map<std::u16string,std::pair<std::u16string,std::u16string>>;
inline GuestEnvironment guest_default_environment(bool user){
    GuestEnvironment values;
    const auto add=[&](const char16_t* name,const char16_t* value){std::u16string key=name;for(auto& c:key)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);values[key]={name,value};};
    for(const auto& [name,value]:std::array{std::pair{u"ProgramFiles",u"C:\\Program Files"},std::pair{u"ProgramW6432",u"C:\\Program Files"},std::pair{u"SystemRoot",u"C:\\Windows"},std::pair{u"windir",u"C:\\Windows"}})add(name,value);
    // Windows 系統環境固定存在的項目；libuv 等函式庫以 PATH 是否存在作為建立子程序的前提。
    for(const auto& [name,value]:std::array{std::pair{u"Path",u"C:\\Windows\\system32;C:\\Windows;C:\\Windows\\System32\\Wbem;C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\"},
        std::pair{u"PATHEXT",u".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC"},std::pair{u"ComSpec",u"C:\\Windows\\system32\\cmd.exe"},
        std::pair{u"SystemDrive",u"C:"},std::pair{u"OS",u"Windows_NT"},std::pair{u"ProgramData",u"C:\\ProgramData"},std::pair{u"ALLUSERSPROFILE",u"C:\\ProgramData"},
        std::pair{u"CommonProgramFiles",u"C:\\Program Files\\Common Files"},std::pair{u"PUBLIC",u"C:\\Users\\Public"}})add(name,value);
    if(user)for(const auto& [name,value]:std::array{std::pair{u"USERPROFILE",u"C:\\Users\\User"},std::pair{u"APPDATA",u"C:\\Users\\User\\AppData\\Roaming"},std::pair{u"LOCALAPPDATA",u"C:\\Users\\User\\AppData\\Local"},std::pair{u"TEMP",u"C:\\Users\\User\\AppData\\Local\\Temp"},std::pair{u"TMP",u"C:\\Users\\User\\AppData\\Local\\Temp"}})add(name,value);
    return values;
}
inline std::optional<GuestEnvironment> decode_guest_environment(std::span<const std::uint8_t> bytes,bool wide){
    const unsigned width=wide?2:1;if(bytes.size()<width*2 || bytes.size()%width)return {};
    const auto character=[&](std::size_t at){return static_cast<unsigned>(bytes[at])|(wide?static_cast<unsigned>(bytes[at+1])<<8:0);};
    GuestEnvironment values;std::size_t at=0;
    while(at<bytes.size()){
        const auto begin=at;while(at<bytes.size() && character(at)){at+=width;if(at-begin>32767U*width)return {};}
        if(at==bytes.size())return {};if(at==begin){if(begin==0){if(bytes.size()!=2*width || character(width))return {};}else if(at+width!=bytes.size())return {};return values;}
        std::u16string text;if(wide){for(auto i=begin;i<at;i+=width)text.push_back(static_cast<char16_t>(character(i)));}else text=utf8_to_utf16(std::string_view(reinterpret_cast<const char*>(bytes.data()+begin),at-begin));at+=width;
        const auto equals=text.find(u'=',text[0]==u'='?1:0);if(equals==text.npos || !equals)return {};const auto name=text.substr(0,equals);auto key=name;for(auto& c:key)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);values.try_emplace(std::move(key),name,text.substr(equals+1));
    }return {};
}
inline std::vector<std::uint8_t> encode_guest_environment(const GuestEnvironment& values){
    std::vector<std::uint8_t> bytes;for(const auto& [key,value]:values){(void)key;const auto text=utf16_bytes(value.first+u"="+value.second);if(bytes.size()+text.size()>16U*1024U*1024U-4)throw std::length_error("客體環境區塊過大");bytes.insert(bytes.end(),text.begin(),text.end());}bytes.resize(bytes.size()+(values.empty()?4:2));return bytes;
}

}
