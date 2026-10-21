#pragma once
#include "x64arm/memory.hpp"
#include "socket_send_control.hpp"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <vector>
#include <span>
namespace x64arm::socket_detail {
inline std::uint32_t socket_error(int error) {
    switch (error) {
    case 0:return 0;case EINTR:return 10004;case EACCES:case EPERM:return 10013;case EFAULT:return 10014;
    case EINVAL:return 10022;case EMFILE:case ENFILE:return 10024;case EAGAIN:return 10035;
    case EINPROGRESS:return 10036;case EALREADY:return 10037;case ENOTSOCK:case EBADF:return 10038;
    case EDESTADDRREQ:return 10039;case EMSGSIZE:return 10040;case EPROTOTYPE:return 10041;
    case ENOPROTOOPT:return 10042;case EPROTONOSUPPORT:return 10043;case EOPNOTSUPP:return 10045;
    case EAFNOSUPPORT:return 10047;case EADDRINUSE:return 10048;case EADDRNOTAVAIL:return 10049;
    case ENETDOWN:return 10050;case ENETUNREACH:return 10051;case ENETRESET:return 10052;
    case ECONNABORTED:return 10053;case ECONNRESET:case EPIPE:return 10054;
    case ENOBUFS:case ENOMEM:return 10055;case EISCONN:return 10056;case ENOTCONN:return 10057;
    case ETIMEDOUT:return 10060;case ECONNREFUSED:return 10061;case EHOSTUNREACH:return 10065;
    default:return 10022;
    }
}
inline int copy_accept_options(int source,int target){
    for(const auto option:{SO_KEEPALIVE,SO_OOBINLINE,SO_LINGER,SO_RCVTIMEO,SO_SNDTIMEO,SO_RCVBUF,SO_SNDBUF}){
        std::array<std::uint8_t,64> value{};socklen_t size=value.size();
        if(::getsockopt(source,SOL_SOCKET,option,value.data(),&size)<0 || ::setsockopt(target,SOL_SOCKET,option,value.data(),size)<0)return -1;
    }
    int value=0;socklen_t size=sizeof(value);if(::getsockopt(source,IPPROTO_TCP,TCP_NODELAY,&value,&size)<0 || ::setsockopt(target,IPPROTO_TCP,TCP_NODELAY,&value,size)<0)return -1;
    const auto flags=::fcntl(source,F_GETFL),target_flags=::fcntl(target,F_GETFL);if(flags<0 || target_flags<0 || ::fcntl(target,F_SETFL,(target_flags&~O_NONBLOCK)|(flags&O_NONBLOCK))<0)return -1;
    return 0;
}
struct Address {sockaddr_storage storage{};socklen_t size{};};
inline bool same_endpoint(const sockaddr* first,const sockaddr* second){
    if(first->sa_family!=second->sa_family)return false;
    if(first->sa_family==AF_INET){
        const auto& a=*reinterpret_cast<const sockaddr_in*>(first);
        const auto& b=*reinterpret_cast<const sockaddr_in*>(second);
        return a.sin_port==b.sin_port && a.sin_addr.s_addr==b.sin_addr.s_addr;
    }
    if(first->sa_family==AF_INET6){
        const auto& a=*reinterpret_cast<const sockaddr_in6*>(first);
        const auto& b=*reinterpret_cast<const sockaddr_in6*>(second);
        return a.sin6_port==b.sin6_port && a.sin6_scope_id==b.sin6_scope_id &&
            std::memcmp(&a.sin6_addr,&b.sin6_addr,sizeof(a.sin6_addr))==0;
    }
    return false;
}
#ifdef __APPLE__
inline ssize_t send_datagram_override(int descriptor,const msghdr& original,int flags){
    sockaddr_storage local{};socklen_t local_size=sizeof(local);
    if(::getsockname(descriptor,reinterpret_cast<sockaddr*>(&local),&local_size)<0)return -1;
    if(local.ss_family!=AF_INET && local.ss_family!=AF_INET6){errno=EAFNOSUPPORT;return -1;}
    int pending_error=0;socklen_t error_size=sizeof(pending_error);
    if(::getsockopt(descriptor,SOL_SOCKET,SO_ERROR,&pending_error,&error_size)<0)return -1;
    if(pending_error){errno=pending_error;return -1;}
    struct Descriptor {
        int value;
        ~Descriptor(){if(value>=0){const auto saved=errno;::close(value);errno=saved;}}
    } outgoing{::socket(local.ss_family,SOCK_DGRAM,IPPROTO_UDP)};
    if(outgoing.value<0)return -1;
    if(::fcntl(outgoing.value,F_SETFD,FD_CLOEXEC)<0)return -1;
    const int reuse=1;
    // SO_REUSEPORT 是內部傳送別名所需的選項，Winsock 不暴露此原生選項。
    // 原 socket 保持連線；接收仍由原 peer 的完整 tuple 匹配，不解除連線。
    if(::setsockopt(descriptor,SOL_SOCKET,SO_REUSEPORT,&reuse,sizeof(reuse))<0 ||
       ::setsockopt(outgoing.value,SOL_SOCKET,SO_REUSEPORT,&reuse,sizeof(reuse))<0)return -1;
    for(const auto option:{SO_REUSEADDR,SO_KEEPALIVE,SO_DONTROUTE,SO_BROADCAST,SO_LINGER,
            SO_OOBINLINE,SO_SNDBUF,SO_RCVBUF,SO_SNDTIMEO,SO_RCVTIMEO,SO_NOSIGPIPE}){
        std::array<std::uint8_t,64> value{};socklen_t size=value.size();
        if(::getsockopt(descriptor,SOL_SOCKET,option,value.data(),&size)<0 ||
           ::setsockopt(outgoing.value,SOL_SOCKET,option,value.data(),size)<0)return -1;
    }
    if(local.ss_family==AF_INET6){
        int only=0;socklen_t size=sizeof(only);
        if(::getsockopt(descriptor,IPPROTO_IPV6,IPV6_V6ONLY,&only,&size)<0 ||
           ::setsockopt(outgoing.value,IPPROTO_IPV6,IPV6_V6ONLY,&only,size)<0)return -1;
    }
    const auto source_flags=::fcntl(descriptor,F_GETFL),outgoing_flags=::fcntl(outgoing.value,F_GETFL);
    if(source_flags<0 || outgoing_flags<0 ||
       ::fcntl(outgoing.value,F_SETFL,(outgoing_flags&~O_NONBLOCK)|(source_flags&O_NONBLOCK))<0)return -1;
    if(::bind(outgoing.value,reinterpret_cast<const sockaddr*>(&local),local_size)<0 ||
       ::connect(outgoing.value,static_cast<const sockaddr*>(original.msg_name),original.msg_namelen)<0)return -1;
    auto message=original;message.msg_name=nullptr;message.msg_namelen=0;
    // payload、scatter/gather、旗標與控制訊息全部交給真正的原生 sendmsg。
    return ::sendmsg(outgoing.value,&message,flags);
}
#endif
inline ssize_t send_message(int descriptor,int type,const msghdr& original,int flags,[[maybe_unused]] int protocol=IPPROTO_UDP){
    auto message=original;
    std::vector<std::uint8_t> converted;bool changed{};
    if(!prepare_send_control(descriptor,type,message,converted,changed))return -1;
    if(changed){message.msg_control=converted.data();message.msg_controllen=static_cast<decltype(message.msg_controllen)>(converted.size());}
    if(message.msg_name && (type==SOCK_STREAM || type==SOCK_DGRAM)){
        bool use_connected_peer=type==SOCK_STREAM;
        if(!use_connected_peer){
            sockaddr_storage peer{};socklen_t size=sizeof(peer);
            const auto* destination=static_cast<const sockaddr*>(message.msg_name);
            const auto required=message.msg_namelen>=sizeof(sockaddr) && destination->sa_family==AF_INET?sizeof(sockaddr_in):sizeof(sockaddr_in6);
            use_connected_peer=message.msg_namelen>=required &&
                ::getpeername(descriptor,reinterpret_cast<sockaddr*>(&peer),&size)==0 &&
                same_endpoint(destination,reinterpret_cast<const sockaddr*>(&peer));
        }
        // Winsock 允許已連線 UDP 附帶同一 peer；Darwin 會拒絕附帶位址。
        // 省略冗餘位址保留原連線、接收篩選、來源埠、旗標與 ancillary data。
        if(use_connected_peer){message.msg_name=nullptr;message.msg_namelen=0;}
    }
    const auto result=::sendmsg(descriptor,&message,flags);
#ifdef __APPLE__
    if(result<0 && errno==EISCONN && type==SOCK_DGRAM && (!protocol || protocol==IPPROTO_UDP) && message.msg_name)
        return send_datagram_override(descriptor,message,flags);
#endif
    return result;
}
inline Address native_address(std::span<const std::uint8_t> bytes) {
    Address result;
    if(bytes.size()==16 && bytes[0]==2){auto& value=*reinterpret_cast<sockaddr_in*>(&result.storage);value.sin_family=AF_INET;std::memcpy(&value.sin_port,bytes.data()+2,2);std::memcpy(&value.sin_addr,bytes.data()+4,4);result.size=sizeof(value);
#ifdef __APPLE__
        value.sin_len=sizeof(value);
#endif
    }else if(bytes.size()==28 && bytes[0]==23){auto& value=*reinterpret_cast<sockaddr_in6*>(&result.storage);value.sin6_family=AF_INET6;std::memcpy(&value.sin6_port,bytes.data()+2,2);std::memcpy(&value.sin6_flowinfo,bytes.data()+4,4);std::memcpy(&value.sin6_addr,bytes.data()+8,16);std::memcpy(&value.sin6_scope_id,bytes.data()+24,4);result.size=sizeof(value);
#ifdef __APPLE__
        value.sin6_len=sizeof(value);
#endif
    }
    return result;
}
inline Address read_address(const GuestMemory& memory,std::uint64_t pointer,unsigned size) {
    if(!pointer || size<2)return {};
    const auto family=memory.load(GuestAddress{pointer},16);const unsigned required=family==2?16:family==23?28:0;
    if(!required || size<required)return {};
    std::array<std::uint8_t,28> bytes{};memory.read(GuestAddress{pointer},std::span(bytes).first(required));
    return native_address(std::span(bytes).first(required));
}
inline std::vector<std::uint8_t> address_bytes(const sockaddr* address) {
    std::vector<std::uint8_t> bytes;
    if (address->sa_family==AF_INET) {
        const auto& value=*reinterpret_cast<const sockaddr_in*>(address);bytes.resize(16);bytes[0]=2;
        std::memcpy(bytes.data()+2,&value.sin_port,2);std::memcpy(bytes.data()+4,&value.sin_addr,4);
    } else if (address->sa_family==AF_INET6) {
        const auto& value=*reinterpret_cast<const sockaddr_in6*>(address);bytes.resize(28);bytes[0]=23;
        std::memcpy(bytes.data()+2,&value.sin6_port,2);std::memcpy(bytes.data()+4,&value.sin6_flowinfo,4);
        std::memcpy(bytes.data()+8,&value.sin6_addr,16);std::memcpy(bytes.data()+24,&value.sin6_scope_id,4);
    }
    return bytes;
}
}
