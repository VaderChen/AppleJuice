#pragma once
#include "socket_ingress.hpp"
#include "socket_address.hpp"
#include "x64arm/memory.hpp"

namespace x64arm::socket_detail {
inline bool filter_control_predecessor(std::span<const PendingSocketIo> earlier,
                                      const std::map<std::uint64_t,std::shared_ptr<NativeSocket>>& sockets,
                                      const NativeSocket& socket){
    return std::any_of(earlier.begin(),earlier.end(),[&](const auto& prior){
        if(prior.done || prior.kind!=PendingSocketIo::Kind::Control)return false;
        const auto other=sockets.find(prior.socket);
        return other!=sockets.end() && other->second->shared->identity()==socket.shared->identity();
    });
}
// 捕捉客體輸入及輸出位置；續作不再依賴可被呼叫者重用的輸入 buffer。
inline int prepare_filter_control(const NativeSocket& socket,GuestMemory& memory,std::uint32_t code,
                                  std::uint64_t input,std::uint32_t input_size,
                                  std::uint64_t output,std::uint32_t output_size,
                                  PendingSocketIo& io,std::uint32_t limit){
    if(!multicast_filter_ioctl(code)){errno=EOPNOTSUPP;return -1;}
    if(socket.type!=SOCK_DGRAM && socket.type!=SOCK_RAW){errno=EOPNOTSUPP;return -1;}
    const bool get=multicast_filter_ioctl_get(code),generic=multicast_filter_ioctl_generic(code);
    const unsigned header=generic?144U:16U,stride=generic?128U:4U;
    if(!input || input_size<header || input_size>limit){errno=EFAULT;return -1;}
    std::vector<std::uint8_t> captured(input_size);memory.read(GuestAddress{input},captured);
    MulticastFilterMessage request;if(decode_multicast_filter(captured,generic,!get,request)<0)return -1;
    if(socket.family!=request.group.ss_family && !(socket.family==AF_INET6 && request.group.ss_family==AF_INET)){errno=EAFNOSUPPORT;return -1;}
    std::vector<std::uint8_t> result;
    if(get){const auto required=header+std::size_t(request.capacity)*stride;
        if(!output || output_size<required){errno=EFAULT;return -1;}
        memory.check(GuestAddress{output},required,Access::Write);result.resize(required);
    }
    io.kind=PendingSocketIo::Kind::Control;io.flags=code;io.data=std::move(captured);
    io.control=std::move(result);io.control_pointer=get?output:0;io.control_capacity=static_cast<std::uint32_t>(io.control.size());
    return 0;
}

// true 表示本請求已完成（含取消／錯誤），false 表示保留相同快照協同續作。
inline bool advance_filter_control(const NativeSocket& socket,PendingSocketIo& io)try{
    if(io.done)return true;
    if(io.error){io.done=true;return true;}
    if(io.kind!=PendingSocketIo::Kind::Control || !multicast_filter_ioctl(io.flags)){io.error=10045;io.done=true;return true;}
    io.attempted=true;
    const bool get=multicast_filter_ioctl_get(io.flags),generic=multicast_filter_ioctl_generic(io.flags);
    MulticastFilterMessage request;int result=decode_multicast_filter(io.data,generic,!get,request);
    if(result==0 && get){SocketStateAccess access(socket.shared->descriptor());
        if(!access)result=-1;
        else{MulticastFilter actual;result=multicast_read_filter(socket.descriptor,request.interface,request.group,actual);
            if(result==0){std::size_t used{};result=encode_multicast_filter(io.data,generic,request,actual,io.control,used);
                if(result==0){io.control_used=static_cast<std::uint32_t>(used);io.transferred=io.control_used;}}
        }
    }else if(result==0)result=socket_multicast_filter_change(socket,request);
    if(result<0){if(errno==EINPROGRESS || errno==EINTR)return false;io.error=socket_error(errno);}
    io.done=true;return true;
}catch(const std::bad_alloc&){io.error=10055;io.done=true;return true;}

inline void write_filter_control(GuestMemory& memory,PendingSocketIo& io){
    if(!io.done || io.error || !io.control_used)return;
    try{memory.write(GuestAddress{io.control_pointer},std::span(io.control).first(io.control_used));}
    catch(const MemoryFault&){io.error=10014;io.transferred=0;io.control_used=0;}
}
} // namespace x64arm::socket_detail
