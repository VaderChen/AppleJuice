#include "icu_spoof_data.hpp"
#include <unicode/uspoof.h>
#include <unicode/ustring.h>
#include <array>
#include <map>
#include <string>

namespace x64arm::icu_spoof {
namespace {
constexpr std::uint32_t signature=0x3845fdef;
constexpr std::uint32_t version=2;
using Mappings=std::map<std::uint32_t,std::u16string>;

std::uint32_t word(std::span<const std::uint8_t> bytes,std::size_t at,unsigned width){
    std::uint32_t result{};
    for(unsigned i=0;i<width;++i)result|=std::uint32_t(bytes[at+i])<<(i*8);
    return result;
}

void store(Data& bytes,std::size_t at,std::uint32_t value,unsigned width){
    for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));
}

std::size_t align(std::size_t count){return (count+15)&~std::size_t{15};}

Data write_data(const Mappings& values,UErrorCode& status){
    const auto keys=header_size;
    const auto indices=align(keys+4*values.size());
    const auto strings=align(indices+2*values.size());
    Data bytes(strings);
    std::map<std::u16string_view,std::size_t,std::less<>> interned;
    std::size_t row{};
    for(const auto& [code,text]:values){
        store(bytes,keys+4*row,code|(static_cast<std::uint32_t>(text.size()-1)<<24),4);
        std::uint32_t value=text.front();
        if(text.size()>1){
            const auto [item,inserted]=interned.try_emplace(text,(bytes.size()-strings)/2);
            if(item->second>UINT16_MAX){status=U_INDEX_OUTOFBOUNDS_ERROR;return {};}
            value=static_cast<std::uint32_t>(item->second);
            if(inserted)for(const auto unit:text){
                bytes.push_back(static_cast<std::uint8_t>(unit));
                bytes.push_back(static_cast<std::uint8_t>(unit>>8));
            }
        }
        store(bytes,indices+2*row,value,2);
        ++row;
    }
    const auto string_count=(bytes.size()-strings)/2;
    if(!values.empty())bytes.resize(align(bytes.size()+2));
    const auto count=static_cast<std::uint32_t>(values.size());
    const std::array header{signature,version,static_cast<std::uint32_t>(bytes.size()),
        static_cast<std::uint32_t>(keys),count,static_cast<std::uint32_t>(indices),count,
        static_cast<std::uint32_t>(strings),static_cast<std::uint32_t>(string_count)};
    for(std::size_t i=0;i<header.size();++i)store(bytes,4*i,header[i],4);
    return bytes;
}

bool space(char16_t unit){return unit==u' ' || unit==u'\t';}

std::u16string_view trim(std::u16string_view text){
    while(!text.empty() && space(text.front()))text.remove_prefix(1);
    while(!text.empty() && space(text.back()))text.remove_suffix(1);
    return text;
}

bool parse_code(std::u16string_view text,std::uint32_t& result){
    if(text.empty())return false;
    result=0;
    for(auto unit:text){
        if(unit>=u'a' && unit<=u'f')unit=static_cast<char16_t>(unit-u'a'+u'A');
        if(result>0x10fff)return false;
        if(unit>=u'0' && unit<=u'9')result=16*result+static_cast<unsigned>(unit-u'0');
        else if(unit>=u'A' && unit<=u'F')result=16*result+static_cast<unsigned>(unit-u'A')+10;
        else return false;
    }
    return result<=0x10ffff;
}

bool parse_line(std::u16string_view line,Mappings& values){
    line=trim(line.substr(0,line.find(u'#')));
    if(line.empty())return true;
    const auto first=line.find(u';');
    if(first==line.npos)return false;
    const auto second=line.find(u';',first+1);
    if(second==line.npos)return false;
    const auto kind=trim(line.substr(second+1));
    if(kind!=u"MA" && kind!=u"ML" && kind!=u"SA" && kind!=u"SL")return false;
    std::uint32_t source{};
    if(!parse_code(trim(line.substr(0,first)),source))return false;
    auto targets=trim(line.substr(first+1,second-first-1));
    std::u16string text;
    while(!targets.empty()){
        const auto boundary=targets.find_first_of(u" \t");
        std::uint32_t target{};
        if(!parse_code(targets.substr(0,boundary),target))return false;
        if(target>0xffff){
            text.push_back(static_cast<char16_t>(0xd800+((target-0x10000)>>10)));
            text.push_back(static_cast<char16_t>(0xdc00+(target&0x3ff)));
        }else text.push_back(static_cast<char16_t>(target));
        if(text.size()>256)return false;
        if(boundary==targets.npos)break;
        targets=trim(targets.substr(boundary));
    }
    if(text.empty())return false;
    values.insert_or_assign(source,std::move(text));
    return true;
}
}

