#include "socket_database.hpp"
#include "socket_address.hpp"
#include "x64arm/shared_wake.hpp"
#include <arpa/inet.h>
#include <netdb.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace x64arm::socket_database {
namespace {
std::uint32_t resolver_error(int error) {
    switch(error){
    case EAI_AGAIN:return 11002;case EAI_BADFLAGS:return 10022;case EAI_FAIL:return 11003;
    case EAI_FAMILY:return 10047;case EAI_MEMORY:return 8;case EAI_NONAME:return 11001;
    case EAI_SERVICE:return 10109;case EAI_SOCKTYPE:return 10044;case EAI_OVERFLOW:return 10014;
    case EAI_SYSTEM:return socket_detail::socket_error(errno);default:return 11003;
    }
}
std::mutex database_mutex;
std::uint32_t host_error(){switch(h_errno){case HOST_NOT_FOUND:return 11001;case TRY_AGAIN:return 11002;case NO_DATA:return 11004;default:return 11003;}}
void aliases(Result& out,char* const* names){
    for(auto item=names;item && *item;++item){if(out.aliases.size()==128){out.error=10055;return;}out.aliases.emplace_back(*item);}
}
struct Work {Query query;std::shared_ptr<Job> job;};
struct Worker {std::mutex mutex;std::condition_variable changed;std::deque<Work> queue;};
std::shared_ptr<Worker> worker(){
    // 舊式 Winsock 非同步查詢依序執行；worker 只持有原生資料，不接觸客體記憶體。
    static const auto shared=[](){auto value=std::make_shared<Worker>();std::thread([value]{for(;;){
        Work work;{std::unique_lock lock(value->mutex);value->changed.wait(lock,[&]{return !value->queue.empty();});work=std::move(value->queue.front());value->queue.pop_front();}
        if(work.job->cancelled.load())continue;
        Result result;try{result=lookup(work.query);}catch(const std::bad_alloc&){result.error=10055;}catch(...){result.error=11003;}
        if(!work.job->cancelled.load()){work.job->result=std::move(result);work.job->ready.store(true,std::memory_order_release);if(work.job->wake)work.job->wake->notify();}
    }}).detach();return value;}();return shared;
}
}
Result lookup(const Query& query){
    Result result;result.kind=query.kind;result.family=query.family;
    if(query.kind==Kind::AddressInfo){
        addrinfo hints{};hints.ai_family=static_cast<int>(query.family);hints.ai_flags=query.flags;hints.ai_socktype=query.socket_type;hints.ai_protocol=query.protocol_number;
        addrinfo* list=nullptr;const auto status=::getaddrinfo(query.has_name?query.name.c_str():nullptr,query.has_service?query.protocol.c_str():nullptr,&hints,&list);
        if(status)result.error=resolver_error(status);
        if(list)result.address_info=std::shared_ptr<addrinfo>(list,&::freeaddrinfo);return result;
    }
    if(query.kind==Kind::NameInfo){
        sockaddr_storage address{};if(query.address.size()>sizeof(address)){result.error=10014;return result;}std::memcpy(&address,query.address.data(),query.address.size());
        std::vector<char> host(query.number),service(query.service_capacity);
        const auto status=::getnameinfo(reinterpret_cast<const sockaddr*>(&address),static_cast<socklen_t>(query.address.size()),host.empty()?nullptr:host.data(),static_cast<socklen_t>(host.size()),service.empty()?nullptr:service.data(),static_cast<socklen_t>(service.size()),query.flags);
        if(status)result.error=resolver_error(status);else{if(!host.empty())result.name=host.data();if(!service.empty())result.protocol=service.data();}return result;
    }
    const std::lock_guard lock(database_mutex);
    if(query.kind==Kind::HostName || query.kind==Kind::HostAddress){
        const bool reverse=query.kind==Kind::HostAddress;const int family=query.family==2?AF_INET:query.family==23?AF_INET6:AF_UNSPEC;
        const auto length=family==AF_INET6?16U:4U;if(family==AF_UNSPEC){result.error=10047;return result;}
        std::string name=query.name;const bool local=!reverse && name.empty();
        if(local){std::array<char,256> hostname{};if(::gethostname(hostname.data(),hostname.size())<0){result.error=11003;return result;}hostname.back()=0;name=hostname.data();}
        if(!reverse){in_addr address{};if(::inet_aton(name.c_str(),&address)){result.name=name;const auto* bytes=reinterpret_cast<const std::uint8_t*>(&address);result.addresses.emplace_back(bytes,bytes+4);return result;}
            if(name.find(':')!=name.npos || (!name.empty() && name.find_first_not_of("0123456789.")==name.npos)){result.error=11004;return result;}}
        if(reverse && query.address.size()!=length){result.error=10014;return result;}
        const auto* info=reverse?::gethostbyaddr(query.address.data(),length,family) : ::gethostbyname(name.c_str());
        if(!info){result.error=host_error();return result;}if(info->h_addrtype!=family || info->h_length!=static_cast<int>(length)){result.error=11004;return result;}
        result.name=local?name:info->h_name?info->h_name:"";aliases(result,info->h_aliases);
        for(auto** item=info->h_addr_list;item && *item;++item){if(result.addresses.size()==128){result.error=10055;break;}const auto* bytes=reinterpret_cast<const std::uint8_t*>(*item);result.addresses.emplace_back(bytes,bytes+length);}
        if(result.addresses.empty() && !result.error)result.error=11004;
    }else if(query.kind==Kind::ProtocolName || query.kind==Kind::ProtocolNumber){
        const auto* info=query.kind==Kind::ProtocolName?::getprotobyname(query.name.c_str()) : ::getprotobynumber(static_cast<int>(query.number));
        if(!info){result.error=11004;return result;}result.name=info->p_name?info->p_name:"";result.number=static_cast<std::uint32_t>(info->p_proto);aliases(result,info->p_aliases);
    }else{
        const auto* protocol=query.has_protocol?query.protocol.c_str():nullptr;
        const auto* info=query.kind==Kind::ServiceName?::getservbyname(query.name.c_str(),protocol) : ::getservbyport(static_cast<int>(query.number&65535U),protocol);
        if(!info){result.error=11004;return result;}result.name=info->s_name?info->s_name:"";result.protocol=info->s_proto?info->s_proto:"";result.number=static_cast<std::uint16_t>(info->s_port);aliases(result,info->s_aliases);
    }
    return result;
}
std::shared_ptr<Job> submit(Query query,std::shared_ptr<SharedWake> wake){
    const auto shared=worker();auto job=std::make_shared<Job>();job->wake=std::move(wake);const std::lock_guard lock(shared->mutex);
    std::erase_if(shared->queue,[](const auto& work){return work.job->cancelled.load();});if(shared->queue.size()>=64)return {};
    shared->queue.push_back({std::move(query),job});shared->changed.notify_one();return job;
}
std::vector<std::uint8_t> pack(const Result& result,std::uint64_t base,bool x86){
    const bool host=result.kind==Kind::HostName || result.kind==Kind::HostAddress,protocol=result.kind==Kind::ProtocolName || result.kind==Kind::ProtocolNumber;
    const unsigned unit=x86?4:8,header=host?(x86?16:32):protocol?(x86?12:24):(x86?16:32);
    const auto address_table=header+(result.aliases.size()+1)*unit,tables=address_table+(host?(result.addresses.size()+1)*unit:0);
    std::vector<std::uint8_t> bytes(tables);const auto put=[&](std::size_t at,std::uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));};
    const auto text=[&](std::size_t at,const std::string& value){put(at,base+bytes.size(),unit);bytes.insert(bytes.end(),value.begin(),value.end());bytes.push_back(0);};
    text(0,result.name);put(unit,base+header,unit);for(std::size_t i=0;i<result.aliases.size();++i)text(header+i*unit,result.aliases[i]);
    if(host){put(2*unit,result.family,2);put(2*unit+2,result.family==23?16:4,2);put(x86?12:24,base+address_table,unit);for(std::size_t i=0;i<result.addresses.size();++i){put(address_table+i*unit,base+bytes.size(),unit);bytes.insert(bytes.end(),result.addresses[i].begin(),result.addresses[i].end());}}
    else if(protocol)put(2*unit,result.number,2);
    else{put(x86?8:24,result.number,2);text(x86?12:16,result.protocol);}
    if(bytes.size()>65535 || (x86 && base>UINT32_MAX-bytes.size()) || base>UINT64_MAX-bytes.size())throw std::length_error("Winsock 查詢結果超過可表示範圍");return bytes;
}
}
