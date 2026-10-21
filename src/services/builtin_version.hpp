#pragma once
#include "x64arm/unicode.hpp"
#include "x64arm/windows_version.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace x64arm {
namespace builtin_version_detail {
using Bytes=std::vector<std::uint8_t>;
inline void put(Bytes& bytes,std::size_t at,unsigned width,std::uint32_t value){
    for(unsigned i=0;i<width;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));
}
inline void align(Bytes& bytes){bytes.resize((bytes.size()+3)&~std::size_t{3});}
inline Bytes node(std::u16string_view key,bool text,const Bytes& value,std::uint16_t length,const std::vector<Bytes>& children={}){
    Bytes bytes(6);const auto name=utf16_bytes(key);bytes.insert(bytes.end(),name.begin(),name.end());align(bytes);
    bytes.insert(bytes.end(),value.begin(),value.end());
    for(const auto& child:children){align(bytes);bytes.insert(bytes.end(),child.begin(),child.end());}
    put(bytes,0,2,static_cast<std::uint32_t>(bytes.size()));put(bytes,2,2,length);put(bytes,4,2,text?1U:0U);return bytes;
}
inline Bytes string(std::u16string_view key,std::u16string_view value){return node(key,true,utf16_bytes(value),static_cast<std::uint16_t>(value.size()+1));}
}
// 載入映像 RT_VERSION 與檔案版本查詢共用同一份 provider metadata。
inline std::vector<std::uint8_t> builtin_version_info(std::string_view name){
    using namespace builtin_version_detail;
    Bytes fixed(52);put(fixed,0,4,0xfeef04bd);put(fixed,4,4,0x10000);
    const auto ms=(windows_version_major<<16)|windows_version_minor;
    const auto ls=(windows_version_build<<16)|windows_version_revision;
    put(fixed,8,4,ms);put(fixed,12,4,ls);put(fixed,16,4,ms);put(fixed,20,4,ls);
    put(fixed,24,4,0x3f);put(fixed,32,4,0x40004);put(fixed,36,4,2);
    const auto version=utf8_to_utf16(std::to_string(windows_version_major)+"."+std::to_string(windows_version_minor)+"."+std::to_string(windows_version_build)+"."+std::to_string(windows_version_revision));
    const auto filename=utf8_to_utf16(name);
    const auto strings=node(u"StringFileInfo",true,{},0,{node(u"040904B0",true,{},0,{
        string(u"CompanyName",u"MacWin"),string(u"FileDescription",u"MacWin Windows API provider"),
        string(u"FileVersion",version),string(u"InternalName",filename),string(u"OriginalFilename",filename),
        string(u"ProductName",u"MacWin Windows compatibility engine"),string(u"ProductVersion",version)})});
    const auto vars=node(u"VarFileInfo",true,{},0,{node(u"Translation",false,Bytes{9,4,0xb0,4},4)});
    return node(u"VS_VERSION_INFO",false,fixed,52,{strings,vars});
}
inline std::vector<std::uint8_t> builtin_version_resource(std::string_view name,std::uint32_t rva){
    using namespace builtin_version_detail;
    const auto info=builtin_version_info(name);Bytes bytes(88+info.size());
    for(const auto offset:{0U,24U,48U})put(bytes,offset+14,2,1);
    put(bytes,16,4,16);put(bytes,20,4,0x80000018);
    put(bytes,40,4,1);put(bytes,44,4,0x80000030);
    put(bytes,64,4,0x409);put(bytes,68,4,72);
    put(bytes,72,4,rva+88);put(bytes,76,4,static_cast<std::uint32_t>(info.size()));put(bytes,80,4,1200);
    std::copy(info.begin(),info.end(),bytes.begin()+88);return bytes;
}
}
