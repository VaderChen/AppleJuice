#pragma once
#include "socket_state_access.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <vector>
#include <utility>
#include <unistd.h>

namespace x64arm::socket_detail {
// 原生 sourcefilter 的清單由核心保存，dup／跨程序移交仍共用同一份狀態。
struct MulticastFilter {
    std::uint32_t mode{};
    std::vector<sockaddr_storage> sources;
};
inline bool multicast_same_source(const sockaddr_storage& first,const sockaddr_storage& second){
    if(first.ss_family!=second.ss_family)return false;
    if(first.ss_family==AF_INET)return reinterpret_cast<const sockaddr_in&>(first).sin_addr.s_addr==reinterpret_cast<const sockaddr_in&>(second).sin_addr.s_addr;
    if(first.ss_family==AF_INET6)return !std::memcmp(&reinterpret_cast<const sockaddr_in6&>(first).sin6_addr,&reinterpret_cast<const sockaddr_in6&>(second).sin6_addr,sizeof(in6_addr));
    return false;
}
inline bool multicast_filter_permits(const MulticastFilter& filter,const sockaddr_storage& source){
    bool found=false;for(const auto& entry:filter.sources)if(multicast_same_source(entry,source)){found=true;break;}
    return filter.mode==MCAST_INCLUDE?found:filter.mode==MCAST_EXCLUDE && !found;
}
inline int multicast_read_filter(int fd,unsigned index,sockaddr_storage group,MulticastFilter& filter){
    using Query=int(*)(int,std::uint32_t,sockaddr*,socklen_t,std::uint32_t*,std::uint32_t*,sockaddr_storage*);
    // Darwin SDK 的舊 C++ 宣告缺少 extern C；以公開 C 符號及固定 ABI 呼叫。
    static const auto query=reinterpret_cast<Query>(::dlsym(RTLD_DEFAULT,"getsourcefilter"));
    if(!query){errno=ENOSYS;return -1;}
    const auto size=group.ss_family==AF_INET?sizeof(sockaddr_in):group.ss_family==AF_INET6?sizeof(sockaddr_in6):0;
    if(!size){errno=EAFNOSUPPORT;return -1;}
    // XNU 的零容量查詢不回傳總數；以有容量的查詢取得完整清單，飽和時擴大。
    for(std::uint32_t capacity=16;capacity<=65536;capacity*=2){
        std::vector<sockaddr_storage> sources(capacity);std::uint32_t count=capacity,mode=0;
        if(query(fd,index,reinterpret_cast<sockaddr*>(&group),static_cast<socklen_t>(size),&mode,&count,sources.data())<0)return -1;
        if(count>capacity || (mode!=MCAST_INCLUDE && mode!=MCAST_EXCLUDE)){errno=EPROTO;return -1;}
        if(count<capacity){sources.resize(count);filter={mode,std::move(sources)};return 0;}
    }
    errno=ENOBUFS;return -1;
}
inline int multicast_native_filter(int fd,unsigned index,sockaddr_storage group,const MulticastFilter& filter){
    using Set=int(*)(int,std::uint32_t,const sockaddr*,socklen_t,std::uint32_t,std::uint32_t,const sockaddr_storage*);
    static const auto set=reinterpret_cast<Set>(::dlsym(RTLD_DEFAULT,"setsourcefilter"));
    if(!set){errno=ENOSYS;return -1;}
    const auto size=group.ss_family==AF_INET?sizeof(sockaddr_in):group.ss_family==AF_INET6?sizeof(sockaddr_in6):0;
    if(!size){errno=EAFNOSUPPORT;return -1;}
    return set(fd,index,reinterpret_cast<const sockaddr*>(&group),static_cast<socklen_t>(size),filter.mode,static_cast<std::uint32_t>(filter.sources.size()),filter.sources.empty()?nullptr:filter.sources.data());
}
inline int multicast_apply_filter(int fd,unsigned index,const sockaddr_storage& group,const MulticastFilter& current,const MulticastFilter& target){
    // Darwin 的完整設定在零來源時沒有移除舊清單；先用實際 delta 操作移除。
    // 不添加虛構來源，也不解除／重新加入整個群組。
    if(target.sources.empty())for(const auto& source:current.sources){
        group_source_req request{};request.gsr_interface=index;request.gsr_group=group;request.gsr_source=source;
        const int option=current.mode==MCAST_EXCLUDE?MCAST_UNBLOCK_SOURCE:MCAST_LEAVE_SOURCE_GROUP;
        if(::setsockopt(fd,group.ss_family==AF_INET?IPPROTO_IP:IPPROTO_IPV6,option,&request,sizeof request)<0)return -1;
    }
    return multicast_native_filter(fd,index,group,target);
}
inline bool multicast_filter_equal(const MulticastFilter& first,const MulticastFilter& second){
    if(first.mode!=second.mode || first.sources.size()!=second.sources.size())return false;
    for(const auto& source:first.sources)if(std::none_of(second.sources.begin(),second.sources.end(),[&](const auto& item){return multicast_same_source(source,item);}))return false;
    return true;
}
// 呼叫者須持有共用 socket 鎖，並先提交原生佇列；成功必須有真正核心讀回。
inline int multicast_write_filter(int fd,unsigned index,sockaddr_storage group,MulticastFilter target){
    if(target.mode!=MCAST_INCLUDE && target.mode!=MCAST_EXCLUDE){errno=EINVAL;return -1;}
    if(target.sources.size()>65536){errno=ENOBUFS;return -1;}
    if(group.ss_family!=AF_INET && group.ss_family!=AF_INET6){errno=EAFNOSUPPORT;return -1;}
    if(group.ss_family==AF_INET)reinterpret_cast<sockaddr_in&>(group).sin_port=0;else reinterpret_cast<sockaddr_in6&>(group).sin6_port=0;
    for(auto& source:target.sources){if(source.ss_family!=group.ss_family){errno=EAFNOSUPPORT;return -1;}
        if(source.ss_family==AF_INET)reinterpret_cast<sockaddr_in&>(source).sin_port=0;else reinterpret_cast<sockaddr_in6&>(source).sin6_port=0;}
    // 核心忽略重複位址；先正規化，讀回核對不會把順序或重複數當成內容差異。
    const auto less=[](const sockaddr_storage& a,const sockaddr_storage& b){
        if(a.ss_family==AF_INET)return ntohl(reinterpret_cast<const sockaddr_in&>(a).sin_addr.s_addr)<ntohl(reinterpret_cast<const sockaddr_in&>(b).sin_addr.s_addr);
        return std::memcmp(&reinterpret_cast<const sockaddr_in6&>(a).sin6_addr,&reinterpret_cast<const sockaddr_in6&>(b).sin6_addr,16)<0;
    };
    std::sort(target.sources.begin(),target.sources.end(),less);
    target.sources.erase(std::unique(target.sources.begin(),target.sources.end(),multicast_same_source),target.sources.end());
    MulticastFilter before;if(multicast_read_filter(fd,index,group,before)<0)return -1;
    if(multicast_apply_filter(fd,index,group,before,target)<0){const auto error=errno;
        // 非空完整設定由核心原子提交；部分清空失敗仍須恢復原清單。
        if(target.sources.empty() && multicast_native_filter(fd,index,group,before)<0)return -1;
        errno=error;return -1;}
    int error=0;
    try{MulticastFilter after;if(multicast_read_filter(fd,index,group,after)<0)error=errno;else if(!multicast_filter_equal(target,after))error=EIO;}
    catch(const std::bad_alloc&){error=ENOMEM;}
    if(!error)return 0;
    if(multicast_apply_filter(fd,index,group,target,before)<0)return -1;
    errno=error;return -1;
}

using MulticastAccess=SocketStateAccess;
inline int multicast_receive_metadata(int fd,int family){
    const int enabled=1;
    if(family==AF_INET){
        if(::setsockopt(fd,IPPROTO_IP,IP_RECVDSTADDR,&enabled,sizeof enabled)<0)return -1;
        return ::setsockopt(fd,IPPROTO_IP,IP_RECVIF,&enabled,sizeof enabled);
    }
    if(family==AF_INET6)return ::setsockopt(fd,IPPROTO_IPV6,61,&enabled,sizeof enabled); // SDK IPV6_RECVPKTINFO
    errno=EAFNOSUPPORT;return -1;
}
struct MulticastPacket {
    sockaddr_storage group{};
    unsigned interface{};
    bool multicast{},complete{};
};
inline MulticastPacket multicast_packet(msghdr& message){
    MulticastPacket packet;
    for(auto* control=CMSG_FIRSTHDR(&message);control;control=CMSG_NXTHDR(&message,control)){
        if(control->cmsg_len<CMSG_LEN(0))continue;
        const auto size=control->cmsg_len-CMSG_LEN(0);
        if(control->cmsg_level==IPPROTO_IP && control->cmsg_type==IP_RECVDSTADDR && size>=sizeof(in_addr)){
            auto& group=reinterpret_cast<sockaddr_in&>(packet.group);group.sin_len=sizeof group;group.sin_family=AF_INET;
            std::memcpy(&group.sin_addr,CMSG_DATA(control),sizeof group.sin_addr);packet.multicast=IN_MULTICAST(ntohl(group.sin_addr.s_addr));
        }else if(control->cmsg_level==IPPROTO_IP && control->cmsg_type==IP_RECVIF && size>=offsetof(sockaddr_dl,sdl_data)){
            sockaddr_dl address{};std::memcpy(&address,CMSG_DATA(control),std::min(size,sizeof address));packet.interface=address.sdl_index;
        }else if(control->cmsg_level==IPPROTO_IPV6 && (control->cmsg_type==19 || control->cmsg_type==46) && size>=20){
            auto& group=reinterpret_cast<sockaddr_in6&>(packet.group);group.sin6_len=sizeof group;group.sin6_family=AF_INET6;
            std::memcpy(&group.sin6_addr,CMSG_DATA(control),16);std::memcpy(&packet.interface,CMSG_DATA(control)+16,4);
            group.sin6_scope_id=packet.interface;packet.multicast=IN6_IS_ADDR_MULTICAST(&group.sin6_addr);
        }
    }
    packet.complete=packet.group.ss_family!=0 && packet.interface!=0;return packet;
}

// 每次至多丟棄 128 個不允許的封包。EINPROGRESS 表示尚須協同重試，
// 與 EAGAIN（核心當前沒有資料）分開；呼叫方不得把它當成 poll 未就緒。
inline ssize_t multicast_receive_message(int fd,int state_fd,msghdr& output,int flags){
    MulticastAccess access(state_fd);if(!access)return -1;
    for(unsigned discarded=0;discarded<128;++discarded){
        sockaddr_storage source{};alignas(cmsghdr) std::array<unsigned char,1024> controls{};
        msghdr message=output;message.msg_name=&source;message.msg_namelen=sizeof source;message.msg_control=controls.data();message.msg_controllen=controls.size();
        const auto count=::recvmsg(fd,&message,flags|MSG_DONTWAIT);if(count<0)return -1;
        const auto packet=multicast_packet(message);
        if(!packet.complete){errno=EPROTO;return -1;}
        bool allowed=true;
        if(packet.multicast){MulticastFilter filter;if(multicast_read_filter(fd,packet.interface,packet.group,filter)<0)return -1;allowed=multicast_filter_permits(filter,source);}
        if(!allowed){
            if(flags&MSG_PEEK){unsigned char byte{};iovec vector{&byte,1};msghdr consume{};consume.msg_iov=&vector;consume.msg_iovlen=1;if(::recvmsg(fd,&consume,MSG_DONTWAIT)<0)return -1;}
            continue;
        }
        if(output.msg_name)std::memcpy(output.msg_name,&source,std::min<std::size_t>(output.msg_namelen,message.msg_namelen));
        output.msg_namelen=message.msg_namelen;output.msg_flags=message.msg_flags;
        // 原生的內部目的位址／介面資料不直接複製成客體控制訊息。
        // Windows WSARecvMsg 的控制資料匯出在整合時由共用 codec 處理。
        const auto capacity=output.msg_controllen;
        if(output.msg_control)std::memcpy(output.msg_control,controls.data(),std::min<std::size_t>(capacity,message.msg_controllen));
        output.msg_controllen=static_cast<socklen_t>(std::min<std::size_t>(capacity,message.msg_controllen));
        if(capacity<message.msg_controllen && capacity)output.msg_flags|=MSG_CTRUNC;
        return count;
    }
    errno=EINPROGRESS;return -1;
}
enum class MulticastReadiness {Ready,Empty,Deferred,Error};
inline MulticastReadiness multicast_readable(int fd,int state_fd){
    unsigned char byte{};iovec vector{&byte,1};msghdr message{};message.msg_iov=&vector;message.msg_iovlen=1;
    if(multicast_receive_message(fd,state_fd,message,MSG_PEEK)>=0)return MulticastReadiness::Ready;
    if(errno==EAGAIN || errno==EWOULDBLOCK)return MulticastReadiness::Empty;
    return errno==EINPROGRESS?MulticastReadiness::Deferred:MulticastReadiness::Error;
}
} // namespace x64arm::socket_detail
