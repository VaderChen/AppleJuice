#pragma once
#include "x64arm/unicode.hpp"
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
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
#include <string_view>
#include <vector>

namespace x64arm::dns_detail {
struct Error {std::uint32_t code;};
// raw：資料區依 wDataLength 原樣保存（沒有內嵌指標的型別）；flags 為 Section／CharSet 以外的 DNS_RECORD 旗標位元（如 Delete）。
struct Record {std::string name;std::uint16_t type{};std::uint32_t ttl{},section{1};std::vector<std::uint8_t> bytes;std::vector<std::string> names;std::vector<std::uint32_t> numbers;std::uint32_t flags{};bool raw{};};
using Clock=std::chrono::steady_clock;
struct Cached {Clock::time_point created;std::vector<Record> records;std::uint32_t ttl;};
// 解析器快取：DnsQuery／DnsQueryEx 共用，DnsFlushResolverCache／DnsGetCacheDataTable 讀寫同一份。
struct CacheState {std::mutex mutex;std::map<std::pair<std::string,std::uint16_t>,Cached> entries;};
inline CacheState& cache_state(){static CacheState state;return state;}
inline std::string lower(std::string name){std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});return name;}
// DNS 名稱比較：ASCII 不分大小寫，忽略結尾的一個點。
inline bool same_name(std::string a,std::string b){if(!a.empty() && a.back()=='.' && a!=".")a.pop_back();if(!b.empty() && b.back()=='.' && b!=".")b.pop_back();return lower(std::move(a))==lower(std::move(b));}
// 解析完整 DNS 回應（標頭計數為網路位元組順序），依序回傳答案、授權與附加區段的 IN 類別 RR。
inline std::vector<Record> parse_message(const std::uint8_t* packet,int count){
    ns_msg message{};if(count<12 || ::ns_initparse(packet,count,&message))throw Error{9502};
    const auto rcode=ns_msg_getflag(message,ns_f_rcode);if(rcode)throw Error{9000U+static_cast<std::uint32_t>(rcode)};
    std::vector<Record> records;
    for(unsigned section=1;section<=3;++section)for(int index=0;index<ns_msg_count(message,static_cast<ns_sect>(section));++index){
        ns_rr rr{};if(::ns_parserr(&message,static_cast<ns_sect>(section),index,&rr))throw Error{9502};if(ns_rr_class(rr)!=ns_c_in)continue;
        Record record;record.name=ns_rr_name(rr);record.type=static_cast<std::uint16_t>(ns_rr_type(rr));record.ttl=ns_rr_ttl(rr);record.section=section;
        const auto* cursor=ns_rr_rdata(rr);const auto* end=cursor+ns_rr_rdlen(rr);
        const auto number=[&](unsigned size){if(static_cast<std::size_t>(end-cursor)<size)throw Error{9502};std::uint32_t value=0;for(unsigned i=0;i<size;++i)value=(value<<8)|*cursor++;return value;};
        const auto domain=[&](){std::array<char,NS_MAXDNAME> expanded{};const auto used=::dn_expand(packet,packet+count,cursor,expanded.data(),static_cast<int>(expanded.size()));if(used<0 || used>end-cursor)throw Error{9502};cursor+=used;record.names.emplace_back(expanded.data());};
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
    return records;
}
// libresolv 狀態：res_ninit 後可改用呼叫端指定的伺服器（原生 sockaddr 位元組）；查詢與動態更新共用。
struct Resolver {
    struct __res_state state{};bool initialized{};
    Resolver()=default;Resolver(const Resolver&)=delete;Resolver& operator=(const Resolver&)=delete;
    ~Resolver(){if(initialized){
#if defined(__APPLE__)
        ::res_ndestroy(&state);
#else
        ::res_nclose(&state);
#endif
    }}
    void open(const std::vector<std::vector<std::uint8_t>>* servers){
        if(::res_ninit(&state))throw Error{9002};initialized=true;
        if(!servers || servers->empty())return;
#if defined(__APPLE__)
        std::vector<res_sockaddr_union> list;for(const auto& bytes:*servers){res_sockaddr_union item{};std::memcpy(&item,bytes.data(),std::min(bytes.size(),sizeof(item)));list.push_back(item);}
        ::res_setservers(&state,list.data(),static_cast<int>(list.size()));
#else
        int count=0;for(const auto& bytes:*servers)if(bytes.size()>=sizeof(sockaddr_in) && reinterpret_cast<const sockaddr*>(bytes.data())->sa_family==AF_INET && count<MAXNS)std::memcpy(&state.nsaddr_list[count++],bytes.data(),sizeof(sockaddr_in));
        if(!count)throw Error{87};state.nscount=count;
#endif
    }
};
// servers：DnsQueryEx 的 pDnsServerList（原生 sockaddr 位元組）；指定時直接詢問這些伺服器，不讀寫快取與 hosts。
inline std::vector<Record> query(std::string name,std::uint16_t type,std::uint32_t options,const std::vector<std::vector<std::uint8_t>>* servers=nullptr){
    if(name.empty() || name.size()>253 || !type)throw Error{87};
    // multicast-only 與原始封包輸出需要不同的資料／傳輸契約，不混入一般 RR 結果。
    if(options&0x600U)throw Error{50};
    name=lower(std::move(name));
    auto& [mutex,cache]=cache_state();const bool custom=servers && !servers->empty();
    const auto key=std::pair{name,type};
    if(!(options&0x108U) && !custom){std::lock_guard lock(mutex);if(auto it=cache.find(key);it!=cache.end()){
        const auto age=std::chrono::duration_cast<std::chrono::seconds>(Clock::now()-it->second.created).count();
        if(age<it->second.ttl){auto records=it->second.records;if(!(options&0x100000U))for(auto& record:records)record.ttl-=std::min(record.ttl,static_cast<std::uint32_t>(age));return records;}cache.erase(it);
    }}
    std::vector<Record> records;
    if((type==1 || type==28) && !(options&0x100U)){
        const auto family=type==1?AF_INET:AF_INET6;const unsigned size=type==1?4U:16U;std::array<std::uint8_t,16> address{};
        if(::inet_pton(family,name.c_str(),address.data())==1)records.push_back({name,type,0,1,{address.begin(),address.begin()+size},{},{}});
        if(records.empty() && !(options&0x40U) && !custom){
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
    Resolver resolver;resolver.open(custom?servers:nullptr);
    if(options&1U)resolver.state.options|=RES_IGNTC;if(options&2U)resolver.state.options|=RES_USEVC;if(options&4U)resolver.state.options&=~static_cast<decltype(resolver.state.options)>(RES_RECURSE);
    std::array<std::uint8_t,65536> packet{};
    const auto count=options&0x1000U?::res_nquery(&resolver.state,name.c_str(),ns_c_in,type,packet.data(),static_cast<int>(packet.size())):
        ::res_nsearch(&resolver.state,name.c_str(),ns_c_in,type,packet.data(),static_cast<int>(packet.size()));
    if(count<0){switch(resolver.state.res_h_errno){case HOST_NOT_FOUND:throw Error{9003};case NO_DATA:throw Error{9501};case TRY_AGAIN:throw Error{1460};default:throw Error{9002};}}
    if(count>static_cast<int>(packet.size()))throw Error{9502};
    records=parse_message(packet.data(),count);
    if(std::none_of(records.begin(),records.end(),[](const auto& record){return record.section==1;}))throw Error{9501};
    auto ttl=UINT32_MAX;for(const auto& record:records)ttl=std::min(ttl,record.ttl);
    if(ttl && !custom){std::lock_guard lock(mutex);if(cache.size()>=512)cache.erase(cache.begin());cache[key]={Clock::now(),records,ttl};}
    return records;
}
// RFC 1035 名稱編碼：空字串或 "." 為根；空標籤、標籤超過 63 或總長超過 255 位元組回 ERROR_INVALID_NAME。
inline std::vector<std::uint8_t> encode_name(std::string_view name){
    std::vector<std::uint8_t> out;
    if(!name.empty() && name!="."){
        if(name.back()=='.')name.remove_suffix(1);
        for(;;){
            const auto dot=name.find('.');const auto label=name.substr(0,dot);if(label.empty() || label.size()>63)throw Error{123};
            out.push_back(static_cast<std::uint8_t>(label.size()));out.insert(out.end(),label.begin(),label.end());
            if(dot==std::string_view::npos)break;name.remove_prefix(dot+1);
        }
    }
    out.push_back(0);if(out.size()>255)throw Error{123};return out;
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
    if(record.raw)data_size=static_cast<unsigned>(record.bytes.size());
    if(data_size>65535)throw Error{9502};blob.bytes.resize(header+data_size);
    const auto put=[&](std::size_t at,std::uint64_t value,unsigned size=4){for(unsigned i=0;i<size;++i)blob.bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));};
    const auto text=[&](std::size_t at,const std::string& value){const auto offset=blob.bytes.size();put(at,offset,unit);blob.pointers.push_back(at);if(charset==1){const auto bytes=utf16_bytes(utf8_to_utf16_lossy(value));blob.bytes.insert(blob.bytes.end(),bytes.begin(),bytes.end());}else {blob.bytes.insert(blob.bytes.end(),value.begin(),value.end());blob.bytes.push_back(0);}};
    text(unit,record.name);put(2*unit,record.type,2);put(2*unit+2,data_size,2);put(2*unit+4,(record.section&3)|(charset<<3)|(record.flags&~0x1bU));put(2*unit+8,record.ttl);
    if(record.raw){std::copy(record.bytes.begin(),record.bytes.end(),blob.bytes.begin()+header);return blob;}
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

// RR 的 RDATA 線路格式（名稱不壓縮）；pack／parse_message 的反向，供動態更新使用。
inline std::vector<std::uint8_t> encode_rdata(const Record& record){
    std::vector<std::uint8_t> out;const auto put16=[&](std::uint32_t value){out.push_back(static_cast<std::uint8_t>(value>>8));out.push_back(static_cast<std::uint8_t>(value));};
    const auto put32=[&](std::uint32_t value){put16(value>>16);put16(value&0xffffU);};
    const auto name=[&](std::size_t index){if(index>=record.names.size())throw Error{87};const auto bytes=encode_name(record.names[index]);out.insert(out.end(),bytes.begin(),bytes.end());};
    const auto number=[&](std::size_t index){if(index>=record.numbers.size())throw Error{87};return record.numbers[index];};
    switch(record.type){
    case 1:case 28:if(record.bytes.size()!=(record.type==1?4U:16U))throw Error{87};return record.bytes;
    case 2:case 3:case 4:case 5:case 7:case 8:case 9:case 12:case 39:name(0);break;
    case 6:name(0);name(1);for(std::size_t i=0;i<5;++i)put32(number(i));break;
    case 14:case 17:name(0);name(1);break;
    case 15:case 18:case 21:put16(number(0));name(0);break;
    case 33:for(std::size_t i=0;i<3;++i)put16(number(i));name(0);break;
    case 13:case 16:case 19:case 20:for(const auto& text:record.names){if(text.size()>255)throw Error{87};out.push_back(static_cast<std::uint8_t>(text.size()));out.insert(out.end(),text.begin(),text.end());}break;
    default:return record.bytes;
    }
    return out;
}
// RFC 2136 動態更新：先以 SOA 查詢找出 zone（答案區或授權區的 SOA 擁有者，沒有 SOA 時逐層往上），再送 UPDATE 到同一組伺服器。
// replace：先刪除整個 RRset（class ANY）再新增；否則刪除指定 RR（class NONE）後新增。回應的 RCODE 對應 DNS_ERROR_RCODE_*（9000＋RCODE）。
inline std::uint32_t update(const std::vector<Record>& adds,const std::vector<Record>& deletes,bool replace,const std::vector<std::vector<std::uint8_t>>* servers){
    const auto& sample=!adds.empty()?adds.front():deletes.front();
    Resolver resolver;resolver.open(servers);
    // 伺服器清單：呼叫端指定者或系統 resolver 的設定。res_nsend 會把 REFUSED／SERVFAIL／NOTIMP 當成換下一台並回報失敗，
    // 動態更新需要原樣的 RCODE，因此自行以 UDP 交換：每台伺服器送兩次、各等 2 秒，回應 ID 相符才採用。
    std::vector<sockaddr_storage> targets;
    if(servers && !servers->empty()){for(const auto& bytes:*servers){sockaddr_storage item{};std::memcpy(&item,bytes.data(),std::min(bytes.size(),sizeof(item)));targets.push_back(item);}}
    else{
#if defined(__APPLE__)
        std::array<res_sockaddr_union,MAXNS> list{};const auto count=::res_getservers(&resolver.state,list.data(),static_cast<int>(list.size()));
        for(int i=0;i<count;++i){sockaddr_storage item{};std::memcpy(&item,&list[static_cast<std::size_t>(i)],std::min(sizeof(item),sizeof(list[0])));targets.push_back(item);}
#else
        for(int i=0;i<resolver.state.nscount;++i){sockaddr_storage item{};std::memcpy(&item,&resolver.state.nsaddr_list[i],sizeof(sockaddr_in));targets.push_back(item);}
#endif
    }
    if(targets.empty())throw Error{9852}; // DNS_ERROR_NO_DNS_SERVERS
    std::array<std::uint8_t,65536> answer{};
    const auto send=[&](const std::uint8_t* message,int size)->int{
        for(const auto& target:targets){
            const auto family=target.ss_family;const auto length=static_cast<socklen_t>(family==AF_INET6?sizeof(sockaddr_in6):sizeof(sockaddr_in));
            const int socket=::socket(family,SOCK_DGRAM,0);if(socket<0)continue;
            struct Close {int fd;~Close(){::close(fd);}} close_socket{socket};
            for(int attempt=0;attempt<2;++attempt){
                if(::sendto(socket,message,static_cast<std::size_t>(size),0,reinterpret_cast<const sockaddr*>(&target),length)!=size)break;
                const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                for(;;){
                    const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(until-std::chrono::steady_clock::now()).count();if(left<=0)break;
                    pollfd wait{socket,POLLIN,0};if(::poll(&wait,1,static_cast<int>(left))<=0)break;
                    const auto count=::recv(socket,answer.data(),answer.size(),0);
                    if(count>=12 && answer[0]==message[0] && answer[1]==message[1] && (answer[2]&0x80))return static_cast<int>(count);
                }
            }
        }
        throw Error{1460};
    };
    std::string zone;
    for(std::string candidate=sample.name;zone.empty();){
        std::array<std::uint8_t,NS_PACKETSZ> query{};
        const auto size=::res_nmkquery(&resolver.state,ns_o_query,candidate.c_str(),ns_c_in,ns_t_soa,nullptr,0,nullptr,query.data(),static_cast<int>(query.size()));if(size<0)throw Error{123};
        const auto count=send(query.data(),size);ns_msg message{};if(::ns_initparse(answer.data(),count,&message))throw Error{9502};
        const auto rcode=ns_msg_getflag(message,ns_f_rcode);
        for(const auto section:{ns_s_an,ns_s_ns})for(int index=0;zone.empty() && index<ns_msg_count(message,section);++index){
            ns_rr rr{};if(::ns_parserr(&message,section,index,&rr))throw Error{9502};if(ns_rr_type(rr)==ns_t_soa)zone=ns_rr_name(rr);
        }
        if(!zone.empty())break;
        if(rcode && rcode!=ns_r_nxdomain)throw Error{9000U+static_cast<std::uint32_t>(rcode)};
        const auto dot=candidate.find('.');if(dot==std::string::npos || dot+1>=candidate.size())throw Error{9010}; // DNS_ERROR_RCODE_NOTZONE
        candidate=candidate.substr(dot+1);
    }
    std::vector<std::uint8_t> message(12);
    const auto put16=[&](std::uint32_t value){message.push_back(static_cast<std::uint8_t>(value>>8));message.push_back(static_cast<std::uint8_t>(value));};
    const auto put32=[&](std::uint32_t value){put16(value>>16);put16(value&0xffffU);};
    const auto put_name=[&](const std::string& name){const auto bytes=encode_name(name);message.insert(message.end(),bytes.begin(),bytes.end());};
    std::uint16_t id=0;::arc4random_buf(&id,sizeof id);message[0]=static_cast<std::uint8_t>(id>>8);message[1]=static_cast<std::uint8_t>(id);message[2]=5U<<3; // opcode UPDATE
    message[5]=1; // ZOCOUNT
    const auto updates=static_cast<std::uint32_t>(adds.size()+deletes.size()+(replace?1U:0U));if(updates>0xffffU)throw Error{87};message[8]=static_cast<std::uint8_t>(updates>>8);message[9]=static_cast<std::uint8_t>(updates);
    put_name(zone);put16(ns_t_soa);put16(ns_c_in);
    if(replace){put_name(sample.name);put16(sample.type);put16(ns_c_any);put32(0);put16(0);}
    for(const auto& record:deletes){const auto data=encode_rdata(record);put_name(record.name);put16(record.type);put16(ns_c_none);put32(0);put16(static_cast<std::uint32_t>(data.size()));message.insert(message.end(),data.begin(),data.end());}
    for(const auto& record:adds){const auto data=encode_rdata(record);put_name(record.name);put16(record.type);put16(ns_c_in);put32(record.ttl);put16(static_cast<std::uint32_t>(data.size()));message.insert(message.end(),data.begin(),data.end());}
    if(message.size()>65535)throw Error{87};
    const auto count=send(message.data(),static_cast<int>(message.size()));ns_msg reply{};if(::ns_initparse(answer.data(),count,&reply))throw Error{9502};
    const auto rcode=ns_msg_getflag(reply,ns_f_rcode);return rcode?9000U+static_cast<std::uint32_t>(rcode):0U;
}
}
