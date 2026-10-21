#pragma once
#include "x64arm/crypto.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace x64arm::crypto_text {
inline bool whitespace(char16_t c){return c==u' ' || c==u'\t' || c==u'\r' || c==u'\n';}
inline int hex_digit(char16_t c){return c>=u'0' && c<=u'9'?c-u'0':c>=u'a' && c<=u'f'?c-u'a'+10:c>=u'A' && c<=u'F'?c-u'A'+10:-1;}
inline bool is_base64(unsigned format){return format==0 || format==1 || format==3 || format==9 || format==13;}
inline std::string encode(std::span<const std::uint8_t> bytes,std::uint32_t flags){
    const auto format=flags&0xfffffffU;
    if(flags&0x10000000U)throw std::invalid_argument("編碼旗標不合法");
    const std::string_view eol=flags&0x40000000U?"":flags&0x80000000U?"\n":"\r\n";
    std::string output;
    if(is_base64(format)){
        auto text=crypto_base64_encode(bytes);
        if(format==13){std::replace(text.begin(),text.end(),'+','-');std::replace(text.begin(),text.end(),'/','_');}
        const std::string_view label=format==0?"CERTIFICATE":format==3?"NEW CERTIFICATE REQUEST":format==9?"X509 CRL":"";
        if(!label.empty()){output="-----BEGIN ";output+=label;output+="-----";output+=eol;}
        for(std::size_t start=0;start<text.size();start+=64){output.append(text,start,std::min<std::size_t>(64,text.size()-start));output+=eol;}
        if(text.empty())output+=eol;
        if(!label.empty()){output+="-----END ";output+=label;output+="-----";output+=eol;}
        return output;
    }
    if(format!=4 && format!=5 && format!=10 && format!=11 && format!=12)throw std::invalid_argument("編碼格式不合法");
    constexpr std::string_view digits="0123456789abcdef";
    if(format==5 || format==10 || format==11){
        for(std::size_t start=0;start<bytes.size();start+=16){
            if(format!=5){std::string address;auto value=start;do{address+=digits[value&15];value>>=4;}while(value);while(address.size()<4)address+='0';std::reverse(address.begin(),address.end());output+=address;output+="    ";}
            const auto count=std::min<std::size_t>(16,bytes.size()-start);std::string line;
            for(std::size_t j=0;j<count;++j){if(j){line+=' ';if(j==8)line+=' ';}line+=digits[bytes[start+j]>>4];line+=digits[bytes[start+j]&15];}
            if(format!=10){line.resize(51,' ');for(std::size_t j=0;j<count;++j){const auto c=bytes[start+j];line+=c>=32 && c<=126?static_cast<char>(c):'.';}}
            output+=line;output+=eol;
        }
        if(bytes.empty())output+=eol;
        return output;
    }
    for(std::size_t i=0;i<bytes.size();++i){
        if(i && format==4){if(i%16==0 && !eol.empty())output+=eol;else {output+=' ';if(i%8==0 && !eol.empty())output+=' ';}}
        output+=digits[bytes[i]>>4];output+=digits[bytes[i]&15];
    }
    output+=eol;return output;
}
struct Decoded {std::vector<std::uint8_t> bytes;std::uint32_t skip{},format{};};
inline std::optional<Decoded> formatted_hex(std::u16string_view text,unsigned format){
    Decoded result;result.format=format;const bool address=format==10 || format==11,ascii=format==5 || format==11;
    while(!text.empty()){
        while(!text.empty() && (text.front()==u'\r' || text.front()==u'\n'))text.remove_prefix(1);
        if(text.empty())break;
        std::size_t prefix=0;
        if(address){while(prefix<text.size() && hex_digit(text[prefix])>=0)++prefix;if(prefix<4 || prefix>8 || text.substr(prefix,4)!=u"    ")return std::nullopt;prefix+=4;}
        const auto end=text.find_first_of(u"\r\n");
        auto line=text.substr(0,end);const auto record=prefix+(ascii?67U:48U);
        if(line.size()>record)line=line.substr(0,record);
        if(line.size()<prefix || (ascii && line.size()<prefix+51))return std::nullopt;
        auto data=line.substr(prefix,ascii?48:line.size()-prefix);
        int high=-1;std::size_t count=0;
        for(const auto c:data){if(c==u' ' || c==u'\t'){if(high>=0)return std::nullopt;continue;}const auto digit=hex_digit(c);if(digit<0)return std::nullopt;if(high<0)high=digit;else {result.bytes.push_back(static_cast<std::uint8_t>(16*high+digit));high=-1;++count;}}
        if(high>=0 || count>16 || (ascii && line.size()!=prefix+51+count))return std::nullopt;
        text.remove_prefix(line.size());
    }
    return result;
}
inline std::optional<Decoded> base64(std::u16string_view text,unsigned format,bool strict){
    Decoded result;result.format=format;
    if(format==0 || format==3 || format==9){
        const auto begin=text.find(u"-----BEGIN ");if(begin==text.npos)return std::nullopt;
        const auto header=text.find(u"-----",begin+11);if(header==text.npos)return std::nullopt;
        const auto end=text.find(u"-----END ",header+5);if(end==text.npos || text.find(u"-----",end+9)==text.npos)return std::nullopt;
        result.skip=static_cast<std::uint32_t>(begin);text=text.substr(header+5,end-header-5);
    }
    std::string plain;unsigned padding=0;bool tail=false;
    for(const auto c:text){
        if(whitespace(c))continue;
        if(c==u'='){tail=true;++padding;continue;}
        if(tail){if(strict)return std::nullopt;break;}
        if(c>127)return std::nullopt;
        auto value=static_cast<char>(c);
        if(format==13){if(value=='-')value='+';if(value=='_')value='/';}
        plain+=value;
    }
    if((strict && padding>3) || plain.size()%4==1)return std::nullopt;
    while(plain.size()%4)plain+='=';
    try{result.bytes=crypto_base64_decode(plain);}catch(const std::invalid_argument&){return std::nullopt;}
    return result;
}
inline std::optional<Decoded> decode(std::u16string_view text,std::uint32_t flags){
    const auto format=flags&0xfffffffU;const bool strict=flags&0x20000000U;
    if(flags&0xd0000000U)return std::nullopt;
    if(is_base64(format))return base64(text,format,strict);
    if(format==6 || format==7){
        if(auto result=base64(text,0,strict))return result;
        if(auto result=base64(text,1,strict))return result;
        if(format==6)return std::nullopt;
        return Decoded{{},0,2}; // 二進位轉換由呼叫端保留原始 A/W 位元組。
    }
    if(format==2)return Decoded{{},0,2};
    if(format==5 || format==10 || format==11)return formatted_hex(text,format);
    if(format!=4 && format!=8 && format!=12)return std::nullopt;
    if(format==8){for(const auto candidate:{10U,11U})if(auto parsed=formatted_hex(text,candidate))return parsed;}
    Decoded result;result.format=format==8?4:format;
    int high=-1;
    for(const auto c:text){
        if(whitespace(c) || c==u',' || c==u'-'){if(high>=0)return std::nullopt;continue;}
        const auto digit=hex_digit(c);if(digit<0)return format==8?formatted_hex(text,5):std::nullopt;
        if(high<0)high=digit;else {result.bytes.push_back(static_cast<std::uint8_t>(16*high+digit));high=-1;}
    }
    if(high>=0)return std::nullopt;
    return result;
}
}
