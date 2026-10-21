#pragma once
#include "perflib_data.hpp"

namespace x64arm::perflib {
// RPC 回覆仍是官方 PERF_DATA_HEADER；只傳數值，不傳 provider 的私人 offset／指標。
struct QueryReply {
    Timestamp stamp;
    std::vector<std::span<const std::uint8_t>> blocks;
};
inline QueryReply query_reply(std::span<const std::uint8_t> bytes,std::size_t expected){
    try{
        if(bytes.size()<48 || bytes.size()>16U*1024U*1024U || bytes.size()%8 || expected>65536)throw Error(13);
        Reader reader{bytes};if(reader.u32(0)!=bytes.size() || reader.u32(4)!=expected)throw Error(13);
        QueryReply result;result.stamp.counter=reader.integer(8,8);result.stamp.filetime=reader.integer(16,8);result.stamp.frequency=reader.integer(24,8);
        // 兩端都使用引擎的 100 ns clock。不能將不同程序的 clock_ticks 視為相同原點。
        if(result.stamp.frequency!=10000000)throw Error(13);
        for(unsigned i=0;i<8;++i)result.stamp.systemtime[i]=static_cast<std::uint16_t>(reader.integer(32+2*i,2));
        const auto& time=result.stamp.systemtime;
        if(!time[0] || !time[1] || time[1]>12 || time[2]>6 || !time[3] || time[3]>31 || time[4]>23 || time[5]>59 || time[6]>59 || time[7]>999)throw Error(13);
        result.blocks.reserve(expected);std::size_t at=48;
        for(std::size_t i=0;i<expected;++i){
            reader.check(at,16);const auto size=reader.u32(at+8);
            if(size<16 || size%8 || size>bytes.size()-at || reader.u32(at+12))throw Error(13);
            result.blocks.push_back(bytes.subspan(at,size));at+=size;
        }
        if(at!=bytes.size())throw Error(13);return result;
    }catch(const Error&){throw Error(13);}
}
struct CounterReply {
    std::uint32_t status{};
    std::vector<Instance> instances;
};
inline CounterReply counter_reply(std::span<const std::uint8_t> bytes,const Identifier& spec,const CounterSet& registration){
    try{
        Reader reader{bytes};reader.check(0,16);
        if(reader.u32(8)!=bytes.size() || reader.u32(12))throw Error(13);
        CounterReply result;result.status=reader.u32(0);const auto type=reader.u32(4);
        if(type==static_cast<std::uint32_t>(DataType::Error)){
            if(!result.status || bytes.size()!=16)throw Error(13);return result;
        }
        if(result.status)throw Error(13);
        std::map<std::uint32_t,const Counter*> expected;
        for(const auto& counter:registration.counters)if(spec.counter==wildcard || spec.counter==counter.id)expected.emplace(counter.id,&counter);
        if(expected.empty() || expected.size()>65536)throw Error(13);
        const bool multiple=spec.counter==wildcard || expected.size()>1;
        const auto expected_type=registration.multi()?(multiple?DataType::CounterSet:DataType::Instances):(multiple?DataType::Counters:DataType::Single);
        if(type!=static_cast<std::uint32_t>(expected_type))throw Error(13);
        std::vector<std::uint32_t> counters;std::size_t at=16;
        if(multiple){
            reader.check(at,8);const auto size=reader.u32(at),count=reader.u32(at+4);
            if(count!=expected.size() || size!=align8(8+4*std::size_t{count}) || size>bytes.size()-at)throw Error(13);
            std::set<std::uint32_t> seen;
            for(std::size_t i=0;i<count;++i){const auto counter=reader.u32(at+8+4*i);
                if(!expected.contains(counter) || !seen.insert(counter).second)throw Error(13);counters.push_back(counter);
            }
            at+=size;
        }else counters.push_back(spec.counter);
        std::uint32_t count=1;
        if(registration.multi()){
            reader.check(at,8);const auto size=reader.u32(at);count=reader.u32(at+4);
            if(size!=bytes.size()-at || size<8 || size%8 || !count || count>65536)throw Error(13);at+=8;
            // 在 reserve 前核對最小 instance／value 空間，避免畸形 count 大量配置。
            const auto minimum=16+16*counters.size();if(count>(bytes.size()-at)/minimum)throw Error(13);
        }
        result.instances.reserve(count);
        for(std::uint32_t i=0;i<count;++i){
            Instance instance;
            if(registration.multi()){
                reader.check(at,8);const auto size=reader.u32(at);instance.id=reader.u32(at+4);
                if(size<16 || size%8 || size>bytes.size()-at)throw Error(13);
                instance.name=reader.text(at+8,at+size);at+=size;
                if((spec.instance!=wildcard && instance.id!=spec.instance) || (!spec.name.empty() && !matches(spec.name,instance.name)))throw Error(13);
            }else{
                // 單實例 wire block 不帶名稱／ID；保留 selector 以便共同編碼流程使用。
                instance.id=spec.instance==wildcard?0:spec.instance;instance.name=spec.name;
            }
            instance.values.reserve(counters.size());
            for(const auto counter:counters){
                reader.check(at,8);const auto data_size=reader.u32(at),size=reader.u32(at+4);
                if(data_size!=expected.at(counter)->size || size!=align8(8+std::size_t{data_size}) || size>bytes.size()-at)throw Error(13);
                const auto data=bytes.subspan(at+8,data_size);
                Value value;value.counter=counter;value.bytes.assign(data.begin(),data.end());
                instance.values.push_back(std::move(value));at+=size;
            }
            result.instances.push_back(std::move(instance));
        }
        if(at!=bytes.size())throw Error(13);return result;
    }catch(const Error&){throw Error(13);}
}
}
