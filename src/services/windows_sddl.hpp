#pragma once
#include "x64arm/security_descriptor.hpp"
#include <array>
#include <algorithm>
#include <charconv>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
namespace x64arm {
// SDDL 轉換只產生描述元，不代表授予權限；存取檢查由物件後端執行。
using SddlError = SecurityDescriptorError;
inline void sddl_put(std::vector<std::uint8_t>& bytes,std::size_t offset,std::uint32_t value,unsigned width=4){for(unsigned i=0;i<width;++i)bytes.at(offset+i)=static_cast<std::uint8_t>(value>>(8*i));}
inline std::uint64_t sddl_number(std::string_view text,std::uint64_t maximum){
    std::uint64_t value{};int base=10;if(text.starts_with("0x") || text.starts_with("0X")){text.remove_prefix(2);base=16;}
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value,base);
    if(text.empty() || parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || value>maximum)throw SddlError{1337};return value;
}
inline std::vector<std::uint8_t> sddl_sid(std::string_view text){
    static const std::map<std::string_view,std::string_view> aliases{{"WD","S-1-1-0"},{"SY","S-1-5-18"},{"LS","S-1-5-19"},{"NS","S-1-5-20"},{"AU","S-1-5-11"},{"AN","S-1-5-7"},{"IU","S-1-5-4"},{"SU","S-1-5-6"},{"BA","S-1-5-32-544"},{"BU","S-1-5-32-545"},{"BG","S-1-5-32-546"},{"CO","S-1-3-0"},{"CG","S-1-3-1"},{"OW","S-1-3-4"},{"LW","S-1-16-4096"},{"ME","S-1-16-8192"},{"HI","S-1-16-12288"},{"SI","S-1-16-16384"},{"AC","S-1-15-2-1"}};
    if(const auto found=aliases.find(text);found!=aliases.end())text=found->second;
    if(!text.starts_with("S-1-"))throw SddlError{1337};text.remove_prefix(4);
    const auto separator=text.find('-');const auto authority=sddl_number(text.substr(0,separator),0xffffffffffffULL);
    std::vector<std::uint8_t> sid(8);sid[0]=1;for(unsigned i=0;i<6;++i)sid[2+i]=static_cast<std::uint8_t>(authority>>(8*(5-i)));
    if(separator!=text.npos){text.remove_prefix(separator+1);for(;;){const auto end=text.find('-');const auto value=sddl_number(text.substr(0,end),UINT32_MAX);if(sid[1]==15)throw SddlError{1337};++sid[1];const auto offset=sid.size();sid.resize(offset+4);sddl_put(sid,offset,static_cast<std::uint32_t>(value));if(end==text.npos)break;text.remove_prefix(end+1);}}
    return sid;
}
inline std::uint32_t sddl_tokens(std::string_view text,const std::map<std::string_view,std::uint32_t>& table){std::uint32_t value=0;if(text.size()%2)throw SddlError{1338};while(!text.empty()){const auto found=table.find(text.substr(0,2));if(found==table.end())throw SddlError{1338};value|=found->second;text.remove_prefix(2);}return value;}
inline std::vector<std::uint8_t> sddl_descriptor(std::string_view input){
    static const std::map<std::string_view,std::uint32_t> flags{{"OI",1},{"CI",2},{"NP",4},{"IO",8},{"ID",16},{"SA",64},{"FA",128}};
    static const std::map<std::string_view,std::uint32_t> rights{{"GA",0x10000000},{"GR",0x80000000},{"GW",0x40000000},{"GX",0x20000000},{"RC",0x20000},{"SD",0x10000},{"WD",0x40000},{"WO",0x80000},{"FA",0x1f01ff},{"FR",0x120089},{"FW",0x120116},{"FX",0x1200a0},{"KA",0xf003f},{"KR",0x20019},{"KW",0x20006},{"KX",0x20019},{"NW",1},{"NR",2},{"NX",4},{"CC",1},{"DC",2},{"LC",4},{"SW",8},{"RP",16},{"WP",32},{"DT",64},{"LO",128},{"CR",256}};
    if(input.size()>65535)throw SddlError{1338};std::string compact;for(char c:input)if(c!=' ' && c!='\t' && c!='\r' && c!='\n')compact+=c;input=compact;
    std::vector<std::uint8_t> result(20);result[0]=1;std::uint32_t control=0x8000,seen=0;
    while(!input.empty()){
        if(input.size()<2 || input[1]!=':')throw SddlError{1338};const char part=input[0];const unsigned field=part=='O'?4:part=='G'?8:part=='S'?12:part=='D'?16:0;
        if(!field || (seen&(1U<<field)))throw SddlError{1338};seen|=1U<<field;input.remove_prefix(2);
        std::size_t end=0;unsigned depth=0;for(;end<input.size();++end){if(!depth && end+1<input.size() && input[end+1]==':')break;if(input[end]=='(')++depth;else if(input[end]==')'){if(!depth)throw SddlError{1338};--depth;}}
        if(depth)throw SddlError{1338};auto value=input.substr(0,end);input.remove_prefix(end);std::vector<std::uint8_t> bytes;
        if(part=='O' || part=='G')bytes=sddl_sid(value);
        else {
            control|=part=='D'?4U:16U;const auto first=value.find('(');auto attributes=value.substr(0,first);bool null_acl=false;
            while(!attributes.empty()){
                if(attributes.starts_with("NO_ACCESS_CONTROL")){null_acl=true;attributes.remove_prefix(17);}
                else if(attributes.starts_with("P")){control|=part=='D'?0x1000U:0x2000U;attributes.remove_prefix(1);}
                else if(attributes.starts_with("AR")){control|=part=='D'?0x100U:0x200U;attributes.remove_prefix(2);}
                else if(attributes.starts_with("AI")){control|=part=='D'?0x400U:0x800U;attributes.remove_prefix(2);}
                else throw SddlError{1338};
            }
            if(null_acl){if(first!=value.npos)throw SddlError{1338};continue;}
            bytes.resize(8);bytes[0]=2;unsigned count=0;value=first==value.npos?std::string_view{}:value.substr(first);
            while(!value.empty()){
                if(value[0]!='(')throw SddlError{1338};const auto close=value.find(')');if(close==value.npos)throw SddlError{1338};auto ace=value.substr(1,close-1);value.remove_prefix(close+1);
                std::array<std::string_view,6> fields;for(unsigned i=0;i<6;++i){const auto split=ace.find(';');if((i<5)==(split==ace.npos))throw SddlError{1338};fields[i]=ace.substr(0,split);if(i<5)ace.remove_prefix(split+1);}
                const auto type=fields[0]=="A"?0:fields[0]=="D"?1:fields[0]=="AU"?2:fields[0]=="ML"?17:-1;
                if(type<0 || !fields[3].empty() || !fields[4].empty())throw SddlError{50};
                const auto sid=sddl_sid(fields[5]);const auto mask=fields[2].starts_with("0x") || fields[2].starts_with("0X")?static_cast<std::uint32_t>(sddl_number(fields[2],UINT32_MAX)):sddl_tokens(fields[2],rights);
                const auto offset=bytes.size(),size=8+sid.size();if(offset+size>65535)throw SddlError{1338};bytes.resize(offset+size);bytes[offset]=static_cast<std::uint8_t>(type);bytes[offset+1]=static_cast<std::uint8_t>(sddl_tokens(fields[1],flags));sddl_put(bytes,offset+2,static_cast<std::uint32_t>(size),2);sddl_put(bytes,offset+4,mask);std::copy(sid.begin(),sid.end(),bytes.begin()+static_cast<std::ptrdiff_t>(offset+8));++count;
            }
            sddl_put(bytes,2,static_cast<std::uint32_t>(bytes.size()),2);sddl_put(bytes,4,count,2);
        }
        sddl_put(result,field,static_cast<std::uint32_t>(result.size()));result.insert(result.end(),bytes.begin(),bytes.end());
    }
    sddl_put(result,2,control,2);return result;
}
}
