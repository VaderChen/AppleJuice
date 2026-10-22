#include "x64arm/windows_services.hpp"
#include "x64arm/resource_usage.hpp"
#include "socket_address.hpp"
#include "socket_message.hpp"
#include "socket_ingress.hpp"
#include "socket_filter_control.hpp"
#include <poll.h>
#include <chrono>
#include <netdb.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <netinet/in.h>
#include <fcntl.h>
#include <filesystem>
#include <atomic>
#include <cstring>

namespace x64arm {
namespace {
using namespace socket_detail;
void trace_loopback_io(std::uint32_t pid,const NativeSocket& socket,const PendingSocketIo& io,const char* phase) {
    if(!std::getenv("X64ARM_TRACE_LOOPBACK_IO"))return;
    const auto saved=errno;sockaddr_storage peer{};socklen_t length=sizeof(peer);
    if(!::getpeername(socket.descriptor,reinterpret_cast<sockaddr*>(&peer),&length)){
        const bool loopback=peer.ss_family==AF_INET?(ntohl(reinterpret_cast<sockaddr_in*>(&peer)->sin_addr.s_addr)>>24)==127:
            peer.ss_family==AF_INET6 && IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<sockaddr_in6*>(&peer)->sin6_addr);
        if(loopback){
            const auto port=ntohs(peer.ss_family==AF_INET?reinterpret_cast<sockaddr_in*>(&peer)->sin_port:reinterpret_cast<sockaddr_in6*>(&peer)->sin6_port);
            std::fprintf(stderr,"[loopback_io] pid=%u socket=%llx port=%u phase=%s kind=%u requested=%zu transferred=%u error=%u overlapped=%llx prefix=",pid,static_cast<unsigned long long>(io.socket),port,phase,static_cast<unsigned>(io.kind),io.data.size(),io.transferred,io.error,static_cast<unsigned long long>(io.overlapped));
            for(std::size_t i=0;i<std::min<std::size_t>(24,io.kind==PendingSocketIo::Kind::Receive?io.transferred:io.data.size());++i)std::fprintf(stderr,"%02x",io.data[i]);
            std::fputc('\n',stderr);
            const auto count=io.kind==PendingSocketIo::Kind::Receive?io.transferred:io.data.size();
            if(const auto* directory=std::getenv("X64ARM_TRACE_LOOPBACK_PAYLOAD_DIR");directory && phase[0]=='c' && count>=5 &&
                (!std::memcmp(io.data.data(),"GET ",4) || !std::memcmp(io.data.data(),"HTTP/",5))){
                static std::atomic<unsigned> sequence{};
                try{
                    const auto path=std::filesystem::path(directory)/(std::to_string(pid)+"-"+std::to_string(io.socket)+"-"+std::to_string(sequence.fetch_add(1))+".bin");
                    const int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
                    if(fd>=0){std::size_t written=0;const auto size=std::min<std::size_t>(count,8192);
                        while(written<size){const auto part=::write(fd,io.data.data()+written,size-written);if(part<0 && errno==EINTR)continue;if(part<=0)break;written+=static_cast<std::size_t>(part);}
                        ::close(fd);
                    }
                }catch(const std::exception&){} // 選用診斷失敗不改變客體 I/O 的結果。
            }
        }
    }
    errno=saved;
}
int send_flags(std::uint32_t flags) {
    int result=MSG_DONTWAIT|(flags&1?MSG_OOB:0)|(flags&4?MSG_DONTROUTE:0);
#ifdef MSG_NOSIGNAL
    result|=MSG_NOSIGNAL;
#endif
    return result;
}
std::uint32_t disconnect_socket(NativeSocket& socket,bool reuse) {
    if(!reuse){if(::shutdown(socket.descriptor,SHUT_RDWR)<0)return socket_error(errno);socket.accepted_at=UINT64_MAX;socket.send_shutdown=true;return 0;}
    // 客體 handle 不變；原生 TCP descriptor 重新建立，保留選項及原先 bind 的位址。
    auto local=socket.bound_address();
    if(local.empty()){sockaddr_storage address{};socklen_t size=sizeof(address);if(::getsockname(socket.descriptor,reinterpret_cast<sockaddr*>(&address),&size)<0)return socket_error(errno);local=address_bytes(reinterpret_cast<sockaddr*>(&address));}
    const int next=::socket(socket.family,socket.type,socket.protocol);if(next<0)return socket_error(errno);
    const auto close_next=[&](std::uint32_t error){::close(next);return error;};
    if(::fcntl(next,F_SETFD,FD_CLOEXEC)<0 || copy_accept_options(socket.descriptor,next)<0)return close_next(socket_error(errno));
#ifdef SO_NOSIGPIPE
    const int no_signal=1;if(::setsockopt(next,SOL_SOCKET,SO_NOSIGPIPE,&no_signal,sizeof(no_signal))<0)return close_next(socket_error(errno));
#endif
    int original_reuse=0;socklen_t option_size=sizeof(original_reuse);const int enabled=1;
    if(::getsockopt(socket.descriptor,SOL_SOCKET,SO_REUSEADDR,&original_reuse,&option_size)<0 ||
       ::setsockopt(socket.descriptor,SOL_SOCKET,SO_REUSEADDR,&enabled,sizeof(enabled))<0 ||
       ::setsockopt(next,SOL_SOCKET,SO_REUSEADDR,&enabled,sizeof(enabled))<0)return close_next(socket_error(errno));
    (void)::shutdown(socket.descriptor,SHUT_RDWR);::close(socket.descriptor);socket.descriptor=next;
    socket.accepted_at=UINT64_MAX;socket.accept_listener=0;socket.accept_context=socket.connect_context=socket.listening=false;socket.reusable=true;socket.send_shutdown=false;
    const auto address=native_address(local);if(!address.size)return 10022;
    if(::bind(next,reinterpret_cast<const sockaddr*>(&address.storage),address.size)<0)return socket_error(errno);
    if(::setsockopt(next,SOL_SOCKET,SO_REUSEADDR,&original_reuse,sizeof(original_reuse))<0)return socket_error(errno);
    socket.set_bound_address(local);return 0;
}
}
void WindowsServices::poll_socket_io(GuestMemory& memory) {
    auto& network=runtime_.network;const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
    // 等待中的一般收送先合併查詢 readiness；每輪仍查核心，不加輪詢期限。
    // 同一 fd 的不同方向合併，避開 Darwin 對重複 poll 項目的合併行為。
    std::array<pollfd,128> entries{};std::array<std::size_t,128> indices{};
    std::size_t first=0,last=0;bool queried=false;
    // 小量一般收送的 recvmsg／sendmsg 較便宜；零長度 receive 原先每筆都需 poll。
    const bool large_batch=network.pending_socket_io.size()>=64;
    const bool batching=large_batch || (network.pending_socket_io.size()>1 &&
        std::any_of(network.pending_socket_io.begin(),network.pending_socket_io.end(),[](const auto& pending){
            return !pending.done && !pending.error && pending.kind==PendingSocketIo::Kind::Receive && pending.data.empty();
        }));
    for(std::size_t position=0;position<network.pending_socket_io.size();++position){
        if(batching && position==last){
            first=position;last=std::min(first+entries.size(),network.pending_socket_io.size());
            std::size_t count=0;indices.fill(SIZE_MAX);queried=false;
            if(network.pending_socket_io.size()>1)for(auto index=first;index<last;++index){
                const auto& pending=network.pending_socket_io[index];
                if(!large_batch && (pending.kind!=PendingSocketIo::Kind::Receive || !pending.data.empty()))continue;
                // 初始化連線、關閉、零長度 send 及 OOB 仍依原契約直接嘗試。
                if(pending.done || pending.error || pending.kind==PendingSocketIo::Kind::Control || pending.kind==PendingSocketIo::Kind::Disconnect ||
                   (pending.kind==PendingSocketIo::Kind::Connect && !pending.started) ||
                   (pending.flags&1) || (pending.kind==PendingSocketIo::Kind::Send && pending.data.empty()))continue;
                const auto socket=network.sockets.find(pending.socket);if(socket==network.sockets.end())continue;
                // 已排隊資料可能不再位於核心；資料報接收由共用入口查詢。
                if(pending.kind==PendingSocketIo::Kind::Receive && socket->second->type==SOCK_DGRAM)continue;
                const auto fd=socket->second->descriptor;
                const auto events=static_cast<short>(pending.kind==PendingSocketIo::Kind::Receive?POLLIN:POLLOUT);
                auto item=std::find_if(entries.begin(),entries.begin()+static_cast<std::ptrdiff_t>(count),[&](const auto& value){return value.fd==fd;});
                const auto offset=static_cast<std::size_t>(item-entries.begin());
                if(offset==count)entries[count++]={fd,events,0};else item->events|=events;
                indices[index-first]=offset;
            }
            if(count)queried=::poll(entries.data(),static_cast<nfds_t>(count),0)>=0;
        }
        auto& io=network.pending_socket_io[position];
        if(io.done)continue;
        struct Progress {
            PendingSocketIo& io;
            ~Progress(){
                const auto kind=io.kind==PendingSocketIo::Kind::Receive?process_io::Kind::Read:io.kind==PendingSocketIo::Kind::Send?process_io::Kind::Write:process_io::Kind::Other;
                process_io::progress(kind,io.transferred,(io.attempted || io.started) && (io.done || io.transferred>0),io.io_recorded,io.io_accounted);
            }
        } progress{io};
        const auto found=network.sockets.find(io.socket);if(found==network.sockets.end())io.error=995;
        if(!io.error){
            auto& socket=*found->second;const int fd=socket.descriptor;
            if(queried){
                const auto index=indices[position-first];
                if(index!=SIZE_MAX && entries[index].fd==fd){
                    const auto events=io.kind==PendingSocketIo::Kind::Receive?POLLIN:POLLOUT;
                    if(!(entries[index].revents&(events|POLLERR|POLLHUP|POLLNVAL)))continue;
                }
            }
            if(io.kind==PendingSocketIo::Kind::Control){
                if(filter_control_predecessor(std::span(network.pending_socket_io).first(position),network.sockets,socket))continue;
            }else if(socket.type==SOCK_STREAM || io.kind==PendingSocketIo::Kind::Receive){
                // TCP 的同方向資料依提交順序送入傳輸層；部分傳送不能讓下一筆插入中間。
                // DisconnectEx 是傳送方向的關閉，必須先交付先前提交的傳送，再取消接收。
                const auto current=network.pending_socket_io.begin()+(&io-network.pending_socket_io.data());
                const bool receiving=io.kind==PendingSocketIo::Kind::Receive;
                if(std::any_of(network.pending_socket_io.begin(),current,[&](const auto& prior){
                    if(prior.done || prior.kind==PendingSocketIo::Kind::Control || receiving!=(prior.kind==PendingSocketIo::Kind::Receive))return false;
                    const auto other=network.sockets.find(prior.socket);
                    return other!=network.sockets.end() && other->second->shared->identity()==socket.shared->identity();
                }))continue;
            }
            if(io.kind==PendingSocketIo::Kind::Control){
                if(!advance_filter_control(socket,io))continue;
            }else if(io.kind==PendingSocketIo::Kind::Connect){
                if(!io.started){
                    const auto address=native_address(io.address);const auto flags=::fcntl(fd,F_GETFL);
                    if(flags<0 || ::fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)io.error=socket_error(errno);
                    else {const auto status=::connect(fd,reinterpret_cast<const sockaddr*>(&address.storage),address.size),error=errno;io.started=true;
                        if(std::getenv("X64ARM_TRACE_NETWORK_STATE")){
                            char host[NI_MAXHOST]{},port[NI_MAXSERV]{};
                            if(!::getnameinfo(reinterpret_cast<const sockaddr*>(&address.storage),address.size,host,sizeof(host),port,sizeof(port),NI_NUMERICHOST|NI_NUMERICSERV))
                                std::fprintf(stderr,"[socket_connect_start] pid=%u ms=%lld socket=%llx host=%s port=%s status=%d native_error=%d\n",process_id(),
                                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()),
                                    static_cast<unsigned long long>(io.socket),host,port,status,status<0?error:0);
                            errno=error;
                        }
                        if(::fcntl(fd,F_SETFL,flags)<0)io.error=socket_error(errno);
                        else if(!status)io.connected=true;
                        else if(error!=EINPROGRESS && error!=EALREADY && error!=EINTR)io.error=socket_error(error);
                    }
                }
                if(!io.error && !io.connected){pollfd entry{fd,POLLOUT,0};const auto ready=::poll(&entry,1,0);if(ready<0)io.error=socket_error(errno);else if(!ready)continue;
                    else{int error=0;socklen_t size=sizeof(error);if(::getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)<0)io.error=socket_error(errno);else if(error)io.error=socket_error(error);else io.connected=true;}
                }
                if(!io.error && io.connected){socket.accepted_at=socket.accepted_at==UINT64_MAX?socket_clock_ticks():socket.accepted_at;socket.connect_context=false;socket.reusable=false;
                    if(io.transferred<io.data.size()){const auto size=::send(fd,io.data.data()+io.transferred,io.data.size()-io.transferred,send_flags(0));
                        if(size<0){if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)continue;io.error=socket_error(errno);}
                        else{io.transferred+=static_cast<std::uint32_t>(size);record_network_transfer(0,static_cast<std::uint64_t>(size));if(io.transferred<io.data.size())continue;}
                    }
                }
            }else if(io.kind==PendingSocketIo::Kind::Disconnect){
                io.attempted=true;io.error=disconnect_socket(socket,(io.flags&2)!=0);
                for(auto& pending:network.pending_socket_io)if(&pending!=&io && pending.socket==io.socket && !pending.done)pending.error=995;
                gui_.socket_watches.erase(io.socket);
            }else{
                const bool sending=io.kind==PendingSocketIo::Kind::Send;const auto address=native_address(io.address);sockaddr_storage source{};
                const auto available=io.data.size()-io.transferred;
                // 零長度 receive 在資料／EOF 到達時完成，不消耗該資料。
                if(!sending && !available && socket.type!=SOCK_DGRAM){pollfd entry{fd,static_cast<short>(io.flags&1?POLLPRI:POLLIN),0};if(::poll(&entry,1,0)<=0)continue;}
                const int flags=sending?send_flags(io.flags):MSG_DONTWAIT|(io.flags&1?MSG_OOB:0)|(io.flags&2?MSG_PEEK:0);
                io.attempted=true;ssize_t size;
                if(sending){iovec vector{io.data.data()+io.transferred,available};msghdr message{};message.msg_iov=&vector;message.msg_iovlen=1;if(address.size){message.msg_name=const_cast<sockaddr_storage*>(&address.storage);message.msg_namelen=address.size;}if(!io.control.empty()){message.msg_control=io.control.data();message.msg_controllen=static_cast<decltype(message.msg_controllen)>(io.control.size());}size=send_message(fd,socket.type,message,flags,socket.protocol);}
                else {iovec vector{io.data.data()+io.transferred,available};msghdr message{};message.msg_iov=&vector;message.msg_iovlen=1;if(io.addressed){message.msg_name=&source;message.msg_namelen=sizeof(source);}
                    if(io.message_pointer){std::size_t used{};size=socket_receive_guest_message(socket,message,flags,x86_,io.control,used,io.result_flags);io.control_used=static_cast<std::uint32_t>(used);if(size>=0){io.address=address_bytes(reinterpret_cast<const sockaddr*>(&source));if(io.result_flags&(guest_message_truncated|guest_control_truncated))io.error=10040;}}
                    else{size=socket_receive_message(socket,message,flags);io.result_flags=message.msg_flags&MSG_OOB?1:0;if(size>=0 && (message.msg_flags&MSG_TRUNC))io.error=10040;}}
                if(size<0){if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR || errno==EINPROGRESS)continue;io.error=socket_error(errno);}
                else {io.transferred+=static_cast<std::uint32_t>(size);record_network_transfer(!sending && !(io.flags&2)?static_cast<std::uint64_t>(size):0,sending?static_cast<std::uint64_t>(size):0);
                    if(io.addressed && !sending && io.address_pointer && !io.message_pointer){const auto bytes=address_bytes(reinterpret_cast<sockaddr*>(&source));memory.write(GuestAddress{io.address_pointer},bytes);memory.store(GuestAddress{io.address_size_pointer},32,bytes.size());}
                    if(!io.error && socket.type==SOCK_STREAM && (sending || (io.flags&8)) && size && io.transferred<io.data.size())continue;
                }
            }
        }
        io.done=true;
        if(std::getenv("X64ARM_TRACE_NETWORK_STATE")){
            const auto saved=errno;
            std::fprintf(stderr,"[socket_io_complete] pid=%u ms=%lld socket=%llx kind=%u requested=%zu transferred=%u error=%u overlapped=%llx port=%llx notify=%u\n",process_id(),static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()),static_cast<unsigned long long>(io.socket),static_cast<unsigned>(io.kind),io.data.size(),io.transferred,io.error,static_cast<unsigned long long>(io.overlapped),static_cast<unsigned long long>(io.port),io.notify?1U:0U);
            errno=saved;
        }
        if(found!=network.sockets.end())trace_loopback_io(process_id(),*found->second,io,"complete");
        if(io.kind==PendingSocketIo::Kind::Control)write_filter_control(memory,io);
        if(io.kind==PendingSocketIo::Kind::Receive){std::size_t cursor=0;for(const auto& buffer:io.buffers){const auto size=std::min<std::size_t>(buffer.length,io.transferred-cursor);if(size)memory.write(GuestAddress{buffer.pointer},{io.data.data()+cursor,size});cursor+=size;if(cursor==io.transferred)break;}
            if(io.message_pointer && (!io.error || io.error==10040))write_receive_message(memory,io.message_pointer,x86_,io.address_pointer,io.control_pointer,io.address,std::span(io.control).first(io.control_used),io.result_flags);
        }
        set_file_io_signaled(io.socket,true);
        if(!io.overlapped)continue;
        memory.store(GuestAddress{io.overlapped},bits,socket_io_status(io.error));memory.store(GuestAddress{io.overlapped+unit},bits,io.transferred);
        if(io.result_flags)network.completion_flags[io.overlapped]={io.socket,io.result_flags};else network.completion_flags.erase(io.overlapped);
        if(socket_io_error(socket_io_status(io.error))!=io.error)network.completion_errors[io.overlapped]={io.socket,io.error};else network.completion_errors.erase(io.overlapped);
        const bool notify=io.notify || !io.error;
        if(io.routine && notify){const auto thread=threads_.items.find(io.thread);if(thread!=threads_.items.end() && thread->second.status!=ThreadStatus::Exited && !thread->second.terminating){thread->second.apcs.push_back({io.routine,{io.error,io.transferred,io.overlapped,io.result_flags}});threads_.yield=true;}}
        if(!io.routine && notify){if(io.event){const auto event=runtime_.wait_objects.find(io.event&~UINT64_C(1));if(event!=runtime_.wait_objects.end())event->second.set_signal(true);}
            if(!(io.event&1) && (io.notify || found==network.sockets.end() || !(found->second->completion_modes&1))){const auto port=runtime_.completion_ports.find(io.port);if(port!=runtime_.completion_ports.end())port->second.packets.push_back({io.key,io.overlapped,socket_io_status(io.error),io.transferred});}
        }
    }
    std::erase_if(network.pending_socket_io,[](const auto& io){return io.done && io.notify;});
}
std::optional<std::uint32_t> WindowsServices::invoke_socket_io(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;auto& network=runtime_.network;
    const bool controlling=symbol=="WSAIoctl",messaging=symbol=="WSASendMsg",message_receive=symbol=="WSARecvMsg",connect=symbol=="ConnectEx",disconnect=symbol=="DisconnectEx",sending=symbol.starts_with("WSASend"),addressed=messaging || message_receive || symbol=="WSASendTo" || symbol=="WSARecvFrom";
    const bool extension=connect || disconnect;const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    std::uint64_t result=extension?0:UINT32_MAX;const auto fail=[&](std::uint32_t error){last_error_=error;result=extension?0:UINT32_MAX;};
    try {
        const auto found=network.sockets.find(arg(0));
        if(!network.startups)fail(10093);
        else if(found==network.sockets.end())fail(10038);
        else {
            auto& socket=*found->second;PendingSocketIo io;io.socket=arg(0);io.thread=threads_.current;io.port=socket.completion_port;io.key=socket.completion_key;
            const auto wait_synchronous=[&]{auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::FileIo;thread.wait.handles={arg(0)};schedule(state,memory);};
            const bool synchronous_control=controlling && (!socket.overlapped || (!arg(7) && !arg(8)));
            if((disconnect && !arg(1)) || synchronous_control){
                const auto kind=controlling?PendingSocketIo::Kind::Control:PendingSocketIo::Kind::Disconnect;
                auto pending=std::find_if(network.pending_socket_io.begin(),network.pending_socket_io.end(),[&](const auto& item){return item.socket==arg(0) && item.thread==threads_.current && item.kind==kind && !item.overlapped;});
                if(pending!=network.pending_socket_io.end()){
                    poll_socket_io(memory);
                    pending=std::find_if(network.pending_socket_io.begin(),network.pending_socket_io.end(),[&](const auto& item){return item.socket==arg(0) && item.thread==threads_.current && item.kind==kind && !item.overlapped;});
                    if(!pending->done){wait_synchronous();return std::nullopt;}
                    const auto error=pending->error,transferred=pending->transferred;network.pending_socket_io.erase(pending);
                    if(error)fail(error);else{result=controlling?0:1;if(controlling)memory.store(GuestAddress{arg(6)},32,transferred);}complete_call(state,memory,result);return std::nullopt;
                }
            }
            io.kind=controlling?PendingSocketIo::Kind::Control:connect?PendingSocketIo::Kind::Connect:disconnect?PendingSocketIo::Kind::Disconnect:sending?PendingSocketIo::Kind::Send:PendingSocketIo::Kind::Receive;
            const auto overlapped_index=controlling?7U:message_receive?3U:messaging?4U:connect?6U:disconnect?1U:addressed?7U:5U;
            io.overlapped=synchronous_control?0:arg(overlapped_index);io.routine=synchronous_control || extension?0:arg(controlling?8U:message_receive?4U:messaging?5U:addressed?8U:6U);
            std::uint32_t error=0;std::uint64_t count_pointer=controlling?arg(6):message_receive?arg(2):connect?(arg(3)?arg(5):0):disconnect?0:arg(3);
            if((!disconnect && !controlling && !io.overlapped) || (controlling && io.routine && !io.overlapped))error=10022;
            else if(io.overlapped && !socket.overlapped)error=10022;
            else if(network.pending_socket_io.size()>=1024)error=10055;
            else if(io.overlapped && (std::any_of(network.pending_socket_io.begin(),network.pending_socket_io.end(),[&](const auto& p){return !p.done && p.overlapped==io.overlapped;}) || std::any_of(network.pending_accepts.begin(),network.pending_accepts.end(),[&](const auto& p){return !p.done && p.overlapped==io.overlapped;})))error=10022;
            if(!error && controlling){
                if(!count_pointer)error=10014;
                else if(prepare_filter_control(socket,memory,d(1),arg(2),d(3),arg(4),d(5),io,output_limit)<0)error=socket_error(errno);
            }else if(!error && extension){
                sockaddr_storage peer{};socklen_t peer_size=sizeof(peer);const bool connected=::getpeername(socket.descriptor,reinterpret_cast<sockaddr*>(&peer),&peer_size)==0;
                if(socket.type!=SOCK_STREAM)error=10045;
                else if(disconnect){io.flags=d(2);if((io.flags&~2U) || d(3))error=10022;else if(!connected || socket.accepted_at==UINT64_MAX)error=10057;}
                else if(connected)error=10056;
                else if(socket.listening)error=10022;
                else {
                    sockaddr_storage local{};socklen_t size=sizeof(local);
                    if(::getsockname(socket.descriptor,reinterpret_cast<sockaddr*>(&local),&size)<0)error=socket_error(errno);
                    else if(!(socket.family==AF_INET?reinterpret_cast<sockaddr_in*>(&local)->sin_port:reinterpret_cast<sockaddr_in6*>(&local)->sin6_port))error=10022;
                    else if(std::any_of(network.pending_socket_io.begin(),network.pending_socket_io.end(),[&](const auto& p){return p.socket==io.socket && p.kind==PendingSocketIo::Kind::Connect && !p.done;}))error=10037;
                    else {const auto address=read_address(memory,arg(1),d(2));if(!address.size)error=10014;else if(address.storage.ss_family!=socket.family)error=10047;
                        else{io.address=address_bytes(reinterpret_cast<const sockaddr*>(&address.storage));if(socket.bound_address().empty())socket.set_bound_address(address_bytes(reinterpret_cast<sockaddr*>(&local)));}
                        if(arg(3)){if(d(4)>output_limit)error=10055;else{io.data.resize(d(4));memory.read(GuestAddress{arg(3)},io.data);}}
                    }
                }
            }else if(!error && message_receive){
                const auto received=read_receive_message(memory,arg(1),x86_,socket.family,output_limit);
                sockaddr_storage local{};socklen_t local_size=sizeof local;
                if(socket.type!=SOCK_DGRAM && socket.type!=SOCK_RAW)error=10045;
                else if(received.flags)error=10022;
                else if(::getsockname(socket.descriptor,reinterpret_cast<sockaddr*>(&local),&local_size)<0)error=socket_error(errno);
                else if(socket.type==SOCK_DGRAM && !(socket.family==AF_INET?reinterpret_cast<sockaddr_in*>(&local)->sin_port:reinterpret_cast<sockaddr_in6*>(&local)->sin6_port))error=10022;
                else{
                    socket.shared->data().watch_rearm.fetch_or(1);io.message_pointer=received.pointer;io.address_pointer=received.name;io.control_pointer=received.control;
                    io.control_capacity=received.control_size;io.control.resize(std::min<std::uint32_t>(received.control_size,65536));io.buffers=received.buffers;io.data.resize(static_cast<std::size_t>(received.total));io.addressed=true;
                }
            }else if(!error){
                const auto sent=messaging?read_send_message(memory,arg(1),x86_):SendMessage{};const auto count=messaging?sent.count:d(2);const auto buffer_pointer=messaging?sent.buffers:arg(1);io.flags=messaging?d(2):sending?d(4):static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)},32));
                if(messaging && socket.type!=SOCK_DGRAM && socket.type!=SOCK_RAW)error=10045;
                else if(!count || count>1024 || !buffer_pointer || (io.flags&~(sending?5U:11U)) || (!sending && (io.flags&10)==10))error=10022;
                else {if(!sending){memory.check(GuestAddress{arg(4)},4,Access::Write);socket.shared->data().watch_rearm.fetch_or((io.flags&1)?4U:1U);}
                    std::uint64_t total=0;for(unsigned i=0;i<count;++i){const auto at=buffer_pointer+i*(x86_?8U:16U);SocketBuffer buffer{memory.load(GuestAddress{at+unit},bits),static_cast<std::uint32_t>(memory.load(GuestAddress{at},32))};if(buffer.length>output_limit-total){error=10055;break;}memory.check(GuestAddress{buffer.pointer},buffer.length,sending?Access::Read:Access::Write);total+=buffer.length;io.buffers.push_back(buffer);}
                    if(!error){io.data.resize(total);if(sending){std::size_t offset=0;for(const auto& buffer:io.buffers){memory.read(GuestAddress{buffer.pointer},{io.data.data()+offset,buffer.length});offset+=buffer.length;}}}
                    io.control=sent.control;io.addressed=addressed && (socket.type==SOCK_DGRAM || messaging);const auto name_pointer=messaging?sent.name:addressed?arg(5):0;
                    if(io.addressed && name_pointer){if(sending){const auto address=read_address(memory,name_pointer,messaging?sent.name_size:d(6));if(!address.size)error=10047;else io.address=address_bytes(reinterpret_cast<const sockaddr*>(&address.storage));}
                        else {const unsigned required=socket.family==AF_INET6?28:16;if(!arg(6) || memory.load(GuestAddress{arg(6)},32)<required)error=10014;else{memory.check(GuestAddress{arg(5)},required,Access::Write);memory.check(GuestAddress{arg(6)},4,Access::Write);io.address_pointer=arg(5);io.address_size_pointer=arg(6);}}
                    }
                }
            }
            if(!error){if(count_pointer)memory.check(GuestAddress{count_pointer},4,Access::Write);
                if(io.overlapped){memory.check(GuestAddress{io.overlapped},x86_?20:32,Access::Write);io.event=memory.load(GuestAddress{io.overlapped+(x86_?16:24)},bits);
                    const auto event=runtime_.wait_objects.find(io.event&~UINT64_C(1));if(!io.routine && io.event && (event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event || !(event->second.access&2)))error=6;
                    if(io.routine)memory.check(GuestAddress{io.routine},1,Access::Execute);
                }
            }
            if(error)fail(error);
            else {
                if(io.overlapped){if(!io.routine && io.event)runtime_.wait_objects.at(io.event&~UINT64_C(1)).set_signal(false);network.completion_flags.erase(io.overlapped);network.completion_errors.erase(io.overlapped);memory.store(GuestAddress{io.overlapped},bits,0x103);memory.store(GuestAddress{io.overlapped+unit},bits,0);}
                trace_loopback_io(process_id(),socket,io,"submit");
                set_file_io_signaled(io.socket,false);network.pending_socket_io.push_back(std::move(io));poll_socket_io(memory);
                const auto item=std::find_if(network.pending_socket_io.begin(),network.pending_socket_io.end(),[&](const auto& p){return p.socket==arg(0) && p.overlapped==arg(overlapped_index);});
                // lpFlags 只在同步完成時回寫；WSA_IO_PENDING 返回後，呼叫者可立即重用該堆疊。
                // 延遲完成的旗標已保存於 completion_flags／APC，由 WSAGetOverlappedResult 查詢。
                if(item->done){if(item->kind==PendingSocketIo::Kind::Receive && !message_receive)memory.store(GuestAddress{arg(4)},32,item->result_flags);if(message_receive && count_pointer && (!item->error || item->error==10040))memory.store(GuestAddress{count_pointer},32,item->transferred);if(item->error)fail(item->error);else{if(count_pointer)memory.store(GuestAddress{count_pointer},32,item->transferred);result=extension?1:0;}network.pending_socket_io.erase(item);}
                else if(!item->overlapped){wait_synchronous();return std::nullopt;}
                else{item->notify=true;fail(997);}
            }
        }
    }catch(const MemoryFault&){fail(10014);}catch(const MessageError& error){fail(error.code);}catch(const std::bad_alloc&){fail(10055);}
    complete_call(state,memory,result);return std::nullopt;
}
}
