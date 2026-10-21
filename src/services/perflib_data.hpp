#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace x64arm::perflib {
using Guid=std::array<std::uint8_t,16>;
using Bytes=std::vector<std::uint8_t>;
inline constexpr std::uint32_t wildcard=UINT32_MAX;
struct Error:std::runtime_error{std::uint32_t code;explicit Error(std::uint32_t value):std::runtime_error("Perflib 資料契約無效"),code(value){}};
inline std::size_t align8(std::size_t value){if(value>SIZE_MAX-7)throw Error(8);return (value+7)&~std::size_t{7};}
struct Reader {
    std::span<const std::uint8_t> bytes;
    void check(std::size_t at,std::size_t size)const{if(at>bytes.size() || size>bytes.size()-at)throw Error(87);}
    std::uint64_t integer(std::size_t at,unsigned size)const{if(size>8)throw Error(87);check(at,size);std::uint64_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint64_t{bytes[at+i]}<<(i*8);return value;}
    std::uint32_t u32(std::size_t at)const{return static_cast<std::uint32_t>(integer(at,4));}
    Guid guid(std::size_t at)const{check(at,16);Guid value;std::copy_n(bytes.data()+at,16,value.data());return value;}
    std::u16string text(std::size_t at,std::size_t end)const{if(end<at)throw Error(87);check(at,end-at);if((end-at)%2)throw Error(87);std::u16string value;
        while(at<end){const auto c=static_cast<char16_t>(integer(at,2));at+=2;if(!c)return value;if(value.size()>=1024)throw Error(87);value+=c;}throw Error(87);}
};
inline void put(Bytes& bytes,std::size_t at,std::uint64_t value,unsigned size=4){if(size>8 || at>bytes.size() || size>bytes.size()-at)throw Error(87);for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));}
inline void put_guid(Bytes& bytes,std::size_t at,const Guid& value){if(at>bytes.size() || value.size()>bytes.size()-at)throw Error(87);std::copy(value.begin(),value.end(),bytes.data()+at);}
inline Bytes text(std::u16string_view value){if(value.size()>1024 || value.find(char16_t{})!=value.npos)throw Error(87);Bytes result((value.size()+1)*2);for(std::size_t i=0;i<value.size();++i)put(result,i*2,value[i],2);return result;}
struct Counter {
    std::uint32_t id{},type{};std::uint64_t attributes{};std::uint32_t size{},detail{};std::int32_t scale{};std::uint32_t offset{};
    bool reference()const{return (attributes&1U)!=0;}
    bool operator==(const Counter&)const=default;
};
struct CounterSet {Guid id{},provider{};std::uint32_t instance_type{};std::vector<Counter> counters;std::size_t extent{32};
    bool multi()const{return (instance_type&2U)!=0;}
    bool operator==(const CounterSet&)const=default;
};
inline CounterSet counter_set(std::span<const std::uint8_t> bytes,unsigned pointer_bytes){
    if(pointer_bytes!=4 && pointer_bytes!=8)throw Error(87);
    Reader reader{bytes};reader.check(0,40);CounterSet result;result.id=reader.guid(0);result.provider=reader.guid(16);const auto count=reader.u32(32);result.instance_type=reader.u32(36);
    if(!count || count>65536 || bytes.size()!=40+static_cast<std::uint64_t>(count)*32 ||
       (result.instance_type!=0 && result.instance_type!=2 && result.instance_type!=4 && result.instance_type!=6 && result.instance_type!=12 && result.instance_type!=22))throw Error(87);
    std::set<std::uint32_t> ids;std::vector<std::pair<std::size_t,std::size_t>> ranges;
    for(unsigned i=0;i<count;++i){const auto at=40+32*i;Counter counter;
        counter.id=reader.u32(at);counter.type=reader.u32(at+4);counter.attributes=reader.integer(at+8,8);counter.size=reader.u32(at+16);counter.detail=reader.u32(at+20);counter.scale=static_cast<std::int32_t>(reader.u32(at+24));counter.offset=reader.u32(at+28);
        if(counter.id==wildcard || !ids.insert(counter.id).second || !counter.size || counter.size>65536 || (counter.attributes&~31U) || counter.scale< -10 || counter.scale>10)throw Error(87);
        const auto size=counter.reference()?pointer_bytes:counter.size;const auto end=std::uint64_t{counter.offset}+size;
        if(counter.offset<32 || end>16U*1024U*1024U || (counter.reference() && counter.offset%pointer_bytes))throw Error(87);
        ranges.emplace_back(counter.offset,static_cast<std::size_t>(end));result.extent=std::max(result.extent,static_cast<std::size_t>(end));result.counters.push_back(counter);
    }
    std::sort(ranges.begin(),ranges.end());for(std::size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].second)throw Error(87);
    return result;
}
inline Bytes instance(const CounterSet& set,std::u16string_view name,std::uint32_t id){
    const auto value=text(name);const auto name_offset=align8(set.extent);if(name_offset>UINT32_MAX-value.size())throw Error(8);
    Bytes result(align8(name_offset+value.size()));put_guid(result,0,set.id);put(result,16,result.size());put(result,20,id);put(result,24,name_offset);put(result,28,value.size());std::copy(value.begin(),value.end(),result.data()+name_offset);return result;
}
struct Identifier {Guid set{};std::uint32_t status{},size{},counter{wildcard},instance{wildcard},index{},reserved{};std::u16string name;
    bool same_query(const Identifier& other)const{return set==other.set && counter==other.counter && instance==other.instance && name==other.name;}
};
inline std::vector<Identifier> identifiers(std::span<const std::uint8_t> bytes){
    if(bytes.size()>16U*1024U*1024U)throw Error(8);Reader reader{bytes};std::vector<Identifier> result;
    for(std::size_t at=0;at<bytes.size();){reader.check(at,40);Identifier id;id.set=reader.guid(at);id.status=reader.u32(at+16);id.size=reader.u32(at+20);id.counter=reader.u32(at+24);id.instance=reader.u32(at+28);id.index=reader.u32(at+32);id.reserved=reader.u32(at+36);
        if(id.size<40 || id.size%8 || id.reserved || id.size>bytes.size()-at || result.size()>=65536)throw Error(87);
        if(id.size>40)id.name=reader.text(at+40,at+id.size);result.push_back(std::move(id));at+=result.back().size;
    }return result;
}
inline Bytes identifier(const Identifier& value){
    const auto name=value.name.empty()?Bytes{}:text(value.name);Bytes result(align8(40+name.size()));put_guid(result,0,value.set);put(result,16,value.status);put(result,20,result.size());put(result,24,value.counter);put(result,28,value.instance);put(result,32,value.index);
    std::copy(name.begin(),name.end(),result.begin()+40);return result;
}
inline bool matches(std::u16string_view pattern,std::u16string_view value){
    // 有界 wildcard 比對；不將客體提供的名稱當成正規表示式。
    std::size_t p=0,v=0,star=pattern.npos,retry=0;
    while(v<value.size()){
        if(p<pattern.size() && (pattern[p]==u'?' || pattern[p]==value[v])){++p;++v;}
        else if(p<pattern.size() && pattern[p]==u'*'){star=p++;retry=v;}
        else if(star!=pattern.npos){p=star+1;v=++retry;}
        else return false;
    }
    while(p<pattern.size() && pattern[p]==u'*')++p;return p==pattern.size();
}
struct Value {std::uint32_t counter{},status{};Bytes bytes;};
struct Instance {std::uint32_t id{};std::u16string name;std::vector<Value> values;};
inline Bytes counter_data(std::span<const std::uint8_t> value){if(value.size()>65536)throw Error(8);Bytes result(align8(8+value.size()));put(result,0,value.size());put(result,4,result.size());std::copy(value.begin(),value.end(),result.begin()+8);return result;}
inline void append(Bytes& target,std::span<const std::uint8_t> source){if(target.size()>16U*1024U*1024U || source.size()>16U*1024U*1024U-target.size())throw Error(8);target.insert(target.end(),source.begin(),source.end());}
inline Bytes counter_ids(std::span<const std::uint32_t> counters){if(counters.size()>65536)throw Error(8);Bytes result(align8(8+4*counters.size()));put(result,0,result.size());put(result,4,counters.size());for(std::size_t i=0;i<counters.size();++i)put(result,8+i*4,counters[i]);return result;}
inline Bytes instance_header(std::uint32_t id,std::u16string_view name){
    if(name.size()>1024 || name.find(char16_t{})!=name.npos)throw Error(87);
    Bytes result(align8(8+2*(name.size()+1)));put(result,0,result.size());put(result,4,id);
    for(std::size_t i=0;i<name.size();++i)put(result,8+i*2,name[i],2);return result;
}
namespace detail {
class CounterValues {
    std::span<const Value> values_;
    std::vector<const Value*> buckets_;
    static std::size_t hash(std::uint32_t id){id^=id>>16;id*=0x7feb352dU;id^=id>>15;return id;}
public:
    CounterValues(std::span<const Value> values,std::size_t lookups):values_(values){
        // 小量查詢保留直接搜尋；大量查詢共用一次建立的借用索引。
        if(values.size()<=16 || lookups<=16 || values.size()>SIZE_MAX/4)return;
        buckets_.resize(2*std::bit_ceil(values.size()));const auto mask=buckets_.size()-1;
        for(const auto& value:values){auto slot=hash(value.counter)&mask;
            while(buckets_[slot] && buckets_[slot]->counter!=value.counter)slot=(slot+1)&mask;
            if(!buckets_[slot])buckets_[slot]=&value; // 重複 ID 維持第一筆的值與錯誤狀態。
        }
    }
    const Value* find(std::uint32_t id)const{
        if(buckets_.empty()){
            const auto found=std::find_if(values_.begin(),values_.end(),[&](const auto& value){return value.counter==id;});
            return found==values_.end()?nullptr:&*found;
        }
        const auto mask=buckets_.size()-1;auto slot=hash(id)&mask;
        while(buckets_[slot] && buckets_[slot]->counter!=id)slot=(slot+1)&mask;
        return buckets_[slot];
    }
};
inline void append_counter_values(Bytes& result,const Instance& instance,std::span<const std::uint32_t> counters){
    const CounterValues values(instance.values,counters.size());
    for(const auto counter:counters){
        const auto found=values.find(counter);if(!found || found->status)throw Error(found?found->status:2);
        if(found->bytes.size()>65536)throw Error(8);
        const auto size=align8(8+found->bytes.size()),at=result.size();
        if(at>16U*1024U*1024U || size>16U*1024U*1024U-at)throw Error(8);
        result.resize(at+size);put(result,at,found->bytes.size());put(result,at+4,size);
        std::copy(found->bytes.begin(),found->bytes.end(),result.begin()+static_cast<std::ptrdiff_t>(at+8));
    }
}
}
inline Bytes instance_data(const Instance& value,std::span<const std::uint32_t> counters){
    auto result=instance_header(value.id,value.name);
    detail::append_counter_values(result,value,counters);return result;
}
enum class DataType:std::uint32_t {Error=0,Single=1,Counters=2,Instances=4,CounterSet=6};
inline Bytes error_block(std::uint32_t code){Bytes result(16);put(result,0,code);put(result,8,16);return result;}
inline Bytes query_block(const Identifier& specification,std::span<const Instance> instances,std::span<const std::uint32_t> counter_list,bool multi_instance){
    Bytes result(16);const auto counters=specification.counter==wildcard?counter_list:std::span<const std::uint32_t>(&specification.counter,1);
    if(counters.empty()){put(result,0,2);put(result,8,16);return result;}
    std::vector<const Instance*> selected;for(const auto& item:instances)if((specification.instance==wildcard || specification.instance==item.id) && (specification.name.empty() || matches(specification.name,item.name)))selected.push_back(&item);
    if(selected.empty()){put(result,0,1168);put(result,8,16);return result;}
    const bool multiple=counters.size()>1 || specification.counter==wildcard;
    put(result,4,static_cast<std::uint32_t>(multi_instance?(multiple?DataType::CounterSet:DataType::Instances):(multiple?DataType::Counters:DataType::Single)));
    try{
        if(multiple)append(result,counter_ids(counters));
        if(multi_instance){const auto at=result.size();result.resize(at+8);put(result,at+4,selected.size());for(const auto value:selected)append(result,instance_data(*value,counters));put(result,at,result.size()-at);}
        else detail::append_counter_values(result,*selected.front(),counters);
    }catch(const Error& error){result.resize(16);std::fill(result.begin(),result.end(),0);put(result,0,error.code);}
    put(result,8,result.size());return result;
}
inline Bytes counter_registration(const Counter& counter){
    Bytes result(48);put(result,0,counter.id);put(result,4,counter.type);put(result,8,counter.attributes,8);put(result,16,counter.detail);put(result,20,static_cast<std::uint32_t>(counter.scale));
    for(const auto at:{24U,28U,32U,36U})put(result,at,wildcard);return result;
}
inline Bytes set_registration(const CounterSet& set){
    Bytes result(32);put_guid(result,0,set.id);put(result,20,set.counters.empty()?100:set.counters.front().detail);put(result,24,set.counters.size());put(result,28,set.instance_type);
    for(const auto& counter:set.counters)append(result,counter_registration(counter));return result;
}
inline Bytes notification(std::uint32_t code,const Guid& provider,const Identifier* counter,std::u16string_view machine,std::uint64_t filetime){
    const auto encoded_machine=text(machine);Bytes payload;
    if(code==1 || code==2){if(!counter)throw Error(87);payload.resize(40);put_guid(payload,0,counter->set);put(payload,20,counter->counter);put(payload,24,counter->instance);put(payload,28,40);
        append(payload,encoded_machine);if(!counter->name.empty()){put(payload,32,payload.size());append(payload,text(counter->name));}put(payload,16,payload.size());
    }else payload=encoded_machine;
    // 官方 callback 契約：WNODE_HEADER 後接 PERF_COUNTER_IDENTITY 或 machine name。
    Bytes result(48);put(result,0,48+payload.size());put(result,16,filetime,8);put_guid(result,24,provider);append(result,payload);return result;
}
struct Timestamp {std::uint64_t counter{},filetime{},frequency{};std::array<std::uint16_t,8> systemtime{};};
inline Bytes query_data(const Timestamp& stamp,std::span<const Bytes> blocks){Bytes result(48);put(result,4,blocks.size());put(result,8,stamp.counter,8);put(result,16,stamp.filetime,8);put(result,24,stamp.frequency,8);for(unsigned i=0;i<8;++i)put(result,32+2*i,stamp.systemtime[i],2);
    for(const auto& block:blocks)append(result,block);put(result,0,result.size());return result;
}
}
