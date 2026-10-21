#pragma once
#include "crt_lowio_abi.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace x64arm::crt_lowio {
inline char16_t unit(std::span<const std::uint8_t> bytes,std::size_t at){return static_cast<char16_t>(bytes[at]|(unsigned{bytes[at+1]}<<8));}
inline void append_unit(std::vector<std::uint8_t>& bytes,char16_t value){bytes.push_back(static_cast<std::uint8_t>(value));bytes.push_back(static_cast<std::uint8_t>(value>>8));}

// 客體 Unicode lowio 一律輸入／輸出 UTF-16；磁碟端碼頁由 descriptor 決定。
inline std::vector<std::uint8_t> encode_text(std::span<const std::uint8_t> source,TextMode mode){
    if(mode==TextMode::Ansi){
        std::vector<std::uint8_t> output;output.reserve(source.size()+static_cast<std::size_t>(std::count(source.begin(),source.end(),10)));
        std::size_t at=0;
        while(at<source.size()){
            const auto* newline=static_cast<const std::uint8_t*>(std::memchr(source.data()+at,10,source.size()-at));
            const auto end=newline?static_cast<std::size_t>(newline-source.data()):source.size();
            output.insert(output.end(),source.begin()+static_cast<std::ptrdiff_t>(at),source.begin()+static_cast<std::ptrdiff_t>(end));
            if(!newline)break;output.push_back(13);output.push_back(10);at=end+1;
        }return output;
    }
    if(source.size()%2)throw std::invalid_argument("Unicode lowio 需要偶數 bytes");
    std::u16string units;units.reserve(source.size()/2);
    for(std::size_t i=0;i<source.size();i+=2){
        auto value=unit(source,i);if(value==10)units.push_back(13);
        // UCRT 使用 CP_UTF8、flags=0；孤立 surrogate 依 Windows Unicode 契約替代。
        if(mode==TextMode::Utf8 && value>=0xd800 && value<=0xdfff){
            if(value<=0xdbff && i+3<source.size() && unit(source,i+2)>=0xdc00 && unit(source,i+2)<=0xdfff){units.push_back(value);units.push_back(unit(source,i+2));i+=2;continue;}
            value=u'\ufffd';
        }
        units.push_back(value);
    }
    if(mode==TextMode::Utf16){std::vector<std::uint8_t> output;output.reserve(units.size()*2);for(auto value:units)append_unit(output,value);return output;}
    const auto encoded=utf16_to_utf8(units);return {encoded.begin(),encoded.end()};
}

// 只有實際短寫入才掃描來源；正常完成不保存逐字元的配置與索引。
inline std::size_t written_source(std::span<const std::uint8_t> source,TextMode mode,std::size_t written){
    std::size_t used=0,consumed=0;
    for(std::size_t at=0;at<source.size();){
        const auto start=at;std::uint32_t value=source[at++];std::size_t extent=1;
        if(mode!=TextMode::Ansi){
            if(start+1>=source.size())break;value=unit(source,start);at=start+2;extent=2;
            if(mode==TextMode::Utf8){
                if(value>=0xd800 && value<=0xdbff){
                    if(at+1<source.size() && unit(source,at)>=0xdc00 && unit(source,at)<=0xdfff){value=0x10000+((value-0xd800)<<10)+(unit(source,at)-0xdc00);at+=2;}
                    else value=0xfffd;
                }else if(value>=0xdc00 && value<=0xdfff)value=0xfffd;
                extent=value<0x80?1:value<0x800?2:value<0x10000?3:4;
            }
        }
        if(value==10)extent+=mode==TextMode::Utf16?2:1;
        if(extent>written-used)break;used+=extent;consumed=at;
    }return consumed;
}

