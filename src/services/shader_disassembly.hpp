#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace x64arm::shader_disassembly {
inline std::string hex(std::uint64_t value,unsigned width){
    constexpr char digits[]="0123456789abcdef";std::string result(width+2,'0');result[1]='x';
    for(unsigned i=0;i<width;++i){result[width+1-i]=digits[value&15U];value>>=4;}return result;
}
inline std::optional<unsigned> sm1_length(std::uint32_t opcode,std::uint32_t version){
    // SM1 沒有 instruction-length 欄位；參數個數由 bytecode 指令契約決定。
    static constexpr std::array<unsigned,25> arithmetic{1,3,4,4,5,4,3,3,4,4,4,4,4,4,3,3,3,4,5,3,4,4,4,4,4};
    if(opcode<arithmetic.size())return arithmetic[opcode];
    if(opcode==0xfffdU)return 1;
    if(opcode==64 || opcode==66)return (version&255U)>=4?3U:2U;
    if(opcode==65 || opcode==87)return 2;
    if((opcode>=67 && opcode<=75) || opcode==77 || opcode==78 || opcode==79 || (opcode>=82 && opcode<=86))return 3;
    if(opcode==76 || opcode==89)return 4;
    if(opcode==80 || opcode==88 || opcode==90)return 5;
    if(opcode==81)return 6;return {};
}
inline std::string_view trim(std::string_view value){
    while(!value.empty() && (value.front()==' ' || value.front()=='\t' || value.front()=='\r'))value.remove_prefix(1);
    while(!value.empty() && (value.back()==' ' || value.back()=='\t' || value.back()=='\r'))value.remove_suffix(1);return value;
}
inline std::optional<std::string> literal_hex(std::string_view value){
    value=trim(value);if(value.empty())return {};if(value.starts_with("0x") || value.starts_with("-0x"))return std::string(value);
    if(value=="true")return hex(1,8);if(value=="false")return hex(0,8);
    const bool double_value=value.ends_with('l'),unsigned_value=value.ends_with('u');if(double_value || unsigned_value)value.remove_suffix(1);
    if(value.find_first_of(".eE") == value.npos && !double_value && value!="inf" && value!="-inf" && value!="nan"){
        std::int64_t integer=0;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),integer);
        if(parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size())return hex(static_cast<std::uint32_t>(integer),8);return {};
    }
    // shader 的數字不使用宿主目前的 LC_NUMERIC，也不影響客體 errno。
    static const auto classic=newlocale(LC_NUMERIC_MASK,"C",nullptr);if(!classic)throw std::bad_alloc();
    const std::string input(value);char* end=nullptr;const auto previous_errno=errno;errno=0;
    const auto number=strtod_l(input.c_str(),&end,classic);const auto failed=errno;errno=previous_errno;
    if(failed || end!=input.c_str()+input.size())return {};
    return double_value?hex(std::bit_cast<std::uint64_t>(number),16):hex(std::bit_cast<std::uint32_t>(static_cast<float>(number)),8);
}
inline std::string hex_literals(std::string_view line){
    const auto content=trim(line);const auto equals=line.find(" = ");
    if(equals!=line.npos && (content.starts_with("def ") || content.starts_with("defi ") || content.starts_with("defb "))){
        std::string result(line.substr(0,equals+3));
        for(std::size_t at=equals+3;at<line.size();){const auto end=line.find(',',at);const auto token=line.substr(at,end==line.npos?line.size()-at:end-at);
            const auto value=literal_hex(token);if(!value)return std::string(line);if(at!=equals+3)result+=", ";result+=*value;if(end==line.npos)break;at=end+1;
        }return result;
    }
    std::string result;std::size_t at=0;
    while(at<line.size()){
        const auto open=line.find('(',at);if(open==line.npos){result+=line.substr(at);break;}
        result+=line.substr(at,open-at+1);const auto close=line.find(')',open+1);if(close==line.npos){result+=line.substr(open+1);break;}
        std::string group;bool converted=true;std::size_t cursor=open+1;
        while(cursor<=close){const auto separator=line.find(',',cursor);const auto last=separator==line.npos || separator>close?close:separator;
            const auto literal=literal_hex(line.substr(cursor,last-cursor));if(!literal){converted=false;break;}if(!group.empty())group+=", ";group+=*literal;
            if(last==close)break;cursor=last+1;
        }
        result+=converted?group:std::string(line.substr(open+1,close-open-1));result+=')';at=close+1;
    }return result;
}
inline std::string html(std::string_view text){
    std::string result="<html><body><pre>";
    for(std::size_t at=0;at<text.size();){const auto end=text.find('\n',at);const auto line=text.substr(at,end==text.npos?text.size()-at:end-at);const auto content=trim(line);
        result+=content.starts_with("//")?"<span style=\"color:#008000\">":"<span style=\"color:#000080\">";
        for(const char c:line){if(c=='&')result+="&amp;";else if(c=='<')result+="&lt;";else if(c=='>')result+="&gt;";else if(c=='\"')result+="&quot;";else result+=c;}
        result+="</span>\n";if(end==text.npos)break;at=end+1;
    }result+="</pre></body></html>";return result;
}
inline std::string format(std::string_view assembly,std::uint32_t flags,std::string_view comments,std::string_view header,std::span<const std::uint64_t> offsets){
    std::string result;if(!(flags&0x40U)){result+=header;
        for(std::size_t at=0;at<comments.size();){const auto end=comments.find('\n',at);result+="// ";result+=comments.substr(at,end==comments.npos?comments.size()-at:end-at);result+='\n';if(end==comments.npos)break;at=end+1;}
    }
    std::size_t instruction=0;bool profile=true;unsigned block_depth=0;
    for(std::size_t at=0;at<assembly.size();){const auto end=assembly.find('\n',at);const auto line=assembly.substr(at,end==assembly.npos?assembly.size()-at:end-at);const auto content=trim(line);
        const bool directive=content.empty() || content.starts_with("//") || content.starts_with('.') || content.starts_with('{') || content.starts_with('}') || content.starts_with('(');
        const bool executable=!directive && !profile && !block_depth;if(!directive && profile)profile=false;
        if(executable){if(flags&4U)result+="/* "+std::to_string(instruction)+" */ ";if((flags&0x20U) && instruction<offsets.size())result+="/* "+hex(offsets[instruction],8)+" */ ";++instruction;}
        if(!(flags&0x40U) || !content.starts_with("//")){result+=flags&0x80U?hex_literals(line):std::string(line);result+='\n';}
        if(!content.starts_with("//"))for(const auto c:content){if(c=='{')++block_depth;else if(c=='}' && block_depth)--block_depth;}
        if(end==assembly.npos)break;at=end+1;
    }
    return flags&1U?html(result):result;
}
}
