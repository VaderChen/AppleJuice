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
// DeriveCapabilitySidsFromName：名稱轉大寫後 UTF-16LE 的 SHA-256 拆成 8 個子授權；群組 SID 為 S-1-5-32-<8 個>，
// 能力 SID 為 S-1-15-3-1024-<8 個>；舊版具名能力（internetClient 等）另含 S-1-15-3-<RID>，排在雜湊版之前。
struct CapabilitySids {std::vector<std::vector<std::uint8_t>> groups,capabilities;};
inline CapabilitySids capability_sids(std::u16string name){
    if(name.size()>32767)throw std::invalid_argument("能力名稱過長");
    auto folded=name;for(auto& c:folded)c=static_cast<char16_t>(u_toupper(c));
    auto input=utf16_bytes(folded);input.resize(folded.size()*2);
    CryptoHash hash("SHA256",false,{});hash.update(input);const auto digest=hash.digest();
    const auto make=[&](std::uint8_t authority,std::initializer_list<std::uint32_t> prefix){
        std::vector<std::uint8_t> sid(8);sid[0]=1;sid[7]=authority;
        const auto add=[&](std::uint32_t value){for(unsigned i=0;i<4;++i)sid.push_back(static_cast<std::uint8_t>(value>>(8*i)));++sid[1];};
        for(const auto value:prefix)add(value);
        for(unsigned at=0;at<32;at+=4){std::uint32_t value{};for(unsigned i=0;i<4;++i)value|=std::uint32_t{digest[at+i]}<<(8*i);add(value);}
        return sid;};
    CapabilitySids out;out.groups.push_back(make(5,{32}));
    static constexpr std::pair<std::u16string_view,std::uint32_t> legacy[]{
        {u"internetClient",1},{u"internetClientServer",2},{u"privateNetworkClientServer",3},{u"picturesLibrary",4},{u"videosLibrary",5},{u"musicLibrary",6},
        {u"documentsLibrary",7},{u"enterpriseAuthentication",8},{u"sharedUserCertificates",9},{u"removableStorage",10},{u"appointments",11},{u"contacts",12}};
    for(const auto& [known,rid]:legacy){
        if(known.size()!=name.size() || !std::equal(known.begin(),known.end(),name.begin(),[](char16_t a,char16_t b){return u_toupper(a)==u_toupper(b);}))continue;
        std::vector<std::uint8_t> sid(8);sid[0]=1;sid[1]=2;sid[7]=15;for(const std::uint32_t value:{3U,rid})for(unsigned i=0;i<4;++i)sid.push_back(static_cast<std::uint8_t>(value>>(8*i)));
        out.capabilities.push_back(std::move(sid));
    }
    out.capabilities.push_back(make(15,{3,1024}));
    return out;
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
