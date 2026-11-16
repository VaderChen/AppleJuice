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
// SID 字串別名（MS-DTYP 2.5.1.1）；解析與輸出共用同一份表。網域相對別名需要電腦帳號網域 SID（S-1-5-21-x-y-z）。
struct SddlAlias {std::string_view alias,sid;};
inline constexpr SddlAlias sddl_aliases[]{
    {"AA","S-1-5-32-579"},{"AC","S-1-15-2-1"},{"AN","S-1-5-7"},{"AO","S-1-5-32-548"},{"AU","S-1-5-11"},{"BA","S-1-5-32-544"},{"BG","S-1-5-32-546"},
    {"BO","S-1-5-32-551"},{"BU","S-1-5-32-545"},{"CD","S-1-5-32-574"},{"CG","S-1-3-1"},{"CO","S-1-3-0"},{"CY","S-1-5-32-569"},{"ED","S-1-5-9"},
    {"ER","S-1-5-32-573"},{"ES","S-1-5-32-576"},{"HA","S-1-5-32-578"},{"HI","S-1-16-12288"},{"IS","S-1-5-32-568"},{"IU","S-1-5-4"},{"LS","S-1-5-19"},
    {"LU","S-1-5-32-559"},{"LW","S-1-16-4096"},{"ME","S-1-16-8192"},{"MP","S-1-16-8448"},{"MU","S-1-5-32-558"},{"NO","S-1-5-32-556"},{"NS","S-1-5-20"},
    {"NU","S-1-5-2"},{"OW","S-1-3-4"},{"PO","S-1-5-32-550"},{"PS","S-1-5-10"},{"PU","S-1-5-32-547"},{"RA","S-1-5-32-575"},{"RC","S-1-5-12"},
    {"RD","S-1-5-32-555"},{"RE","S-1-5-32-552"},{"RM","S-1-5-32-580"},{"RU","S-1-5-32-554"},{"SI","S-1-16-16384"},{"SO","S-1-5-32-549"},{"SS","S-1-18-2"},
    {"SU","S-1-5-6"},{"SY","S-1-5-18"},{"UD","S-1-5-84-0-0-0-0-0"},{"WD","S-1-1-0"},{"WR","S-1-5-33"}};
inline constexpr std::pair<std::string_view,std::uint32_t> sddl_domain_aliases[]{
    {"LA",500},{"LG",501},{"DA",512},{"DU",513},{"DG",514},{"DC",515},{"DD",516},{"CA",517},{"SA",518},{"EA",519},{"PA",520},{"RO",498},
    {"CN",522},{"AP",525},{"KA",526},{"EK",527},{"RS",553}};
