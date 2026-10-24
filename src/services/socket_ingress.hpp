#pragma once
#include <array>
#include <sys/stat.h>
#include <string_view>
#include <cstdlib>
#include "x64arm/network.hpp"
#include "socket_datagram_broker.hpp"
#include "socket_multicast_options.hpp"
#include "socket_receive_control.hpp"
#include "socket_multicast_filter_message.hpp"
#include <climits>
#include <poll.h>
#include <sys/ioctl.h>

namespace x64arm::socket_detail {
inline int socket_multicast_filter_change(const NativeSocket& socket,const MulticastFilterMessage& message)try{
    SocketStateAccess access(socket.shared->descriptor());if(!access)return -1;
    if(socket.type==SOCK_DGRAM){DatagramBroker broker(socket.descriptor,socket.shared->descriptor());if(!broker)return -1;
        const auto status=broker.pump();if(status!=DatagramPump::Drained){if(status==DatagramPump::Deferred)errno=EINPROGRESS;else if(status==DatagramPump::Full)errno=ENOBUFS;return -1;}}
    return multicast_write_filter(socket.descriptor,message.interface,message.group,message.filter);
}catch(const std::bad_alloc&){errno=ENOMEM;return -1;}
struct ReceiveAncillaryOption{std::uint32_t bit{};int level{},option{};};
inline ReceiveAncillaryOption socket_receive_option(const NativeSocket& socket,std::uint32_t level,std::uint32_t option){
    if(socket.type!=SOCK_DGRAM && socket.type!=SOCK_RAW)return {};
    if(level==0 && (socket.family==AF_INET || socket.family==AF_INET6)){
        if(option==19)return {1,0,0};
        // Darwin 的 IPv6 HOPLIMIT 對 v4 mapped 訊息不產生 TTL control。
        // 不以它代替 IPv4 RECVTTL，保留核心拒絕而不宣告無效的成功。
        if(option==21)return {4,IPPROTO_IP,IP_RECVTTL};
        if(option==40 && socket.type==SOCK_DGRAM)return socket.family==AF_INET6?ReceiveAncillaryOption{8,IPPROTO_IPV6,IPV6_RECVTCLASS}:ReceiveAncillaryOption{8,IPPROTO_IP,IP_RECVTOS};
        // Windows IP_RECVECN／IPV6_RECVECN 與 ECN control 同為 50。
        // metadata 可共用 TOS／class，客體開關與已排隊政策各自獨立。
        if(option==50)return socket.family==AF_INET6?ReceiveAncillaryOption{receive_ecn4,IPPROTO_IPV6,IPV6_RECVTCLASS}:ReceiveAncillaryOption{receive_ecn4,IPPROTO_IP,IP_RECVTOS};
    }
    if(level==41 && socket.family==AF_INET6){
        if(option==19)return {2,0,0};
        if(option==21)return {16,IPPROTO_IPV6,37}; // SDK RFC3542 IPV6_RECVHOPLIMIT
        if(option==40 && socket.type==SOCK_DGRAM)return {32,IPPROTO_IPV6,IPV6_RECVTCLASS};
        if(option==50)return {receive_ecn6,IPPROTO_IPV6,IPV6_RECVTCLASS};
    }
    return {};
}
inline bool socket_packet_info_option(const NativeSocket& socket,std::uint32_t level,std::uint32_t option){return option==19 && socket_receive_option(socket,level,option).bit!=0;}
inline int socket_receive_option_get(const NativeSocket& socket,ReceiveAncillaryOption option,std::uint32_t& value){
    if(!option.bit){errno=ENOPROTOOPT;return -1;}
    SocketStateAccess access(socket.shared->descriptor());if(!access)return -1;std::uint32_t flags{};
    if(!receive_options_read(socket.shared->descriptor(),flags))return -1;value=(flags&option.bit)?1U:0U;return 0;
}
inline int socket_receive_option_set(const NativeSocket& socket,ReceiveAncillaryOption option,std::uint32_t value)try{
    if(!option.bit){errno=ENOPROTOOPT;return -1;}
    SocketStateAccess access(socket.shared->descriptor());if(!access)return -1;
    if(socket.type==SOCK_DGRAM){DatagramBroker broker(socket.descriptor,socket.shared->descriptor());if(!broker)return -1;
        const auto status=broker.pump();if(status!=DatagramPump::Drained){if(status==DatagramPump::Deferred)errno=EINPROGRESS;else if(status==DatagramPump::Full)errno=ENOBUFS;return -1;}}
    std::uint32_t flags{};if(!receive_options_read(socket.shared->descriptor(),flags))return -1;
    // 原生 metadata 一旦開啟即保留供內部收包；客體值由每筆政策控制匯出。
    // 先確認真實核心選項成功，才提交客體設定。
    if(value && option.option){const int enabled=1;if(::setsockopt(socket.descriptor,option.level,option.option,&enabled,sizeof enabled)<0)return -1;}
    const auto bit=option.bit;flags=value?(flags|bit):(flags&~bit);
    return receive_options_write(socket.shared->descriptor(),flags)?0:-1;
}catch(const std::bad_alloc&){errno=ENOMEM;return -1;}
inline int socket_packet_info_get(const NativeSocket& socket,std::uint32_t level,std::uint32_t& value){return socket_receive_option_get(socket,socket_receive_option(socket,level,19),value);}
inline int socket_packet_info_set(const NativeSocket& socket,std::uint32_t level,std::uint32_t value){return socket_receive_option_set(socket,socket_receive_option(socket,level,19),value);}
inline ssize_t socket_receive_guest_message(const NativeSocket& socket,msghdr& message,int flags,bool x86,
                                          std::span<std::uint8_t> control,std::size_t& used,std::uint32_t& result_flags)try{
    SocketStateAccess access(socket.shared->descriptor());if(!access)return -1;
    std::uint32_t policy{};if(!receive_options_read(socket.shared->descriptor(),policy))return -1;
    alignas(cmsghdr) std::array<std::uint8_t,65536> raw{};message.msg_control=raw.data();message.msg_controllen=raw.size();
    struct ClearControl{msghdr& message;~ClearControl(){message.msg_control=nullptr;message.msg_controllen=0;}} clear{message};
    ssize_t count;
    if(socket.type==SOCK_DGRAM){policy=0;DatagramBroker broker(socket.descriptor,socket.shared->descriptor());count=broker.receive(message,flags);}
    else count=::recvmsg(socket.descriptor,&message,flags);
    if(count<0)return -1;
    if(!receive_policy_read(message,policy) || !encode_receive_control(message,x86,policy,control,used,result_flags))return -1;
    // raw 是內部暫存；不可讓呼叫者保留失效的宿主 pointer。
    message.msg_control=nullptr;message.msg_controllen=0;return count;
}catch(const std::bad_alloc&){errno=ENOMEM;return -1;}
// 已提交資料與原生資料使用同一個入口；TCP 的原生傳輸及 OOB 保留。
inline ssize_t socket_receive_message(const NativeSocket& socket,msghdr& message,int flags)try{
    if(socket.type!=SOCK_DGRAM)return ::recvmsg(socket.descriptor,&message,flags);
    DatagramBroker broker(socket.descriptor,socket.shared->descriptor());
    return broker.receive(message,flags);
}catch(const std::bad_alloc&){errno=ENOMEM;return -1;}
inline ssize_t socket_receive_buffer(const NativeSocket& socket,void* bytes,std::size_t size,int flags,
                                     sockaddr_storage* source,socklen_t* source_size,int& result_flags){
    iovec vector{bytes,size};msghdr message{};message.msg_iov=&vector;message.msg_iovlen=1;
    if(source){message.msg_name=source;message.msg_namelen=*source_size;}
    const auto count=socket_receive_message(socket,message,flags);result_flags=message.msg_flags;
    if(count>=0 && source)*source_size=message.msg_namelen;return count;
}
// Deferred 是內部有界工作／鎖競爭，呼叫者必須協同重試，不能當成完整空快照。
inline bool socket_readiness(const NativeSocket& socket,pollfd& entry)try{
    if(socket.type!=SOCK_DGRAM || !(entry.events&POLLIN))return false;
    // 原生 socket 沒有資料，且共享佇列狀態檔自上次完整檢查後沒有變動（其他程序放入資料報必經 pwrite 而改變 mtime）時，
    // 直接沿用上次的空／可讀結果，不取跨程序紀錄鎖、不讀佇列標頭。標記在完整檢查之前取得：檢查期間的寫入只會讓下一次重查。
    // Steam.exe 每次排程都會查所有 WSAEventSelect 監看的 UDP socket，原本每個都要 fcntl 鎖與數次 pread。X64ARM_DATAGRAM_IDLE_CACHE=0 關閉。
    static const bool idle_cache=[] {const auto* value=std::getenv("X64ARM_DATAGRAM_IDLE_CACHE");return !value || std::string_view(value)!="0";}();
    const bool native=(entry.revents&(POLLIN|POLLERR|POLLHUP))!=0;
    struct stat before{};std::array<std::int64_t,6> stamp{};
    const bool stamped=idle_cache && !native && ::fstat(socket.shared->descriptor(),&before)==0;
    if(stamped){
#if defined(__APPLE__)
        stamp={static_cast<std::int64_t>(before.st_ino),static_cast<std::int64_t>(before.st_size),before.st_mtimespec.tv_sec,before.st_mtimespec.tv_nsec,before.st_ctimespec.tv_sec,before.st_ctimespec.tv_nsec};
#else
        stamp={static_cast<std::int64_t>(before.st_ino),static_cast<std::int64_t>(before.st_size),before.st_mtim.tv_sec,before.st_mtim.tv_nsec,before.st_ctim.tv_sec,before.st_ctim.tv_nsec};
#endif
        if(socket.datagram_idle && socket.datagram_idle_stamp==stamp){
            entry.revents=static_cast<short>(entry.revents&~POLLIN);if(socket.datagram_ready)entry.revents|=POLLIN;return false;}
    }
    socket.datagram_idle=false;
    DatagramBroker broker(socket.descriptor,socket.shared->descriptor());
    const auto ready=broker.readable();entry.revents=static_cast<short>(entry.revents&~POLLIN);
    if(ready==MulticastReadiness::Ready)entry.revents|=POLLIN;
    else if(ready==MulticastReadiness::Error)entry.revents|=POLLERR;
    // 有封包但尚未讀走（例如 WSAEventSelect 的 FD_READ 未重新啟用）同樣沿用：讀取、放入與 pump 都經 pwrite 改變戳記。
    // 每次排程都查監看中的 socket，原本佇列非空時每次都取鎖並讀標頭與布局（DQ3 強制平行約占主執行緒四成）。
    if(stamped && (ready==MulticastReadiness::Empty || ready==MulticastReadiness::Ready)){
        socket.datagram_idle=true;socket.datagram_ready=ready==MulticastReadiness::Ready;socket.datagram_idle_stamp=stamp;}
    return ready==MulticastReadiness::Deferred;
}catch(const std::bad_alloc&){entry.revents=static_cast<short>((entry.revents&~POLLIN)|POLLERR);errno=ENOMEM;return false;}
inline int socket_readable_bytes(const NativeSocket& socket,int& bytes)try{
    if(socket.type!=SOCK_DGRAM)return ::ioctl(socket.descriptor,FIONREAD,&bytes);
    DatagramBroker broker(socket.descriptor,socket.shared->descriptor());if(!broker)return -1;
    const auto status=broker.pump();
    if(status!=DatagramPump::Drained){if(status==DatagramPump::Deferred)errno=EINPROGRESS;else if(status==DatagramPump::Full)errno=ENOBUFS;return -1;}
    bytes=static_cast<int>(std::min<std::uint64_t>(INT_MAX,broker.snapshot().payload));return 0;
}catch(const std::bad_alloc&){errno=ENOMEM;return -1;}
inline int socket_multicast_change(const NativeSocket& socket,MulticastOption option,std::span<const std::uint8_t> bytes)try{
    if(socket.type!=SOCK_DGRAM)return multicast_set(socket.descriptor,option,socket.family,bytes);
    DatagramBroker broker(socket.descriptor,socket.shared->descriptor());if(!broker)return -1;
    // 保留在變更前已提交的封包；原生到達與 setsockopt 之間的競態另行驗收。
    const auto status=broker.pump();
    if(status!=DatagramPump::Drained){if(status==DatagramPump::Deferred)errno=EINPROGRESS;else if(status==DatagramPump::Full)errno=ENOBUFS;return -1;}
    return multicast_set(socket.descriptor,option,socket.family,bytes);
}catch(const std::bad_alloc&){errno=ENOMEM;return -1;}
} // namespace x64arm::socket_detail