struct ReadText {
    std::vector<std::uint8_t> bytes;
    std::size_t consumed{},needed{};
    std::optional<char16_t> low_surrogate;
    std::uint32_t error{};
    bool eof{},read_newline{};
};
inline ReadText decode_text(std::span<const std::uint8_t> source,TextMode mode,std::size_t capacity,bool final){
    ReadText result;
    if(mode!=TextMode::Ansi && capacity%2){result.error=22;return result;}
    result.bytes.reserve(std::min(source.size()*(mode==TextMode::Utf8?2U:1U),capacity));
    const auto character=[&](std::size_t at,std::u16string& output,std::size_t& length)->bool{
        if(at>=source.size()){length=1;return false;}
        const auto first=source[at];length=mode==TextMode::Utf16?2U:mode==TextMode::Ansi || first<128?1U:first>=0xc2 && first<=0xdf?2U:first>=0xe0 && first<=0xef?3U:first>=0xf0 && first<=0xf4?4U:0U;
        if(mode==TextMode::Utf8 && first>=128){
            // 僅保留可能成為有效字元的前綴；已無效序列以 maximal subpart 替代，
            // 不吞掉其後 ASCII、CRLF 或另一個有效字元。
            const auto expected=length;
            length=1;
            if(expected)for(;length<expected && at+length<source.size();++length){
                const auto next=source[at+length];
                if((next&0xc0U)!=0x80U || (length==1 && ((first==0xe0 && next<0xa0) || (first==0xed && next>0x9f) || (first==0xf0 && next<0x90) || (first==0xf4 && next>0x8f))))break;
            }
            if(!expected || length<expected){
                if(expected && at+length==source.size() && !final){result.needed=at+expected;return false;}
                output.assign(1,u'\ufffd');return true;
            }
        }
        if(length>source.size()-at){if(final)result.consumed=source.size();else result.needed=at+length;return false;}
        if(mode==TextMode::Utf16)output.assign(1,unit(source,at));
        else if(mode==TextMode::Ansi)output.assign(1,first);
        else output=utf8_to_utf16(std::string_view(reinterpret_cast<const char*>(source.data()+at),length));
        return true;
    };
    while(result.consumed<source.size() && result.bytes.size()<capacity){
        if(mode==TextMode::Utf8 && source[result.consumed]<128 && source[result.consumed]!=13 && source[result.consumed]!=26){
            const auto first=source.begin()+static_cast<std::ptrdiff_t>(result.consumed);
            const auto last=std::find_if(first,source.end(),[](auto value){return value>=128 || value==13 || value==26;});
            const auto count=std::min<std::size_t>(static_cast<std::size_t>(last-first),(capacity-result.bytes.size())/2);
            if(count){
                if(!result.consumed)result.read_newline=*first==10;
                const auto units=utf8_to_utf16(std::string_view(reinterpret_cast<const char*>(source.data()+result.consumed),count));
                if constexpr(std::endian::native==std::endian::little){const auto at=result.bytes.size();result.bytes.resize(at+count*2);std::memcpy(result.bytes.data()+at,units.data(),count*2);}
                else for(auto value:units)append_unit(result.bytes,value);
                result.consumed+=count;continue;
            }
        }
        std::u16string value;std::size_t length=0;const auto at=result.consumed;
        if(!character(at,value,length))break;
        if(!at)result.read_newline=value[0]==10;
        if(value[0]==26){result.consumed+=length;result.eof=true;break;}
        if(value[0]==13){
            const auto next=at+length;
            if(next==source.size() && !final){result.needed=next+(mode==TextMode::Utf16?2U:1U);break;}
            if(next<source.size()){
                const auto after=mode==TextMode::Utf16?2U:1U;
                if(after>source.size()-next && !final){result.needed=next+after;break;}
                if(after<=source.size()-next && (mode==TextMode::Utf16?unit(source,next):source[next])==10){value.assign(1,10);length+=after;}
            }
        }
        const auto bytes=mode==TextMode::Ansi?1U:value.size()*2;
        if(bytes>capacity-result.bytes.size()){
            if(value.size()!=2 || capacity-result.bytes.size()!=2)break;
            append_unit(result.bytes,value[0]);result.low_surrogate=value[1];
        }else if(mode==TextMode::Ansi)result.bytes.push_back(static_cast<std::uint8_t>(value[0]));
        else for(auto word:value)append_unit(result.bytes,word);
        result.consumed+=length;
    }return result;
}
}
