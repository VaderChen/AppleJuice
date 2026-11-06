#pragma once
#include <array>
#include <initializer_list>
#include <string>
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
// 引擎內建 VC++ 執行階段對外公開的版本；RT_VERSION 與 Runtimes 登錄機碼共用。
inline constexpr std::array<std::uint32_t,4> vc_runtime_version{14,44,35211,0};
// 系統 DLL 回報 Windows 版本；VC 可再散發 DLL 的版本屬於各自的工具組，程式會拿它判斷執行階段是否夠新。
inline std::array<std::uint32_t,4> builtin_file_version(std::string_view name){
    std::string lower(name);for(auto& c:lower)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
    const std::string_view text=lower;
    const auto has=[&](std::initializer_list<std::string_view> prefixes){for(const auto prefix:prefixes)if(text.starts_with(prefix))return true;return false;};
    if(has({"msvcp140","vcruntime140","concrt140","vcomp140","vccorlib140","mfc140","mfcm140"}))return vc_runtime_version;
    if(has({"msvcp120","msvcr120","vcomp120","mfc120"}))return {12,0,40664,0};
    if(has({"msvcp110","msvcr110","vcomp110","mfc110"}))return {11,0,61135,400};
    if(has({"msvcp100","msvcr100","vcomp100","mfc100"}))return {10,0,40219,473};
    if(has({"msvcp90","msvcr90","vcomp90","mfc90"}))return {9,0,30729,9625};
    if(has({"msvcp80","msvcr80","vcomp80","mfc80"}))return {8,0,50727,6229};
    return {windows_version_major,windows_version_minor,windows_version_build,windows_version_revision};
}
// 載入映像 RT_VERSION 與檔案版本查詢共用同一份 provider metadata。
inline std::vector<std::uint8_t> builtin_version_info(std::string_view name){
    using namespace builtin_version_detail;
    Bytes fixed(52);put(fixed,0,4,0xfeef04bd);put(fixed,4,4,0x10000);
    const auto file_version=builtin_file_version(name);
    const auto ms=(file_version[0]<<16)|file_version[1];
    const auto ls=(file_version[2]<<16)|file_version[3];
    put(fixed,8,4,ms);put(fixed,12,4,ls);put(fixed,16,4,ms);put(fixed,20,4,ls);
    put(fixed,24,4,0x3f);put(fixed,32,4,0x40004);put(fixed,36,4,2);
    const auto version=utf8_to_utf16(std::to_string(file_version[0])+"."+std::to_string(file_version[1])+"."+std::to_string(file_version[2])+"."+std::to_string(file_version[3]));
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
