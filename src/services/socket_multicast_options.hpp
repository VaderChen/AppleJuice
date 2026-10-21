#pragma once
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <span>
#include <sys/socket.h>

namespace x64arm::socket_detail {
enum class MulticastKind {None,Membership4,Membership6,Source4,Group,GroupSource,Interface4,Interface6,Ttl4,Hops6};
struct MulticastOption {int level{},option{};MulticastKind kind{};std::size_t size{};};
inline bool multicast_membership(MulticastKind kind){return kind>=MulticastKind::Membership4 && kind<=MulticastKind::GroupSource;}
inline MulticastOption multicast_option(std::uint32_t level,std::uint32_t option,int family,int type){
    if(type!=SOCK_DGRAM && type!=SOCK_RAW)return {};
    if(!((level==0 && family==AF_INET) || (level==41 && family==AF_INET6)))return {};
    const auto native_level=level==0?IPPROTO_IP:IPPROTO_IPV6;
    if(option==41)return {native_level,MCAST_JOIN_GROUP,MulticastKind::Group,136};
    if(option==42)return {native_level,MCAST_LEAVE_GROUP,MulticastKind::Group,136};
    if(option==43)return {native_level,MCAST_BLOCK_SOURCE,MulticastKind::GroupSource,264};
    if(option==44)return {native_level,MCAST_UNBLOCK_SOURCE,MulticastKind::GroupSource,264};
    if(option==45)return {native_level,MCAST_JOIN_SOURCE_GROUP,MulticastKind::GroupSource,264};
    if(option==46)return {native_level,MCAST_LEAVE_SOURCE_GROUP,MulticastKind::GroupSource,264};
    if(family==AF_INET){
        switch(option){
        case 9:return {IPPROTO_IP,IP_MULTICAST_IF,MulticastKind::Interface4,4};
        case 10:return {IPPROTO_IP,IP_MULTICAST_TTL,MulticastKind::Ttl4,4};
        case 12:return {IPPROTO_IP,IP_ADD_MEMBERSHIP,MulticastKind::Membership4,8};
        case 13:return {IPPROTO_IP,IP_DROP_MEMBERSHIP,MulticastKind::Membership4,8};
        case 15:return {IPPROTO_IP,IP_ADD_SOURCE_MEMBERSHIP,MulticastKind::Source4,12};
        case 16:return {IPPROTO_IP,IP_DROP_SOURCE_MEMBERSHIP,MulticastKind::Source4,12};
        case 17:return {IPPROTO_IP,IP_BLOCK_SOURCE,MulticastKind::Source4,12};
        case 18:return {IPPROTO_IP,IP_UNBLOCK_SOURCE,MulticastKind::Source4,12};
        default:return {};
        }
    }
    switch(option){
    case 9:return {IPPROTO_IPV6,IPV6_MULTICAST_IF,MulticastKind::Interface6,4};
    case 10:return {IPPROTO_IPV6,IPV6_MULTICAST_HOPS,MulticastKind::Hops6,4};
    case 12:return {IPPROTO_IPV6,IPV6_JOIN_GROUP,MulticastKind::Membership6,20};
    case 13:return {IPPROTO_IPV6,IPV6_LEAVE_GROUP,MulticastKind::Membership6,20};
    default:return {};
    }
}
inline std::uint32_t multicast_word(std::span<const std::uint8_t> bytes,std::size_t offset=0){
    return std::uint32_t{bytes[offset]}|(std::uint32_t{bytes[offset+1]}<<8)|(std::uint32_t{bytes[offset+2]}<<16)|(std::uint32_t{bytes[offset+3]}<<24);
}
inline void multicast_store(std::span<std::uint8_t> bytes,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[i]=static_cast<std::uint8_t>(value>>(i*8));}
inline int multicast_interface_address(in_addr& address){
    const auto index=ntohl(address.s_addr);if(!index || index>=0x1000000U)return 0;
    char name[IF_NAMESIZE];if(!::if_indextoname(index,name)){errno=EADDRNOTAVAIL;return -1;}
    ifaddrs* list{};if(::getifaddrs(&list)<0)return -1;
    bool found=false;for(const auto* item=list;item;item=item->ifa_next)if(item->ifa_addr && item->ifa_addr->sa_family==AF_INET && !std::strcmp(item->ifa_name,name)){
        address=reinterpret_cast<const sockaddr_in*>(item->ifa_addr)->sin_addr;found=true;break;
    }
    ::freeifaddrs(list);if(!found){errno=EADDRNOTAVAIL;return -1;}return 0;
}
inline int multicast_address(std::span<const std::uint8_t> bytes,sockaddr_storage& storage,int family){
    const auto guest=unsigned{bytes[0]}|(unsigned{bytes[1]}<<8);
    if(family==AF_INET && guest==2){
        auto& address=reinterpret_cast<sockaddr_in&>(storage);address.sin_family=AF_INET;
#if defined(__APPLE__)
        address.sin_len=sizeof address;
#endif
        std::memcpy(&address.sin_port,bytes.data()+2,2);std::memcpy(&address.sin_addr,bytes.data()+4,4);return 0;
    }
    if(family==AF_INET6 && guest==23){
        auto& address=reinterpret_cast<sockaddr_in6&>(storage);address.sin6_family=AF_INET6;
#if defined(__APPLE__)
        address.sin6_len=sizeof address;
#endif
        std::memcpy(&address.sin6_port,bytes.data()+2,2);std::memcpy(&address.sin6_flowinfo,bytes.data()+4,4);
        std::memcpy(&address.sin6_addr,bytes.data()+8,16);address.sin6_scope_id=multicast_word(bytes,24);return 0;
    }
    errno=EAFNOSUPPORT;return -1;
}
inline int multicast_set(int fd,MulticastOption option,int family,std::span<const std::uint8_t> bytes){
    if(bytes.size()<option.size){errno=EFAULT;return -1;}
    switch(option.kind){
    case MulticastKind::Membership4:{ip_mreq request{};std::memcpy(&request.imr_multiaddr,bytes.data(),4);std::memcpy(&request.imr_interface,bytes.data()+4,4);if(multicast_interface_address(request.imr_interface)<0)return -1;return ::setsockopt(fd,option.level,option.option,&request,sizeof request);}
    case MulticastKind::Membership6:{ipv6_mreq request{};std::memcpy(&request.ipv6mr_multiaddr,bytes.data(),16);request.ipv6mr_interface=multicast_word(bytes,16);return ::setsockopt(fd,option.level,option.option,&request,sizeof request);}
    case MulticastKind::Source4:{ip_mreq_source request{};std::memcpy(&request.imr_multiaddr,bytes.data(),4);std::memcpy(&request.imr_sourceaddr,bytes.data()+4,4);std::memcpy(&request.imr_interface,bytes.data()+8,4);if(multicast_interface_address(request.imr_interface)<0)return -1;return ::setsockopt(fd,option.level,option.option,&request,sizeof request);}
    case MulticastKind::Group:{group_req request{};request.gr_interface=multicast_word(bytes);if(multicast_address(bytes.subspan(8,128),request.gr_group,family)<0)return -1;return ::setsockopt(fd,option.level,option.option,&request,sizeof request);}
    case MulticastKind::GroupSource:{group_source_req request{};request.gsr_interface=multicast_word(bytes);if(multicast_address(bytes.subspan(8,128),request.gsr_group,family)<0 || multicast_address(bytes.subspan(136,128),request.gsr_source,family)<0)return -1;return ::setsockopt(fd,option.level,option.option,&request,sizeof request);}
    case MulticastKind::Interface4:{in_addr address{};std::memcpy(&address,bytes.data(),4);if(multicast_interface_address(address)<0)return -1;return ::setsockopt(fd,option.level,option.option,&address,sizeof address);}
    case MulticastKind::Interface6:{const auto index=multicast_word(bytes);return ::setsockopt(fd,option.level,option.option,&index,sizeof index);}
    case MulticastKind::Ttl4:case MulticastKind::Hops6:{const auto value=multicast_word(bytes);if(value>255){errno=EINVAL;return -1;}
        if(option.kind==MulticastKind::Ttl4){const auto ttl=static_cast<unsigned char>(value);return ::setsockopt(fd,option.level,option.option,&ttl,sizeof ttl);}
        const auto hops=static_cast<int>(value);return ::setsockopt(fd,option.level,option.option,&hops,sizeof hops);}
    default:errno=ENOPROTOOPT;return -1;
    }
}
inline int multicast_get(int fd,MulticastOption option,std::span<std::uint8_t> bytes){
    if(multicast_membership(option.kind)){errno=ENOPROTOOPT;return -1;}
    if(bytes.size()<4){errno=EFAULT;return -1;}
    unsigned value=0;socklen_t size=sizeof value;
    if(option.kind==MulticastKind::Ttl4){unsigned char ttl{};size=sizeof ttl;if(::getsockopt(fd,option.level,option.option,&ttl,&size)<0)return -1;value=ttl;}
    else if(option.kind==MulticastKind::Interface4){
#if defined(IP_MULTICAST_IFINDEX)
        if(::getsockopt(fd,IPPROTO_IP,IP_MULTICAST_IFINDEX,&value,&size)<0)return -1;
#else
        in_addr address{};size=sizeof address;if(::getsockopt(fd,IPPROTO_IP,IP_MULTICAST_IF,&address,&size)<0)return -1;
        if(address.s_addr){ifaddrs* list{};if(::getifaddrs(&list)<0)return -1;for(const auto* item=list;item;item=item->ifa_next)if(item->ifa_addr && item->ifa_addr->sa_family==AF_INET && reinterpret_cast<const sockaddr_in*>(item->ifa_addr)->sin_addr.s_addr==address.s_addr){value=::if_nametoindex(item->ifa_name);break;}::freeifaddrs(list);if(!value){errno=EADDRNOTAVAIL;return -1;}}
#endif
    }else if(option.kind==MulticastKind::Interface6 || option.kind==MulticastKind::Hops6){if(::getsockopt(fd,option.level,option.option,&value,&size)<0)return -1;}
    else {errno=ENOPROTOOPT;return -1;}
    multicast_store(bytes,value);return 0;
}
} // namespace x64arm::socket_detail