inline std::vector<std::uint8_t> sddl_sid(std::string_view text,std::span<const std::uint8_t> domain={}){
    for(const auto& item:sddl_aliases)if(item.alias==text){text=item.sid;break;}
    if(text.size()==2 && domain.size()==24)for(const auto& [alias,rid]:sddl_domain_aliases)if(alias==text){
        std::vector<std::uint8_t> sid(domain.begin(),domain.end());sid[1]=5;sid.resize(28);sddl_put(sid,24,rid);return sid;
    }
    if(!text.starts_with("S-1-"))throw SddlError{1337};text.remove_prefix(4);
    const auto separator=text.find('-');const auto authority=sddl_number(text.substr(0,separator),0xffffffffffffULL);
    std::vector<std::uint8_t> sid(8);sid[0]=1;for(unsigned i=0;i<6;++i)sid[2+i]=static_cast<std::uint8_t>(authority>>(8*(5-i)));
    if(separator!=text.npos){text.remove_prefix(separator+1);for(;;){const auto end=text.find('-');const auto value=sddl_number(text.substr(0,end),UINT32_MAX);if(sid[1]==15)throw SddlError{1337};++sid[1];const auto offset=sid.size();sid.resize(offset+4);sddl_put(sid,offset,static_cast<std::uint32_t>(value));if(end==text.npos)break;text.remove_prefix(end+1);}}
    return sid;
}
inline std::uint32_t sddl_word(std::span<const std::uint8_t> bytes,std::size_t at){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{bytes[at+i]}<<(8*i);return value;}
// 數字 SID 字串；識別碼授權超過 32 位元時以 0x 十六進位表示。
inline std::string sddl_sid_string(std::span<const std::uint8_t> sid){
    std::uint64_t authority=0;for(unsigned i=0;i<6;++i)authority=(authority<<8)|sid[2+i];std::string result="S-1-";
    if(authority<=UINT32_MAX)result+=std::to_string(authority);
    else {constexpr char digits[]="0123456789abcdef";std::string hex;for(int shift=44;shift>=0;shift-=4)hex+=digits[(authority>>shift)&15U];result+="0x"+hex;}
    for(unsigned i=0;i<sid[1];++i)result+="-"+std::to_string(sddl_word(sid,8+i*4));return result;
}
inline std::string sddl_sid_text(std::span<const std::uint8_t> sid,std::span<const std::uint8_t> domain={}){
    for(const auto& item:sddl_aliases){const auto known=sddl_sid(item.sid);if(std::equal(known.begin(),known.end(),sid.begin(),sid.end()))return std::string(item.alias);}
    if(domain.size()==24 && sid.size()==28 && sid[1]==5 && std::equal(domain.begin()+2,domain.end(),sid.begin()+2)){
        const auto rid=sddl_word(sid,24);for(const auto& [alias,value]:sddl_domain_aliases)if(value==rid)return std::string(alias);
    }
    return sddl_sid_string(sid);
}
// SDDL 的 GUID 不含大括號：xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx（前三段為小端序整數）。
inline security::Guid sddl_guid(std::string_view text){
    if(text.size()!=36 || text[8]!='-' || text[13]!='-' || text[18]!='-' || text[23]!='-')throw SddlError{1338};
    security::Guid guid{};static constexpr std::array<unsigned,16> order{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    std::size_t at=0;for(unsigned i=0;i<16;++i){if(text[at]=='-')++at;unsigned value=0;
        for(unsigned j=0;j<2;++j){const char c=text[at++];const int digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(digit<0)throw SddlError{1338};value=value*16+static_cast<unsigned>(digit);}
        guid[order[i]]=static_cast<std::uint8_t>(value);}
    return guid;
}
inline std::string sddl_guid_text(const security::Guid& guid){
    constexpr char digits[]="0123456789abcdef";static constexpr std::array<unsigned,16> order{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};std::string text;
    for(unsigned i=0;i<16;++i){if(i==4 || i==6 || i==8 || i==10)text+='-';text+=digits[guid[order[i]]>>4];text+=digits[guid[order[i]]&15U];}
    return text;
}
inline std::uint32_t sddl_tokens(std::string_view text,const std::map<std::string_view,std::uint32_t>& table){std::uint32_t value=0;if(text.size()%2)throw SddlError{1338};while(!text.empty()){const auto found=table.find(text.substr(0,2));if(found==table.end())throw SddlError{1338};value|=found->second;text.remove_prefix(2);}return value;}
inline std::vector<std::uint8_t> sddl_descriptor(std::string_view input,std::span<const std::uint8_t> domain={}){
    static const std::map<std::string_view,std::uint32_t> flags{{"OI",1},{"CI",2},{"NP",4},{"IO",8},{"ID",16},{"SA",64},{"FA",128}};
    static const std::map<std::string_view,std::uint32_t> rights{{"GA",0x10000000},{"GR",0x80000000},{"GW",0x40000000},{"GX",0x20000000},{"RC",0x20000},{"SD",0x10000},{"WD",0x40000},{"WO",0x80000},{"FA",0x1f01ff},{"FR",0x120089},{"FW",0x120116},{"FX",0x1200a0},{"KA",0xf003f},{"KR",0x20019},{"KW",0x20006},{"KX",0x20019},{"NW",1},{"NR",2},{"NX",4},{"CC",1},{"DC",2},{"LC",4},{"SW",8},{"RP",16},{"WP",32},{"DT",64},{"LO",128},{"CR",256}};
    if(input.size()>65535)throw SddlError{1338};std::string compact;for(char c:input)if(c!=' ' && c!='\t' && c!='\r' && c!='\n')compact+=c;input=compact;
    std::vector<std::uint8_t> result(20);result[0]=1;std::uint32_t control=0x8000,seen=0;
    while(!input.empty()){
        if(input.size()<2 || input[1]!=':')throw SddlError{1338};const char part=input[0];const unsigned field=part=='O'?4:part=='G'?8:part=='S'?12:part=='D'?16:0;
        if(!field || (seen&(1U<<field)))throw SddlError{1338};seen|=1U<<field;input.remove_prefix(2);
        std::size_t end=0;unsigned depth=0;for(;end<input.size();++end){if(!depth && end+1<input.size() && input[end+1]==':')break;if(input[end]=='(')++depth;else if(input[end]==')'){if(!depth)throw SddlError{1338};--depth;}}
        if(depth)throw SddlError{1338};auto value=input.substr(0,end);input.remove_prefix(end);std::vector<std::uint8_t> bytes;
        if(part=='O' || part=='G')bytes=sddl_sid(value,domain);
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
            bytes.resize(8);bytes[0]=2;unsigned count=0;bool object_aces=false;value=first==value.npos?std::string_view{}:value.substr(first);
            while(!value.empty()){
                if(value[0]!='(')throw SddlError{1338};const auto close=value.find(')');if(close==value.npos)throw SddlError{1338};auto ace_text=value.substr(1,close-1);value.remove_prefix(close+1);
                std::array<std::string_view,6> fields;for(unsigned i=0;i<6;++i){const auto split=ace_text.find(';');if((i<5)==(split==ace_text.npos))throw SddlError{1338};fields[i]=ace_text.substr(0,split);if(i<5)ace_text.remove_prefix(split+1);}
                const auto type=fields[0]=="A"?0:fields[0]=="D"?1:fields[0]=="AU"?2:fields[0]=="AL"?3:fields[0]=="OA"?5:fields[0]=="OD"?6:fields[0]=="OU"?7:fields[0]=="OL"?8:fields[0]=="ML"?17:-1;
                if(type<0 || (!security::object_ace_type(static_cast<unsigned>(type)) && (!fields[3].empty() || !fields[4].empty())))throw SddlError{50};
                security::ExplicitAce ace;ace.type=static_cast<std::uint8_t>(type);ace.flags=static_cast<std::uint8_t>(sddl_tokens(fields[1],flags));ace.sid=sddl_sid(fields[5],domain);
                ace.mask=fields[2].starts_with("0x") || fields[2].starts_with("0X")?static_cast<std::uint32_t>(sddl_number(fields[2],UINT32_MAX)):sddl_tokens(fields[2],rights);
                // 物件 ACE 的第 4、5 欄是 ObjectType 與 InheritedObjectType GUID，空白表示不帶該欄位。
                if(security::object_ace_type(ace.type)){security::AceObjects objects;if(!fields[3].empty()){objects.flags|=1;objects.type=sddl_guid(fields[3]);}
                    if(!fields[4].empty()){objects.flags|=2;objects.inherited=sddl_guid(fields[4]);}ace.objects=objects;object_aces=true;}
                const auto encoded=security::encode_ace(ace);if(bytes.size()+encoded.size()>65535)throw SddlError{1338};bytes.insert(bytes.end(),encoded.begin(),encoded.end());++count;
            }
            if(object_aces)bytes[0]=4; // 含物件 ACE 的 ACL 版本為 ACL_REVISION_DS。
            sddl_put(bytes,2,static_cast<std::uint32_t>(bytes.size()),2);sddl_put(bytes,4,count,2);
        }
        sddl_put(result,field,static_cast<std::uint32_t>(result.size()));result.insert(result.end(),bytes.begin(),bytes.end());
    }
    sddl_put(result,2,control,2);return result;
}
// 安全描述元轉 SDDL：O、G、D、S 依序輸出；權限先比對完整的檔案／登錄組合，否則依位元由低到高拆成單一權限，
// 有無法表示的位元時改用十六進位。information 為 SECURITY_INFORMATION（LABEL 只輸出 SACL 中的完整性標籤）。
inline std::string sddl_text(const security::SecurityDescriptor& sd,std::uint32_t information,std::span<const std::uint8_t> domain={}){
    static constexpr std::pair<std::string_view,std::uint32_t> combined[]{{"FA",0x1f01ff},{"FR",0x120089},{"FW",0x120116},{"FX",0x1200a0},{"KA",0xf003f},{"KR",0x20019},{"KW",0x20006}};
    static constexpr std::pair<std::string_view,std::uint32_t> single[]{{"CC",1},{"DC",2},{"LC",4},{"SW",8},{"RP",16},{"WP",32},{"DT",64},{"LO",128},{"CR",256},
        {"SD",0x10000},{"RC",0x20000},{"WD",0x40000},{"WO",0x80000},{"GA",0x10000000},{"GX",0x20000000},{"GW",0x40000000},{"GR",0x80000000}};
    static constexpr std::pair<std::string_view,std::uint32_t> label[]{{"NW",1},{"NR",2},{"NX",4}};
    static constexpr std::pair<std::string_view,std::uint32_t> ace_flags[]{{"OI",1},{"CI",2},{"NP",4},{"IO",8},{"ID",16},{"SA",64},{"FA",128}};
    const auto hex=[](std::uint32_t value){constexpr char digits[]="0123456789abcdef";std::string text;do{text.insert(text.begin(),digits[value&15U]);value>>=4;}while(value);return "0x"+text;};
    const auto rights=[&](std::uint32_t mask,bool mandatory){
        if(!mask)return std::string{};
        if(!mandatory)for(const auto& [token,value]:combined)if(value==mask)return std::string(token);
        std::string text;auto rest=mask;
        for(const auto& [token,value]:mandatory?std::span<const std::pair<std::string_view,std::uint32_t>>(label):std::span<const std::pair<std::string_view,std::uint32_t>>(single))if(mask&value){text+=token;rest&=~value;}
        return rest?hex(mask):text;
    };
    const auto acl_text=[&](unsigned field,char tag){
        const std::uint16_t present=field==3?4:16,protect=field==3?0x1000:0x2000,required=field==3?0x100:0x200,automatic=field==3?0x400:0x800;
        std::string text(1,tag);text+=':';
        if(sd.control&protect)text+='P';if(sd.control&required)text+="AR";if(sd.control&automatic)text+="AI";
        if((sd.control&present) && sd.data[field].empty())return text+"NO_ACCESS_CONTROL";
        for(const auto& bytes:security::acl_entries(sd.data[field])){
            const auto ace=security::read_ace(bytes);
            if(field==2 && !(information&8U) && ace.type!=0x11)continue;
            static constexpr std::string_view types[]{"A","D","AU","AL","","OA","OD","OU","OL"};
            const auto type=ace.type<=8?types[ace.type]:ace.type==0x11?std::string_view{"ML"}:ace.type==0x13?std::string_view{"SP"}:std::string_view{"TL"};
            if(type.empty())throw SddlError{50};
            text+='(';text+=type;text+=';';for(const auto& [token,value]:ace_flags)if(ace.flags&value)text+=token;
            text+=';'+rights(ace.mask,ace.type==0x11)+';';
            if(ace.objects && (ace.objects->flags&1U))text+=sddl_guid_text(ace.objects->type);text+=';';
            if(ace.objects && (ace.objects->flags&2U))text+=sddl_guid_text(ace.objects->inherited);
            text+=';'+sddl_sid_text(ace.sid,domain)+')';
        }
        return text;
    };
    std::string text;
    if((information&1U) && !sd.data[0].empty())text+="O:"+sddl_sid_text(sd.data[0],domain);
    if((information&2U) && !sd.data[1].empty())text+="G:"+sddl_sid_text(sd.data[1],domain);
    if((information&4U) && (sd.control&4U))text+=acl_text(3,'D');
    if((information&0x18U) && (sd.control&16U))text+=acl_text(2,'S');
    return text;
}
}
