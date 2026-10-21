#pragma once
#include "socket_datagram_queue.hpp"
#include "socket_receive_options.hpp"

namespace x64arm::socket_detail {
enum class DatagramPump {Drained,Deferred,Full,Error};

// 同一鎖涵蓋原生收包、來源判斷、排隊與消耗。已提交封包不重新套用之後
// 的來源政策；readiness、payload 總量與 recvmsg 共用同一份佇列。
class DatagramBroker {
    int fd_;
    int state_fd_;
    DatagramQueue queue_;
public:
    explicit DatagramBroker(int fd,int state_fd,std::uint64_t base=4096,std::uint64_t capacity=0):fd_(fd),state_fd_(state_fd),queue_(state_fd,base,capacity){}
    explicit operator bool()const{return bool(queue_);}
    DatagramSnapshot snapshot()const{return queue_.snapshot();}
    DatagramPump pump(unsigned budget=128){
        if(!queue_)return DatagramPump::Error;
        std::uint32_t receive_options{};if(!receive_options_read(state_fd_,receive_options))return DatagramPump::Error;
        for(unsigned i=0;i<budget;++i){
            std::array<std::uint8_t,65536> bytes{};alignas(cmsghdr) std::array<std::uint8_t,65536> controls{};sockaddr_storage source{};
            iovec vector{bytes.data(),bytes.size()};msghdr message{};message.msg_iov=&vector;message.msg_iovlen=1;message.msg_name=&source;message.msg_namelen=sizeof source;message.msg_control=controls.data();message.msg_controllen=controls.size();
            const auto size=::recvmsg(fd_,&message,MSG_DONTWAIT|MSG_PEEK);
            if(size<0){if(errno==EAGAIN || errno==EWOULDBLOCK)return DatagramPump::Drained;if(errno==EINTR){errno=EINPROGRESS;return DatagramPump::Deferred;}return DatagramPump::Error;}
            if(message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)){errno=EMSGSIZE;return DatagramPump::Error;}
            const auto packet=multicast_packet(message);if(!packet.complete){errno=EPROTO;return DatagramPump::Error;}
            bool allowed=true;
            if(packet.multicast){MulticastFilter filter;if(multicast_read_filter(fd_,packet.interface,packet.group,filter)<0)return DatagramPump::Error;allowed=multicast_filter_permits(filter,source);}
            DatagramRecord record;
            if(allowed){
                record.name.assign(reinterpret_cast<std::uint8_t*>(&source),reinterpret_cast<std::uint8_t*>(&source)+message.msg_namelen);
                record.control.assign(controls.begin(),controls.begin()+message.msg_controllen);record.data.assign(bytes.begin(),bytes.begin()+size);record.flags=static_cast<std::uint32_t>(message.msg_flags);
                receive_policy_append(record.control,receive_options);
                if(!queue_.fits(record))return errno==ENOBUFS?DatagramPump::Full:DatagramPump::Error;
            }
            // PEEK 後仍在同一鎖內；不允許其他入口直接讀取這個 descriptor。
            message.msg_namelen=sizeof source;message.msg_controllen=controls.size();
            if(::recvmsg(fd_,&message,MSG_DONTWAIT)<0)return DatagramPump::Error;
            if(allowed && !queue_.push(record))return DatagramPump::Error;
        }errno=EINPROGRESS;return DatagramPump::Deferred;
    }
    MulticastReadiness readable(){
        if(!queue_)return errno==EINPROGRESS?MulticastReadiness::Deferred:MulticastReadiness::Error;
        if(queue_.snapshot().packets)return MulticastReadiness::Ready;
        const auto status=pump();if(queue_.snapshot().packets)return MulticastReadiness::Ready;
        if(status==DatagramPump::Drained)return MulticastReadiness::Empty;
        return status==DatagramPump::Deferred?MulticastReadiness::Deferred:MulticastReadiness::Error;
    }
    ssize_t receive(msghdr& output,int flags){
        if(!queue_)return -1;
        if(flags&~(MSG_PEEK|MSG_DONTWAIT)){errno=EOPNOTSUPP;return -1;}
        if(output.msg_iovlen<0 || (output.msg_iovlen && !output.msg_iov) || (output.msg_namelen && !output.msg_name) || (output.msg_controllen && !output.msg_control)){errno=EFAULT;return -1;}
        std::size_t capacity=0;for(int i=0;i<output.msg_iovlen;++i){const auto& vector=output.msg_iov[i];if((vector.iov_len && !vector.iov_base) || vector.iov_len>std::size_t(SSIZE_MAX)-capacity){errno=EINVAL;return -1;}capacity+=vector.iov_len;}
        if(!queue_.snapshot().packets){const auto status=pump();if(!queue_.snapshot().packets){if(status==DatagramPump::Drained)errno=EAGAIN;return -1;}}
        DatagramRecord record;if(!queue_.peek(record))return -1;
        const auto copied=std::min(capacity,record.data.size());std::size_t offset=0;
        for(int i=0;i<output.msg_iovlen && offset<copied;++i){const auto count=std::min(output.msg_iov[i].iov_len,copied-offset);std::memcpy(output.msg_iov[i].iov_base,record.data.data()+offset,count);offset+=count;}
        if(output.msg_name)std::memcpy(output.msg_name,record.name.data(),std::min<std::size_t>(output.msg_namelen,record.name.size()));output.msg_namelen=static_cast<socklen_t>(record.name.size());
        const auto control=std::min<std::size_t>(output.msg_controllen,record.control.size());if(control)std::memcpy(output.msg_control,record.control.data(),control);
        output.msg_flags=static_cast<int>(record.flags)|(copied<record.data.size()?MSG_TRUNC:0)|(control<record.control.size()?MSG_CTRUNC:0);output.msg_controllen=static_cast<socklen_t>(control);
        if(!(flags&MSG_PEEK) && !queue_.pop(record))return -1;return static_cast<ssize_t>(copied);
    }
};
} // namespace x64arm::socket_detail
