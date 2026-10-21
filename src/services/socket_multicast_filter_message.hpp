#pragma once
#include "socket_multicast_options.hpp"
#include "socket_multicast_filter.hpp"
#include <climits>

namespace x64arm::socket_detail {
inline bool multicast_filter_ioctl(std::uint32_t code){return code>=0x8004747cU && code<=0x8004747fU;}
inline bool multicast_filter_ioctl_get(std::uint32_t code){return code==0x8004747cU || code==0x8004747fU;}
inline bool multicast_filter_ioctl_generic(std::uint32_t code){return code==0x8004747eU || code==0x8004747fU;}
struct MulticastFilterMessage{sockaddr_storage group{};std::uint32_t interface{},capacity{};MulticastFilter filter;};
inline int multicast_filter_index4(in_addr address,const sockaddr_storage& group,std::uint32_t& index){
    const auto number=ntohl(address.s_addr);if(number && number<0x1000000U){index=number;return 0;}
    if(!number){
        // 預設接收介面由路由決定，不能套用 socket 的傳送介面設定。
        const int probe=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(probe<0)return -1;
        auto target=reinterpret_cast<const sockaddr_in&>(group);target.sin_port=htons(9);sockaddr_in local{};socklen_t length=sizeof local;
        const auto status=::connect(probe,reinterpret_cast<sockaddr*>(&target),sizeof target);int error=status<0?errno:0;
        if(!error && ::getsockname(probe,reinterpret_cast<sockaddr*>(&local),&length)<0)error=errno;
        ::close(probe);if(error){errno=error;return -1;}address=local.sin_addr;
    }
    ifaddrs* list{};if(::getifaddrs(&list)<0)return -1;index=0;
    for(const auto* item=list;item;item=item->ifa_next)if(item->ifa_addr && item->ifa_addr->sa_family==AF_INET && reinterpret_cast<const sockaddr_in*>(item->ifa_addr)->sin_addr.s_addr==address.s_addr){index=::if_nametoindex(item->ifa_name);break;}
    ::freeifaddrs(list);if(!index){errno=EADDRNOTAVAIL;return -1;}return 0;
}
inline int decode_multicast_filter(std::span<const std::uint8_t> bytes,bool generic,bool setting,MulticastFilterMessage& result){
    const unsigned header=generic?144U:16U,stride=generic?128U:4U;
    if(bytes.size()<header){errno=EFAULT;return -1;}
    const auto mode=multicast_word(bytes,generic?136:8),count=multicast_word(bytes,generic?140:12);
    if(count>65536){errno=ENOBUFS;return -1;}
    if(setting && (mode>1 || bytes.size()<header+std::size_t(count)*stride)){errno=mode>1?EINVAL:EFAULT;return -1;}
    MulticastFilterMessage decoded;decoded.capacity=count;decoded.filter.mode=mode==0?MCAST_INCLUDE:MCAST_EXCLUDE;
    if(generic){const auto family=unsigned(bytes[8])|(unsigned(bytes[9])<<8);const int native=family==2?AF_INET:family==23?AF_INET6:0;
        if(!native || multicast_address(bytes.subspan(8,128),decoded.group,native)<0){errno=EAFNOSUPPORT;return -1;}decoded.interface=multicast_word(bytes);
    }else{auto& address=reinterpret_cast<sockaddr_in&>(decoded.group);address.sin_len=sizeof address;address.sin_family=AF_INET;std::memcpy(&address.sin_addr,bytes.data(),4);
        in_addr interface{};std::memcpy(&interface,bytes.data()+4,4);if(multicast_filter_index4(interface,decoded.group,decoded.interface)<0)return -1;}
    if(decoded.group.ss_family==AF_INET){if(!IN_MULTICAST(ntohl(reinterpret_cast<const sockaddr_in&>(decoded.group).sin_addr.s_addr))){errno=EINVAL;return -1;}}
    else if(!IN6_IS_ADDR_MULTICAST(&reinterpret_cast<const sockaddr_in6&>(decoded.group).sin6_addr)){errno=EINVAL;return -1;}
    if(setting){decoded.filter.sources.reserve(count);for(unsigned i=0;i<count;++i){sockaddr_storage source{};const auto offset=header+std::size_t(i)*stride;
            if(generic){if(multicast_address(bytes.subspan(offset,128),source,decoded.group.ss_family)<0)return -1;}
            else{auto& address=reinterpret_cast<sockaddr_in&>(source);address.sin_len=sizeof address;address.sin_family=AF_INET;std::memcpy(&address.sin_addr,bytes.data()+offset,4);}
            decoded.filter.sources.push_back(source);}}
    result=std::move(decoded);return 0;
}
inline int encode_multicast_filter(std::span<const std::uint8_t> input,bool generic,const MulticastFilterMessage& request,const MulticastFilter& actual,std::span<std::uint8_t> output,std::size_t& used){
    const unsigned header=generic?144U:16U,stride=generic?128U:4U;
    if(input.size()<header || output.size()<header+std::size_t(request.capacity)*stride){errno=EFAULT;return -1;}
    if(actual.mode!=MCAST_INCLUDE && actual.mode!=MCAST_EXCLUDE){errno=EPROTO;return -1;}
    const auto count=std::min<std::size_t>(request.capacity,actual.sources.size());
    for(std::size_t i=0;i<count;++i)if(actual.sources[i].ss_family!=request.group.ss_family){errno=EPROTO;return -1;}
    std::memcpy(output.data(),input.data(),header);multicast_store(output.subspan(generic?136:8,4),actual.mode==MCAST_INCLUDE?0U:1U);
    multicast_store(output.subspan(generic?140:12,4),static_cast<std::uint32_t>(actual.sources.size()));
    for(std::size_t i=0;i<count;++i){auto entry=output.subspan(header+i*stride,stride);const auto& source=actual.sources[i];
        if(generic){std::fill(entry.begin(),entry.end(),0);entry[0]=source.ss_family==AF_INET?2:23;
            if(source.ss_family==AF_INET)std::memcpy(entry.data()+4,&reinterpret_cast<const sockaddr_in&>(source).sin_addr,4);
            else{const auto& address=reinterpret_cast<const sockaddr_in6&>(source);std::memcpy(entry.data()+8,&address.sin6_addr,16);multicast_store(entry.subspan(24,4),address.sin6_scope_id);}}
        else std::memcpy(entry.data(),&reinterpret_cast<const sockaddr_in&>(source).sin_addr,4);}
    used=header+count*stride;return 0;
}
} // namespace x64arm::socket_detail