std::size_t serialized_size(std::span<const std::uint8_t> header,std::size_t capacity){
    if(header.size()<header_size || word(header,0,4)!=signature || word(header,4,4)!=version)return 0;
    const auto size=word(header,8,4);
    return size>=header_size && size<=capacity?size:0;
}

bool valid_data(std::span<const std::uint8_t> bytes){
    const auto size=serialized_size(bytes,bytes.size());
    if(!size || size!=bytes.size())return false;
    const auto keys=word(bytes,12,4),count=word(bytes,16,4);
    const auto values=word(bytes,20,4),values_count=word(bytes,24,4);
    const auto strings=word(bytes,28,4),strings_count=word(bytes,32,4);
    const auto section=[&](std::size_t offset,std::size_t length,std::size_t width){
        if(!offset && !length)return true;
        return offset>=header_size && offset<=size && offset%width==0 && length<=(size-offset)/width;
    };
    if(count>0x110000 || count!=values_count || !section(keys,count,4) ||
       !section(values,values_count,2) || !section(strings,strings_count,2))return false;
    std::uint32_t previous{};
    for(std::size_t index=0;index<count;++index){
        const auto key=word(bytes,keys+4*index,4);
        const auto code=key&0xffffffU,length=(key>>24)+1;
        if(code>0x10ffff || (index && code<=previous))return false;
        previous=code;
        if(length>1){
            const auto value=word(bytes,values+2*index,2);
            if(value>=strings_count || length>strings_count-value)return false;
        }
    }
    return true;
}

bool empty_data(std::span<const std::uint8_t> bytes){return word(bytes,16,4)==0;}

Data identity_data(){
    auto status=U_ZERO_ERROR;
    return write_data({{0,std::u16string(1,0)}},status);
}

Data compile_source(std::string_view source,std::int32_t& error_type,UParseError& error,UErrorCode& status){
    if(U_FAILURE(status))return {};
    error_type=0;
    error.line=error.offset=0;
    error.preContext[0]=error.postContext[0]=0;
    if(source.size()>=INT32_MAX){status=U_ILLEGAL_ARGUMENT_ERROR;return {};}
    std::u16string input(source.size(),0);
    std::int32_t length{};
    auto to_utf16=U_ZERO_ERROR;
    if(!source.empty())u_strFromUTF8(input.data(),static_cast<std::int32_t>(input.size()),&length,
                                    source.data(),static_cast<std::int32_t>(source.size()),&to_utf16);
    if(U_FAILURE(to_utf16)){status=to_utf16;error_type=USPOOF_SINGLE_SCRIPT_CONFUSABLE;return {};}
    auto text=std::u16string_view(input.data(),static_cast<std::size_t>(length));
    if(!text.empty() && text.front()==0xfeff)text.remove_prefix(1);
    Mappings mappings;
    for(std::int32_t line=1;!text.empty();++line){
        const auto end=text.find_first_of(u"\r\n\x85\u2028\u2029");
        if(!parse_line(text.substr(0,end),mappings)){
            error_type=USPOOF_SINGLE_SCRIPT_CONFUSABLE;
            error.line=line;
            status=U_PARSE_ERROR;
            return {};
        }
        if(end==text.npos)break;
        const auto newline=text[end];
        text.remove_prefix(end+1);
        if(newline==u'\r' && !text.empty() && text.front()==u'\n')text.remove_prefix(1);
    }
    auto data=write_data(mappings,status);
    if(U_FAILURE(status))error_type=USPOOF_SINGLE_SCRIPT_CONFUSABLE;
    return data;
}
}
