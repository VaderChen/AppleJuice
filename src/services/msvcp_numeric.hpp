#pragma once
#include "crt_float_chars.hpp"
#include <array>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <bit>
#include <string>
#include <string_view>
#include <new>
#include <utility>

namespace x64arm::msvcp_detail {
enum class NumberType {Boolean,Signed16,Unsigned16,Signed32,Unsigned32,Signed64,Unsigned64,Floating32,Floating64,Pointer};
struct NumberInput{NumberType type{NumberType::Signed32};std::uint64_t value{};unsigned pointer_bytes{8};};
struct NumberFormat{std::uint32_t flags{};std::int64_t precision{6},width{};char16_t fill{u' '};};
struct NumericPunctuation{char16_t decimal{u'.'},thousands{u','};std::string grouping;std::u16string true_name{u"true"},false_name{u"false"};};
inline std::u16string group_numeric(std::u16string text,std::size_t first,std::size_t last,const NumericPunctuation& punctuation){
    if(punctuation.grouping.empty() || first>=last)return text;
    std::size_t cursor=last,group=0;unsigned amount=0;
    while(cursor>first){
        if(group<punctuation.grouping.size()){
            const auto next=static_cast<unsigned char>(punctuation.grouping[group++]);
            if(next==127 || next>127)break;if(next)amount=next;else group=punctuation.grouping.size();
        }
        if(!amount || cursor-first<=amount)break;cursor-=amount;text.insert(cursor,1,punctuation.thousands);
    }return text;
}
// ctype 的客體 widen 回呼先產生字元；分隔符與 fill 依 numpunct／呼叫參數插入。
inline std::u16string localize_number(std::u16string text,std::u16string_view raw,bool floating,bool boolalpha,NumberFormat format,const NumericPunctuation& punctuation){
    const auto flags=format.flags;std::size_t internal_prefix=0;
    if(!boolalpha){
        const auto first=!raw.empty() && (raw.front()==u'+' || raw.front()==u'-')?1U:0U;
        const auto prefix=raw.size()>=first+2 && raw[first]==u'0' && (raw[first+1]==u'x' || raw[first+1]==u'X')?first+2:first;
        const auto decimal=raw.find(u'.',prefix),exponent=floating?raw.find_first_of((flags&0x3000U)==0x3000U?u"pP":u"eE",prefix):raw.npos;
        internal_prefix=prefix;const auto last=std::min(decimal==raw.npos?raw.size():decimal,exponent==raw.npos?raw.size():exponent);
        if(decimal!=raw.npos)text[decimal]=punctuation.decimal;
        const bool digits=std::all_of(raw.begin()+prefix,raw.begin()+last,[&](char16_t c){return (c>=u'0' && c<=u'9') || (!floating && ((c>=u'a' && c<=u'f') || (c>=u'A' && c<=u'F')));});
        if(digits)text=group_numeric(std::move(text),prefix,last,punctuation);
    }
    if(format.width>1048576)throw std::bad_alloc();
    if(format.width>0 && static_cast<std::uint64_t>(format.width)>text.size()){
        const auto after=(flags&0x1c0U)==0x40U?text.size():(flags&0x1c0U)==0x100U?internal_prefix:0U;
        text.insert(after,static_cast<std::size_t>(format.width)-text.size(),format.fill);
    }return text;
}
// VC10／VC14 及預設 num_put 共用數值語義；寬度以客體字元計，不以宿主 UTF-8 byte 計。
inline std::u16string format_number(NumberInput input,NumberFormat format,const NumericPunctuation& punctuation={}){
    const auto flags=format.flags;const bool boolean=input.type==NumberType::Boolean;
    const bool floating=input.type==NumberType::Floating32 || input.type==NumberType::Floating64;
    const bool pointer=input.type==NumberType::Pointer;std::string narrow;std::u16string text;
    if(boolean && (flags&0x4000U))text=input.value?punctuation.true_name:punctuation.false_name;
    else if(floating){
        const auto number=input.type==NumberType::Floating32?static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(input.value))):std::bit_cast<double>(input.value);
        const auto style=(flags&0x3000U)==0x3000U?std::chars_format::hex:(flags&0x3000U)==0x1000U?std::chars_format::scientific:(flags&0x3000U)==0x2000U?std::chars_format::fixed:std::chars_format::general;
        auto precision=format.precision<0?6:format.precision;if(style==std::chars_format::hex)precision=13;
        if(precision>1048576)throw std::bad_alloc();if(!precision && style==std::chars_format::general)precision=1;
        narrow=crt_float_chars(number,style,static_cast<int>(precision));
        if(style==std::chars_format::hex && std::isfinite(number))narrow.insert(std::signbit(number)?1U:0U,"0x");
        if((flags&0x20U) && !std::signbit(number))narrow.insert(narrow.begin(),'+');
        if((flags&0x10U) && std::isfinite(number)){
            auto stop=narrow.find_first_of("eEpP");if(stop==narrow.npos)stop=narrow.size();
            if(narrow.substr(0,stop).find('.')==narrow.npos){narrow.insert(stop,1,'.');++stop;}
            if(style==std::chars_format::general){
                const auto first=narrow.front()=='+' || narrow.front()=='-'?1U:0U;std::size_t significant=0;bool nonzero=false;
                for(auto i=first;i<stop;++i)if(narrow[i]>='0' && narrow[i]<='9'){nonzero|=narrow[i]!='0';if(nonzero)++significant;}
                if(!significant)significant=1;if(significant<static_cast<std::size_t>(precision))narrow.insert(stop,static_cast<std::size_t>(precision)-significant,'0');
            }
        }
    }else{
        const bool short_value=input.type==NumberType::Signed16 || input.type==NumberType::Unsigned16;
        const bool wide_value=input.type==NumberType::Signed64 || input.type==NumberType::Unsigned64 || pointer;
        const bool signed_value=boolean || input.type==NumberType::Signed16 || input.type==NumberType::Signed32 || input.type==NumberType::Signed64;
        const unsigned width=pointer?input.pointer_bytes*8:wide_value?64:short_value?16:32;
        const auto mask=width==64?UINT64_MAX:(UINT64_C(1)<<width)-1;
        auto number=boolean?std::uint64_t{input.value!=0}:input.value&mask;
        const unsigned base=pointer?16:(flags&0xe00U)==0x800U?16:(flags&0xe00U)==0x400U?8:10;
        const bool negative=signed_value && !boolean && base==10 && (number&(UINT64_C(1)<<(width-1)));
        if(negative)number=(UINT64_C(0)-number)&mask;
        std::array<char,70> buffer{};const auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),number,static_cast<int>(base));narrow.assign(buffer.data(),result.ptr);
        if(pointer)narrow.insert(0,width/4-narrow.size(),'0');
        else if((flags&8U) && number && base!=10)narrow.insert(0,base==16?"0x":"0");
        if(negative)narrow.insert(narrow.begin(),'-');else if((flags&0x20U) && signed_value && base==10)narrow.insert(narrow.begin(),'+');
    }
    if(text.empty() && !(boolean && (flags&0x4000U))){
        if((flags&4U) || pointer)for(auto& c:narrow)if(c>='a' && c<='z')c=static_cast<char>(c-32);
        text.assign(narrow.begin(),narrow.end());
    }
    const auto raw=text;return localize_number(std::move(text),raw,floating,boolean && (flags&0x4000U),format,punctuation);
}
}
