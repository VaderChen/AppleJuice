#include "x64arm/hid.hpp"
#include <algorithm>
#include <bit>
#include <map>
#include <limits>
#include <set>

namespace x64arm::hid {
namespace {
constexpr unsigned limit=1U<<20;
void put(std::span<std::uint8_t> bytes,unsigned at,std::uint64_t value,unsigned size=2){for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));}
std::uint32_t get(std::span<const std::uint8_t> bytes,unsigned at,unsigned size=4){std::uint32_t value{};for(unsigned i=0;i<size;++i)value|=std::uint32_t{bytes[at+i]}<<(i*8);return value;}
std::int32_t signed_value(std::uint32_t value,unsigned size){if(!size)return 0;const unsigned shift=32-size*8;return std::bit_cast<std::int32_t>(value<<shift)>>shift;}
struct Global {unsigned page{},bits{},count{};std::uint8_t report{};std::int32_t minimum{},maximum{},physical_minimum{},physical_maximum{};std::uint32_t unit{},exponent{};};
struct Local {
    std::vector<Usage> usages;Usage minimum{},maximum{};bool have_minimum{},have_maximum{},delimiter{};
    void clear(){*this={};}
    std::vector<Usage> expand()const{
        if(have_minimum!=have_maximum)throw Error(invalid_preparsed);
        auto values=usages;
        if(have_minimum){if(minimum.page!=maximum.page || maximum.value<minimum.value)throw Error(invalid_preparsed);
            if(values.size()+maximum.value-minimum.value+1>65536)throw Error(invalid_preparsed);
            for(unsigned i=minimum.value;i<=maximum.value;++i)values.push_back({minimum.page,static_cast<std::uint16_t>(i)});}
        return values;
    }
};
struct Collection {Usage usage{};unsigned parent{},root{};};
struct ParsedField {Field field;unsigned root{};};
constexpr std::array<std::uint8_t,8> signature{'M','W','H','P','D','0','0','1'};
}
Layout parse(std::span<const std::uint8_t> descriptor,Usage top){
    if(descriptor.empty() || descriptor.size()>limit)throw Error(invalid_preparsed);
    Global global;Local local;std::vector<Global> globals;std::vector<unsigned> stack;
    std::vector<Collection> collections;std::vector<ParsedField> fields;
    std::array<std::array<unsigned,256>,3> offsets{};
    std::array<std::array<bool,256>,3> used{};bool numbered=false;
    std::map<std::pair<unsigned,unsigned>,std::set<unsigned>> report_roots;
    const auto usage=[&](std::uint32_t value,unsigned size){return Usage{static_cast<std::uint16_t>(size==4?value>>16:global.page),static_cast<std::uint16_t>(value)};};
    for(std::size_t cursor=0;cursor<descriptor.size();){
        const auto prefix=descriptor[cursor++];
        if(prefix==0xfe){if(descriptor.size()-cursor<2)throw Error(invalid_preparsed);const auto size=descriptor[cursor];cursor+=2;if(size>descriptor.size()-cursor)throw Error(invalid_preparsed);cursor+=size;continue;}
        unsigned size=prefix&3U;if(size==3)size=4;
        if(size>descriptor.size()-cursor)throw Error(invalid_preparsed);
        const auto number=get(descriptor,static_cast<unsigned>(cursor),size);cursor+=size;
        const unsigned kind=(prefix>>2)&3U,tag=static_cast<unsigned>(prefix)>>4;
        if(kind==1){
            switch(tag){
                case 0:global.page=number;break;
                case 1:global.minimum=signed_value(number,size);break;
                case 2:global.maximum=global.minimum<0?signed_value(number,size):static_cast<std::int32_t>(number);break;
                case 3:global.physical_minimum=signed_value(number,size);break;
                case 4:global.physical_maximum=global.physical_minimum<0?signed_value(number,size):static_cast<std::int32_t>(number);break;
                case 5:global.exponent=std::bit_cast<std::uint32_t>((number&8U)?static_cast<std::int32_t>(number&15U)-16:static_cast<std::int32_t>(number&15U));break;
                case 6:global.unit=number;break;
                case 7:global.bits=number;break;
                case 8:if(!number || number>255)throw Error(invalid_preparsed);global.report=static_cast<std::uint8_t>(number);numbered=true;break;
                case 9:global.count=number;break;
                case 10:if(globals.size()>=64)throw Error(invalid_preparsed);globals.push_back(global);break;
                case 11:if(globals.empty())throw Error(invalid_preparsed);global=globals.back();globals.pop_back();break;
                default:break;
            }
        }else if(kind==2){
            if(local.delimiter)continue;
            if(tag==0){if(local.usages.size()>=65536)throw Error(invalid_preparsed);local.usages.push_back(usage(number,size));}
            else if(tag==1){local.minimum=usage(number,size);local.have_minimum=true;}
            else if(tag==2){local.maximum=usage(number,size);local.have_maximum=true;}
            else if(tag==10 && number)local.delimiter=true;
        }else if(kind==0){
            if(tag==10){
                if(stack.size()>=64 || collections.size()>=65535)throw Error(invalid_preparsed);
                const auto values=local.expand();const auto index=static_cast<unsigned>(collections.size());
                collections.push_back({values.empty()?Usage{}:values.front(),stack.empty()?index:stack.back(),stack.empty()?index:collections[stack.back()].root});stack.push_back(index);
            }else if(tag==12){if(stack.empty())throw Error(invalid_preparsed);stack.pop_back();}
            else if(tag==8 || tag==9 || tag==11){
                if(stack.empty() || !global.bits || global.bits>65535 || !global.count || global.count>65535)throw Error(invalid_preparsed);
                const unsigned type=tag==8?0U:tag==9?1U:2U;
                auto& offset=offsets[type][global.report];if(std::uint64_t{global.bits}*global.count+offset>limit*8U)throw Error(invalid_preparsed);
                const auto at=offset+8;offset+=global.bits*global.count;used[type][global.report]=true;report_roots[{type,global.report}].insert(collections[stack.back()].root);
                if(!(number&1U)){
                    auto values=local.expand();if(values.empty()){local.clear();continue;}
                    Field field;field.type=type;field.offset=at;field.width=global.bits;field.count=global.count;field.link=stack.back();field.report=global.report;field.flags=number;
                    field.minimum=global.minimum;field.maximum=global.maximum;field.physical_minimum=global.physical_minimum;field.physical_maximum=global.physical_maximum;
                    field.unit=global.unit;field.unit_exponent=global.exponent;field.range=local.have_minimum;field.link_usage=collections[stack.back()].usage;
                    // 一位元及 array controls 為 button；一般整數軸為 value。
                    field.button=global.bits==1 || !(number&2U);field.usages=std::move(values);
                    if(field.variable()){field.usages.resize(field.count,field.usages.back());}
                    if(fields.size()>=65536)throw Error(invalid_preparsed);
                    fields.push_back({std::move(field),collections[stack.back()].root});
                }
            }
            local.clear();
        }
    }
    if(!stack.empty() || !globals.empty() || collections.empty())throw Error(invalid_preparsed);
    const auto selected=std::find_if(collections.begin(),collections.end(),[&](const auto& c){return &c==&collections[c.root] && ((!top.page && !top.value) || c.usage==top);});
    if(selected==collections.end())throw Error(invalid_preparsed);
    const auto root=static_cast<unsigned>(selected-collections.begin());Layout result;result.usage=selected->usage;result.numbered=numbered;
    std::map<unsigned,unsigned> links;
    for(unsigned i=0;i<collections.size();++i)if(collections[i].root==root)links.emplace(i,static_cast<unsigned>(links.size()));
    result.links=static_cast<std::uint16_t>(links.size());
    for(auto& item:fields)if(item.root==root){auto& f=item.field;f.link=links.at(f.link);f.index=result.data_indices[f.type];
        if(f.indices()>65535-result.data_indices[f.type])throw Error(invalid_preparsed);
        result.data_indices[f.type]+=f.indices();result.fields.push_back(std::move(f));}
    for(unsigned type=0;type<3;++type)for(unsigned report=0;report<256;++report)if(used[type][report]){
        const bool belongs=report_roots.at({type,report}).contains(root);
        if(belongs){const auto bytes=(offsets[type][report]+7)/8+1;if(bytes>65535)throw Error(invalid_preparsed);result.report_bytes[type][report]=bytes;}}
    return result;
}
unsigned Layout::bytes(unsigned type)const{if(type>=3)throw Error(invalid_type);return *std::max_element(report_bytes[type].begin(),report_bytes[type].end());}
unsigned Field::data_index(unsigned element)const{
    if(button || !variable())return index+element;
    unsigned n=0;for(unsigned i=1;i<=element;++i)if(usages[i]!=usages[i-1])++n;return index+n;
}
unsigned Field::value_count(unsigned element)const{
    if(button || !variable())return 1;
    unsigned n=1;while(element+n<count && usages[element+n]==usages[element])++n;return n;
}
unsigned Field::indices()const{
    if(!variable())return static_cast<unsigned>(usages.size());
    if(button)return count;return data_index(count-1)-index+1;
}
std::vector<std::array<std::uint8_t,72>> Layout::caps(unsigned type,bool buttons)const{
    if(type>=3)throw Error(invalid_type);std::vector<std::array<std::uint8_t,72>> result;
    for(const auto& f:fields)if(f.type==type && f.button==buttons){
        const unsigned count=f.range?1U:static_cast<unsigned>(f.usages.size());
        for(unsigned n=0;n<count;++n){
            if(!buttons && !f.range && n && f.usages[n]==f.usages[n-1])continue;
            std::array<std::uint8_t,72> out{};const auto u=f.usages[f.range?0:n];put(out,0,u.page);out[2]=f.report;put(out,4,f.flags);put(out,6,f.link);put(out,8,f.link_usage.value);put(out,10,f.link_usage.page);out[12]=f.range;out[15]=f.absolute();
            if(!buttons){out[16]=(f.flags&64U)!=0;put(out,18,f.width);put(out,20,f.range?f.count:f.value_count(n));put(out,32,f.unit_exponent,4);put(out,36,f.unit,4);put(out,40,std::bit_cast<std::uint32_t>(f.minimum),4);put(out,44,std::bit_cast<std::uint32_t>(f.maximum),4);put(out,48,std::bit_cast<std::uint32_t>(f.physical_minimum),4);put(out,52,std::bit_cast<std::uint32_t>(f.physical_maximum),4);}
            put(out,56,u.value);if(f.range)put(out,58,f.usages.back().value);put(out,68,f.range?f.index:f.data_index(n));if(f.range)put(out,70,f.index+f.indices()-1);result.push_back(out);
        }
    }
    return result;
}
std::array<std::uint8_t,64> Layout::caps()const{
    std::array<std::uint8_t,64> out{};put(out,0,usage.value);put(out,2,usage.page);for(unsigned type=0;type<3;++type)put(out,4+type*2,bytes(type));put(out,44,links);
    for(unsigned type=0;type<3;++type){put(out,46+type*6,caps(type,true).size());put(out,48+type*6,caps(type,false).size());put(out,50+type*6,data_indices[type]);}return out;
}
void Layout::validate(unsigned type,std::span<const std::uint8_t> report)const{
    if(type>=3)throw Error(invalid_type);if(!bytes(type))throw Error(report_not_found);if(report.empty() || report.size()!=bytes(type))throw Error(invalid_length);
    if(!report_bytes[type][report[0]])throw Error(incompatible_id);
}
std::uint32_t Layout::value(const Field& f,unsigned element,std::span<const std::uint8_t> report)const{
    if(element>=f.count || f.width>32)throw Error(not_value_array);const auto at=f.offset+element*f.width;if(std::uint64_t{at}+f.width>report.size()*8)throw Error(invalid_length);
    std::uint32_t result{};for(unsigned i=0;i<f.width;++i)result|=std::uint32_t{(report[(at+i)/8]>>((at+i)&7U))&1U}<<i;return result;
}
std::int32_t Layout::scaled_value(const Field& f,unsigned element,std::span<const std::uint8_t> report)const{
    const auto raw=value(f,element,report);
    if(!f.width)throw Error(bad_log_phy_values);
    std::int64_t logical=raw;
    if(f.minimum<0 && (raw&(UINT32_C(1)<<(f.width-1))))logical-=INT64_C(1)<<f.width;
    const std::int64_t low=f.minimum,high=f.minimum<0?std::int64_t{f.maximum}:std::int64_t{static_cast<std::uint32_t>(f.maximum)};
    if(low>=high)throw Error(bad_log_phy_values);
    if(logical<low || logical>high)throw Error((f.flags&0x40U)?null_value:value_out_of_range);
    std::int64_t scaled=logical;
    if(f.physical_minimum || f.physical_maximum){
        const std::int64_t first=f.physical_minimum,last=f.physical_minimum<0?std::int64_t{f.physical_maximum}:std::int64_t{static_cast<std::uint32_t>(f.physical_maximum)};
        if(first>=last)throw Error(bad_log_phy_values);
        // 差值乘積可能超過 64 位；保留整數精度後才縮回 Windows LONG。
        scaled=static_cast<std::int64_t>(first+(static_cast<__int128>(logical-low)*(last-first))/(high-low));
    }
    if(scaled<INT32_MIN || scaled>INT32_MAX)throw Error(value_out_of_range);
    return static_cast<std::int32_t>(scaled);
}
void Layout::value(const Field& f,unsigned element,std::uint32_t value,std::span<std::uint8_t> report)const{
    if(element>=f.count || f.width>32)throw Error(not_value_array);const auto at=f.offset+element*f.width;if(std::uint64_t{at}+f.width>report.size()*8)throw Error(invalid_length);
    if(f.width<32 && value>=(1U<<f.width)){
        const auto signed_raw=std::bit_cast<std::int32_t>(value);
        if(f.minimum>=0 || signed_raw>=0 || signed_raw<-(INT64_C(1)<<(f.width-1)))throw Error(value_out_of_range);
        value&=(1U<<f.width)-1;
    }
    for(unsigned i=0;i<f.width;++i){const auto bit=(at+i)&7U;auto& byte=report[(at+i)/8];byte=static_cast<std::uint8_t>((byte&~(1U<<bit))|(((value>>i)&1U)<<bit));}
}
const Field* Layout::find(unsigned type,Usage wanted,unsigned link,std::uint8_t report,bool button,unsigned* element)const{
    if(type>=3)throw Error(invalid_type);bool wrong_report=false;
    for(const auto& f:fields)if(f.type==type && f.button==button && (!link || f.link==link)){
        const auto u=std::find(f.usages.begin(),f.usages.end(),wanted);if(u==f.usages.end())continue;
        if(f.report!=report){wrong_report=true;continue;}if(element)*element=static_cast<unsigned>(u-f.usages.begin());return &f;
    }
    if(wrong_report)throw Error(incompatible_id);
    return nullptr;
}
std::vector<std::uint8_t> preparsed(std::span<const std::uint8_t> descriptor,Usage top){
    const auto layout=parse(descriptor,top);std::vector<std::uint8_t> data(20+descriptor.size());std::copy(signature.begin(),signature.end(),data.begin());put(data,8,data.size(),4);put(data,12,layout.usage.page);put(data,14,layout.usage.value);put(data,16,descriptor.size(),4);std::copy(descriptor.begin(),descriptor.end(),data.begin()+20);return data;
}
std::uint32_t preparsed_size(std::span<const std::uint8_t> header){
    if(header.size()<20 || !std::equal(signature.begin(),signature.end(),header.begin()))throw Error(invalid_preparsed);
    const auto size=get(header,8);if(size<21 || size>limit+20 || size-20!=get(header,16))throw Error(invalid_preparsed);return size;
}
Layout decode(std::span<const std::uint8_t> data){if(preparsed_size(data)!=data.size())throw Error(invalid_preparsed);return parse(data.subspan(20),{static_cast<std::uint16_t>(get(data,12,2)),static_cast<std::uint16_t>(get(data,14,2))});}
}
