#pragma once
#include "crt_lowio_abi.hpp"
#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace x64arm::crt_lowio {
struct StreamMode {
    char operation{};
    bool update{},exclusive{},noinherit{},temporary{};
    bool explicit_translation{},translated{true};
    int commit{-1};
    std::uint32_t unicode_flags{},attributes{};
    std::uint32_t disposition()const{return operation=='r'?3U:operation=='a'?4U:exclusive?1U:2U;}
};
inline std::string_view trim_mode(std::string_view value){
    while(!value.empty() && (value.front()==' ' || value.front()=='\t'))value.remove_prefix(1);
    while(!value.empty() && (value.back()==' ' || value.back()=='\t'))value.remove_suffix(1);
    return value;
}
inline std::optional<StreamMode> stream_mode(std::string_view value){
    StreamMode result;const auto comma=value.find(',');auto base=trim_mode(value.substr(0,comma));
    if(base.empty() || (base.front()!='r' && base.front()!='w' && base.front()!='a'))return {};
    result.operation=base.front();base.remove_prefix(1);
    for(const auto option:base){
        switch(option){
        case '+':if(result.update)return {};result.update=true;break;
        case 'b':case 't':if(result.explicit_translation)return {};result.explicit_translation=true;result.translated=option=='t';break;
        case 'x':if(result.exclusive || result.operation!='w')return {};result.exclusive=true;break;
        case 'N':result.noinherit=true;break;
        case 'D':result.temporary=true;break;
        case 'T':result.attributes|=0x100U;break;
        case 'c':result.commit=1;break;
        case 'n':result.commit=0;break;
        case 'S':case 'R':break; // cache hint 不改變檔案或資料格式。
        default:return {};
        }
    }
    if(comma!=value.npos){
        const auto encoding=trim_mode(value.substr(comma+1));
        if(encoding=="ccs=UTF-8")result.unicode_flags=0x40000;
        else if(encoding=="ccs=UTF-16LE")result.unicode_flags=0x20000;
        else if(encoding=="ccs=UNICODE")result.unicode_flags=0x10000;
        else return {};
        // ccs 指定 Unicode 文字轉碼；常用 rb／wb 的 b 不取消 ccs。
        result.translated=true;result.explicit_translation=true;
    }
    return result;
}
inline std::uint32_t open_translation(std::uint32_t flags,std::uint32_t fallback){
    const auto explicit_mode=flags&0x7c000U;
    return explicit_mode?explicit_mode:(fallback&0x7c000U)?fallback&0x7c000U:0x4000U;
}
inline bool valid_open_translation(std::uint32_t flags){
    const auto unicode_flags=flags&0x70000U;
    return (flags&0xc000U)!=0xc000U && !(unicode_flags && ((unicode_flags&(unicode_flags-1)) || (flags&0x8000U)));
}
struct Bom {TextMode mode{TextMode::Ansi};std::size_t bytes{};std::uint32_t error{};};
inline Bom byte_order_mark(std::span<const std::uint8_t> bytes){
    if(bytes.size()>=3 && bytes[0]==0xef && bytes[1]==0xbb && bytes[2]==0xbf)return {TextMode::Utf8,3,0};
    if(bytes.size()>=2 && bytes[0]==0xff && bytes[1]==0xfe)return {TextMode::Utf16,2,0};
    if(bytes.size()>=2 && bytes[0]==0xfe && bytes[1]==0xff)return {TextMode::Ansi,0,22};
    return {};
}
inline std::span<const std::uint8_t> byte_order_mark(TextMode mode){
    static constexpr std::array<std::uint8_t,3> u8{0xef,0xbb,0xbf};
    static constexpr std::array<std::uint8_t,2> u16{0xff,0xfe};
    return mode==TextMode::Utf8?std::span<const std::uint8_t>(u8):mode==TextMode::Utf16?std::span<const std::uint8_t>(u16):std::span<const std::uint8_t>{};
}
}
