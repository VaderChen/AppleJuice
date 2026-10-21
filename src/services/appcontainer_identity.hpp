#pragma once
#include "x64arm/crypto.hpp"
#include "x64arm/unicode.hpp"
#include "windows_sddl.hpp"
#include <unicode/uchar.h>

namespace x64arm::appcontainer {
inline std::vector<std::uint8_t> derive(std::u16string name){
    if(name.empty() || name.size()>32767)throw std::invalid_argument("AppContainer 名稱長度無效");
    for(auto& c:name)c=static_cast<char16_t>(u_tolower(c));
    auto input=utf16_bytes(name);input.resize(name.size()*2);
    CryptoHash hash("SHA256",false,{});hash.update(input);const auto digest=hash.digest();
    std::vector<std::uint8_t> sid(40);sid[0]=1;sid[1]=8;sid[7]=15;sid[8]=2;
    std::copy_n(digest.begin(),28,sid.begin()+12);return sid;
}
inline std::string name_of_sid(const std::vector<std::uint8_t>& sid){
    if(sid.size()!=40 || sid[0]!=1 || sid[1]!=8 || sid[7]!=15 || sid[8]!=2 ||
       std::any_of(sid.begin()+2,sid.begin()+7,[](auto c){return c!=0;}) || sid[9] || sid[10] || sid[11])
        throw std::invalid_argument("不是 AppContainer 設定檔 SID");
    std::string text="S-1-15-2";
    for(unsigned at=12;at<40;at+=4){std::uint32_t value{};for(unsigned i=0;i<4;++i)value|=std::uint32_t{sid[at+i]}<<(8*i);text+='-';text+=std::to_string(value);}
    return text;
}
inline bool valid_profile_name(std::u16string_view name){
    return !name.empty() && name.size()<=64 && std::all_of(name.begin(),name.end(),[](char16_t c){
        return (c>=u'a' && c<=u'z') || (c>=u'A' && c<=u'Z') || (c>=u'0' && c<=u'9') || c==u'-' || c==u'_' || c==u'.' || c==u' ';
    });
}
}
