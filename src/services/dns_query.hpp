#pragma once
#include "x64arm/unicode.hpp"
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <netdb.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace x64arm::dns_detail {
struct Error {std::uint32_t code;};
struct Record {std::string name;std::uint16_t type{};std::uint32_t ttl{},section{1};std::vector<std::uint8_t> bytes;std::vector<std::string> names;std::vector<std::uint32_t> numbers;};
inline std::vector<Record> query(std::string name,std::uint16_t type,std::uint32_t options){
    if(name.empty() || name.size()>253 || !type)throw Error{87};
    // multicast-only 與原始封包輸出需要不同的資料／傳輸契約，不混入一般 RR 結果。
    if(options&0x600U)throw Error{50};
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    using Clock=std::chrono::steady_clock;
    struct Cached {Clock::time_point created;std::vector<Record> records;std::uint32_t ttl;};
    static std::mutex mutex;static std::map<std::pair<std::string,std::uint16_t>,Cached> cache;
    const auto key=std::pair{name,type};
    if(!(options&0x108U)){std::lock_guard lock(mutex);if(auto it=cache.find(key);it!=cache.end()){
        const auto age=std::chrono::duration_cast<std::chrono::seconds>(Clock::now()-it->second.created).count();
        if(age<it->second.ttl){auto records=it->second.records;if(!(options&0x100000U))for(auto& record:records)record.ttl-=std::min(record.ttl,static_cast<std::uint32_t>(age));return records;}cache.erase(it);
    }}
    std::vector<Record> records;
    if((type==1 || type==28) && !(options&0x100U)){
        const auto family=type==1?AF_INET:AF_INET6;const unsigned size=type==1?4U:16U;std::array<std::uint8_t,16> address{};
        if(::inet_pton(family,name.c_str(),address.data())==1)records.push_back({name,type,0,1,{address.begin(),address.begin()+size},{},{}});
        if(records.empty() && !(options&0x40U)){
            std::ifstream hosts("/etc/hosts");std::string line;
            while(std::getline(hosts,line)){
                line.resize(line.find('#')==std::string::npos?line.size():line.find('#'));std::istringstream words(line);std::string ip,alias;words>>ip;
                if(::inet_pton(family,ip.c_str(),address.data())!=1)continue;
                while(words>>alias){std::transform(alias.begin(),alias.end(),alias.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});if(alias==name){records.push_back({name,type,0,1,{address.begin(),address.begin()+size},{},{}});break;}}
            }
        }
        if(!records.empty())return records;
    }
    if(options&0x10U)throw Error{9501};
    struct Resolver {struct __res_state state{};bool initialized{};~Resolver(){if(initialized){
#if defined(__APPLE__)
        ::res_ndestroy(&state);
#else
        ::res_nclose(&state);
#endif
    }}} resolver;
    if(::res_ninit(&resolver.state))throw Error{9002};resolver.initialized=true;
    if(options&1U)resolver.state.options|=RES_IGNTC;if(options&2U)resolver.state.options|=RES_USEVC;if(options&4U)resolver.state.options&=~static_cast<decltype(resolver.state.options)>(RES_RECURSE);
    std::array<std::uint8_t,65536> packet{};
    const auto count=options&0x1000U?::res_nquery(&resolver.state,name.c_str(),ns_c_in,type,packet.data(),static_cast<int>(packet.size())):
        ::res_nsearch(&resolver.state,name.c_str(),ns_c_in,type,packet.data(),static_cast<int>(packet.size()));
    if(count<0){switch(resolver.state.res_h_errno){case HOST_NOT_FOUND:throw Error{9003};case NO_DATA:throw Error{9501};case TRY_AGAIN:throw Error{1460};default:throw Error{9002};}}
    ns_msg message{};if(count>static_cast<int>(packet.size()) || ::ns_initparse(packet.data(),count,&message))throw Error{9502};
    const auto rcode=ns_msg_getflag(message,ns_f_rcode);if(rcode)throw Error{9000U+static_cast<std::uint32_t>(rcode)};
    for(unsigned section=1;section<=3;++section)for(int index=0;index<ns_msg_count(message,static_cast<ns_sect>(section));++index){
        ns_rr rr{};if(::ns_parserr(&message,static_cast<ns_sect>(section),index,&rr))throw Error{9502};if(ns_rr_class(rr)!=ns_c_in)continue;
        Record record;record.name=ns_rr_name(rr);record.type=static_cast<std::uint16_t>(ns_rr_type(rr));record.ttl=ns_rr_ttl(rr);record.section=section;
        const auto* cursor=ns_rr_rdata(rr);const auto* end=cursor+ns_rr_rdlen(rr);
        const auto number=[&](unsigned size){if(static_cast<std::size_t>(end-cursor)<size)throw Error{9502};std::uint32_t value=0;for(unsigned i=0;i<size;++i)value=(value<<8)|*cursor++;return value;};
        const auto domain=[&](){std::array<char,NS_MAXDNAME> expanded{};const auto used=::dn_expand(packet.data(),packet.data()+count,cursor,expanded.data(),static_cast<int>(expanded.size()));if(used<0 || used>end-cursor)throw Error{9502};cursor+=used;record.names.emplace_back(expanded.data());};
        switch(record.type){
        case 1:case 28:if(end-cursor!=(record.type==1?4:16))throw Error{9502};record.bytes.assign(cursor,end);break;
        case 2:case 3:case 4:case 5:case 7:case 8:case 9:case 12:case 39:domain();break;
        case 6:domain();domain();for(unsigned i=0;i<5;++i)record.numbers.push_back(number(4));break;
        case 14:case 17:domain();domain();break;
        case 15:case 18:case 21:record.numbers.push_back(number(2));domain();break;
        case 33:for(unsigned i=0;i<3;++i)record.numbers.push_back(number(2));domain();break;
        case 13:case 16:case 19:case 20:while(cursor<end){const auto size=number(1);if(size>static_cast<std::size_t>(end-cursor))throw Error{9502};record.names.emplace_back(reinterpret_cast<const char*>(cursor),size);cursor+=size;}break;
        default:record.bytes.assign(cursor,end);break;
        }
        records.push_back(std::move(record));if(records.size()>4096)throw Error{8};
    }
    if(std::none_of(records.begin(),records.end(),[](const auto& record){return record.section==1;}))throw Error{9501};
    auto ttl=UINT32_MAX;for(const auto& record:records)ttl=std::min(ttl,record.ttl);
    if(ttl){std::lock_guard lock(mutex);if(cache.size()>=512)cache.erase(cache.begin());cache[key]={Clock::now(),records,ttl};}
    return records;
}
struct Blob {std::vector<std::uint8_t> bytes;std::vector<std::size_t> pointers;};
inline Blob pack(const Record& record,unsigned unit,unsigned charset){
    const unsigned header=2*unit+16;Blob blob;unsigned data_size=0;
    switch(record.type){
    case 1:case 28:case 11:data_size=static_cast<unsigned>(record.bytes.size());break;
    case 2:case 3:case 4:case 5:case 7:case 8:case 9:case 12:case 39:data_size=unit;break;
    case 6:data_size=2*unit+((20+unit-1)/unit)*unit;break;case 14:case 17:data_size=2*unit;break;
    case 15:case 18:case 21:data_size=2*unit;break;case 33:data_size=unit+8;break;
    case 13:case 16:case 19:case 20:data_size=unit+static_cast<unsigned>(record.names.size())*unit;break;
    default:data_size=4+static_cast<unsigned>(record.bytes.size());break;
    }
    if(data_size>65535)throw Error{9502};blob.bytes.resize(header+data_size);
    const auto put=[&](std::size_t at,std::uint64_t value,unsigned size=4){for(unsigned i=0;i<size;++i)blob.bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));};
    const auto text=[&](std::size_t at,const std::string& value){const auto offset=blob.bytes.size();put(at,offset,unit);blob.pointers.push_back(at);if(charset==1){const auto bytes=utf16_bytes(utf8_to_utf16_lossy(value));blob.bytes.insert(blob.bytes.end(),bytes.begin(),bytes.end());}else {blob.bytes.insert(blob.bytes.end(),value.begin(),value.end());blob.bytes.push_back(0);}};
    text(unit,record.name);put(2*unit,record.type,2);put(2*unit+2,data_size,2);put(2*unit+4,record.section|(charset<<3));put(2*unit+8,record.ttl);
    switch(record.type){
    case 1:case 28:case 11:std::copy(record.bytes.begin(),record.bytes.end(),blob.bytes.begin()+header);break;
    case 2:case 3:case 4:case 5:case 7:case 8:case 9:case 12:case 39:text(header,record.names.at(0));break;
    case 6:text(header,record.names.at(0));text(header+unit,record.names.at(1));for(unsigned i=0;i<5;++i)put(header+2*unit+i*4,record.numbers.at(i));break;
    case 14:case 17:text(header,record.names.at(0));text(header+unit,record.names.at(1));break;
    case 15:case 18:case 21:case 33:text(header,record.names.at(0));for(unsigned i=0;i<record.numbers.size();++i)put(header+unit+i*2,record.numbers[i],2);break;
    case 13:case 16:case 19:case 20:put(header,record.names.size());for(unsigned i=0;i<record.names.size();++i)text(header+unit+i*unit,record.names[i]);break;
    default:put(header,record.bytes.size());std::copy(record.bytes.begin(),record.bytes.end(),blob.bytes.begin()+header+4);break;
    }
    return blob;
}
}
