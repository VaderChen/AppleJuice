#include "x64arm/windows_services.hpp"
#include "x64arm/resource_usage.hpp"
#include "x64arm/unicode.hpp"
#include "socket_address.hpp"
#include "socket_address_text.hpp"
#include "socket_database.hpp"
#include "socket_message.hpp"
#include "socket_fragmentation_option.hpp"
#include "socket_randomize_option.hpp"
#include "socket_multicast_options.hpp"
#include "socket_ingress.hpp"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <poll.h>
#ifdef __APPLE__
#include <sys/event.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>

namespace x64arm {
NetworkState::NetworkState(const NetworkState& other) : external_io(other.external_io),configuration(other.configuration),startups(other.startups),next_socket(other.next_socket),addresses(other.addresses),service_buffers(other.service_buffers) {
    next_lookup=other.next_lookup;namespace_lookups=other.namespace_lookups;completion_flags=other.completion_flags;completion_errors=other.completion_errors;
    if (!other.resolvers.empty() || !other.duplications.empty() || !other.pending_lookups.empty() || !other.sockets.empty() || !other.pending_accepts.empty() || !other.pending_socket_io.empty()) throw std::logic_error("無法複製含有使用中網路連線的執行狀態");
}
NetworkState& NetworkState::operator=(const NetworkState& other) {if (this!=&other) {*this=NetworkState(other);}return *this;}
bool NetworkState::operator==(const NetworkState& other) const {
    return resolvers==other.resolvers && namespace_lookups==other.namespace_lookups && duplications==other.duplications && next_lookup==other.next_lookup && pending_lookups==other.pending_lookups && pending_accepts==other.pending_accepts && pending_socket_io==other.pending_socket_io && completion_flags==other.completion_flags && completion_errors==other.completion_errors && external_io==other.external_io && configuration==other.configuration && startups==other.startups && next_socket==other.next_socket && addresses==other.addresses && service_buffers==other.service_buffers && sockets==other.sockets;
}
namespace {
using namespace socket_detail;
int readable_socket_bytes(const NativeSocket& socket,int& bytes){
    return socket_readable_bytes(socket,bytes);
}
#ifdef __APPLE__
bool socket_urgent_ready(int fd){
    // Darwin poll 不回傳 TCP POLLPRI；使用核心的例外 filter，且不讀取客體資料。
    struct Queue {int fd;~Queue(){if(fd>=0)::close(fd);}} queue{::kqueue()};if(queue.fd<0)return false;
    (void)::fcntl(queue.fd,F_SETFD,FD_CLOEXEC);struct kevent change{},event{};
    EV_SET(&change,static_cast<std::uintptr_t>(fd),EVFILT_EXCEPT,EV_ADD|EV_ENABLE|EV_ONESHOT,NOTE_OOB,0,nullptr);
    const timespec immediate{};return ::kevent(queue.fd,&change,1,&event,1,&immediate)>0 && !(event.flags&EV_ERROR) && (event.fflags&NOTE_OOB);
}
#endif
int host_family(unsigned family) {return family==2?AF_INET:family==23?AF_INET6:family==0?AF_UNSPEC:-1;}
unsigned guest_family(int family) {return family==AF_INET?2U:family==AF_INET6?23U:0U;}
constexpr std::array<std::uint32_t,4> socket_provider_id{0x53eaf460,0x4f693da2,0x29c67d91,0x05b47ade};
struct SocketProtocol {unsigned family,type,protocol,catalog;};
constexpr std::array<SocketProtocol,4> socket_protocols{{{2,1,6,1},{2,2,17,2},{23,1,6,3},{23,2,17,4}}};
bool socket_text_provider(const GuestMemory& memory,std::uint64_t pointer,unsigned family,bool wide){
    if(!pointer)return true;
    memory.check(GuestAddress{pointer},wide?628:372,Access::Read);
    for(unsigned i=0;i<4;++i)if(memory.load(GuestAddress{pointer+20+4*i},32)!=socket_provider_id[i])return false;
    return memory.load(GuestAddress{pointer+40},32)==1 && memory.load(GuestAddress{pointer+76},32)==family;
}
std::vector<std::uint8_t> socket_protocol_info(const SocketProtocol& protocol,bool wide){
    std::vector<std::uint8_t> bytes(wide?628:372);const auto put=[&](unsigned offset,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8));};
    const bool tcp=protocol.type==1,ipv6=protocol.family==23;
    // 只宣告實際 POSIX backend；不支援檔案式 socket I/O，故不設 XP1_IFS_HANDLES。
    put(0,tcp?0x66U:ipv6?9U:0x209U);put(16,8);for(unsigned i=0;i<4;++i)put(20+i*4,socket_provider_id[i]);
    put(36,protocol.catalog);put(40,1);put(72,2);put(76,protocol.family);put(80,ipv6?28:16);put(84,ipv6?28:16);put(88,protocol.type);put(92,protocol.protocol);put(108,tcp?0U:ipv6?65527U:65507U);
    const auto name=std::string("X64ARM ")+(tcp?"TCP/":"UDP/")+(ipv6?"IPv6":"IPv4");
    for(std::size_t i=0;i<name.size();++i)bytes[116+i*(wide?2U:1U)]=static_cast<std::uint8_t>(name[i]);return bytes;
}

}
bool WindowsServices::socket_select_ready(const ThreadWait& wait) const {
    if(wait.deadline<=runtime_.clock_ticks)return true;
    std::vector<pollfd> entries;entries.reserve(wait.handles.size());
    for(std::size_t i=0;i<wait.handles.size();++i){const auto socket=runtime_.network.sockets.find(wait.handles[i]);if(socket==runtime_.network.sockets.end())return true;entries.push_back({socket->second->descriptor,static_cast<short>(wait.socket_events[i]),0});}
#ifdef __APPLE__
    for(const auto& entry:entries)if((entry.events&POLLPRI) && socket_urgent_ready(entry.fd))return true;
#endif
    const auto ready=::poll(entries.data(),static_cast<nfds_t>(entries.size()),0);
    if(ready<0)return errno!=EINTR;
    for(std::size_t i=0;i<entries.size();++i){
        if(socket_detail::socket_readiness(*runtime_.network.sockets.at(wait.handles[i]),entries[i]))return true;
        if(entries[i].revents)return true;
    }return false;
}
void WindowsServices::poll_network_io(GuestMemory& memory){
    poll_socket_lookups(memory);pump_gui_network();
    auto& network=runtime_.network;const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
    // 尚未就緒的 AcceptEx 不必切換 descriptor flags 或嘗試 accept／recv。
    // 每批仍取得新的核心快照；錯誤與取消直接完成，保留原本的完成順序與競態處理。
    constexpr std::size_t batch_size=128;
    for(std::size_t first=0;first<network.pending_accepts.size();first+=batch_size){
        const auto end=std::min(first+batch_size,network.pending_accepts.size());
        std::array<pollfd,batch_size> entries;std::array<std::size_t,batch_size> slots;
        std::array<bool,batch_size> ready{};std::size_t polled_count=0;
        for(std::size_t index=first;index<end;++index){
            auto& io=network.pending_accepts[index];if(io.done || io.error)continue;
            const auto listener=network.sockets.find(io.listener),accepted=network.sockets.find(io.accepted);
            if(listener==network.sockets.end() || accepted==network.sockets.end()){io.error=995;continue;}
            if(io.connected && !io.receive_size)continue;
            const auto fd=(io.connected?accepted:listener)->second->descriptor;
            if(fd<0){ready[index-first]=true;continue;}
            slots[polled_count]=index-first;entries[polled_count++]={fd,POLLIN,0};
        }
        if(polled_count){
            int result;
#ifdef __APPLE__
            // Darwin 的小型讀取集合使用 select；超出 fd_set 範圍仍使用 poll。
            fd_set readable;FD_ZERO(&readable);int limit=0;bool use_select=true;
            for(std::size_t index=0;index<polled_count;++index){
                const auto fd=entries[index].fd;if(fd>=FD_SETSIZE){use_select=false;break;}
                FD_SET(fd,&readable);limit=std::max(limit,fd+1);
            }
            if(use_select){
                timeval immediate{};result=::select(limit,&readable,nullptr,nullptr,&immediate);
                if(result>=0)for(std::size_t index=0;index<polled_count;++index)entries[index].revents=FD_ISSET(entries[index].fd,&readable)?POLLIN:0;
            }else
#endif
            result=::poll(entries.data(),static_cast<nfds_t>(polled_count),0);
            if(result>=0){for(std::size_t index=0;index<polled_count;++index)ready[slots[index]]=entries[index].revents!=0;}
            else if(errno!=EINTR){for(std::size_t index=0;index<polled_count;++index)ready[slots[index]]=true;}
        }
        for(std::size_t index=first;index<end;++index){
        auto& io=network.pending_accepts[index];
        if(io.done || (!io.error && (!io.connected || io.receive_size) && !ready[index-first]))continue;
        const auto listener=network.sockets.find(io.listener),accepted=network.sockets.find(io.accepted);
        if(listener==network.sockets.end() || accepted==network.sockets.end())io.error=995;
        if(!io.error && !io.connected){
            const auto fd=listener->second->descriptor;const auto flags=::fcntl(fd,F_GETFL);
            if(flags<0 || ::fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0)io.error=socket_error(errno);
            else{
                sockaddr_storage peer{};socklen_t size=sizeof(peer);const auto connected=::accept(fd,reinterpret_cast<sockaddr*>(&peer),&size);const auto error=errno;
                if(::fcntl(fd,F_SETFL,flags)<0){if(connected>=0)::close(connected);io.error=socket_error(errno);}
                else if(connected<0){if(error==EAGAIN || error==EWOULDBLOCK || error==EINTR)continue;io.error=socket_error(error);}
                else{
                    if(::fcntl(connected,F_SETFD,FD_CLOEXEC)<0 || copy_accept_options(accepted->second->descriptor,connected)<0){io.error=socket_error(errno);::close(connected);}
                    else{auto& socket=*accepted->second;::close(socket.descriptor);socket.descriptor=connected;socket.accept_listener=io.listener;socket.accepted_at=socket_clock_ticks();socket.accept_context=false;socket.reusable=false;socket.set_bound_address({});io.connected=true;
#ifdef SO_NOSIGPIPE
                        const int enabled=1;(void)::setsockopt(connected,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled));
#endif
                        sockaddr_storage local{};socklen_t local_size=sizeof(local);
                        if(::getsockname(connected,reinterpret_cast<sockaddr*>(&local),&local_size)<0)io.error=socket_error(errno);
                        else{const auto local_bytes=address_bytes(reinterpret_cast<sockaddr*>(&local)),remote_bytes=address_bytes(reinterpret_cast<sockaddr*>(&peer));
                            // 位址區是 provider 的不透明格式：16-byte 標頭記錄實際 sockaddr 長度。
                            const auto put=[&](std::uint64_t address,const auto& bytes){memory.write(GuestAddress{address},std::array<std::uint8_t,16>{});memory.store(GuestAddress{address},32,bytes.size());memory.write(GuestAddress{address+16},bytes);};
                            put(io.buffer+io.receive_size,local_bytes);put(io.buffer+io.receive_size+io.local_size,remote_bytes);
                        }
                    }
                }
            }
        }
        if(!io.error && io.connected && io.receive_size){
            std::vector<std::uint8_t> bytes(io.receive_size);const auto count=::recv(accepted->second->descriptor,bytes.data(),bytes.size(),MSG_DONTWAIT);
            if(count<0){if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)continue;io.error=socket_error(errno);}
            else{record_network_transfer(static_cast<std::uint64_t>(count),0);io.transferred=static_cast<std::uint32_t>(count);memory.write(GuestAddress{io.buffer},std::span(bytes).first(io.transferred));}
        }
        if(!io.error && !io.connected)continue;
        process_io::record(process_io::Kind::Other,io.transferred);
        io.done=true;memory.store(GuestAddress{io.overlapped},bits,socket_io_status(io.error));memory.store(GuestAddress{io.overlapped+unit},bits,io.transferred);
        set_file_io_signaled(io.listener,true);
        if(io.event){const auto event=runtime_.wait_objects.find(io.event&~UINT64_C(1));if(event!=runtime_.wait_objects.end())event->second.set_signal(true);}
        if(!(io.event&1) && (io.notify || !io.error) && (io.notify || io.error || listener==network.sockets.end() || !(listener->second->completion_modes&1))){const auto port=runtime_.completion_ports.find(io.port);if(port!=runtime_.completion_ports.end())port->second.packets.push_back({io.key,io.overlapped,socket_io_status(io.error),io.transferred});}
        }
    }
    std::erase_if(network.pending_accepts,[](const auto& io){return io.done && io.notify;});
    poll_socket_io(memory);
}
bool WindowsServices::cancel_network_io(GuestMemory& memory,std::uint64_t handle,std::uint64_t thread,std::uint64_t overlapped){
    bool any=false;for(auto& io:runtime_.network.pending_accepts)if(!io.done && (!handle || io.listener==handle || io.accepted==handle) && (!thread || io.thread==thread) && (!overlapped || io.overlapped==overlapped)){io.error=995;any=true;}
    for(auto& io:runtime_.network.pending_socket_io)if(!io.done && (!handle || io.socket==handle) && (!thread || io.thread==thread) && (!overlapped || io.overlapped==overlapped)){io.error=995;any=true;}
    poll_network_io(memory);return any;
}
std::optional<std::uint32_t> WindowsServices::invoke_network_extension(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol=="GetOverlappedResult" || service.symbol=="GetOverlappedResultEx" || service.symbol=="WSAGetOverlappedResult")return invoke_overlapped_result(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;auto& network=runtime_.network;std::uint64_t result=0;
    const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto fail=[&](std::uint32_t error){last_error_=error;result=0;};
    try{
    if(symbol=="GetAcceptExSockaddrs"){
        const auto length=std::uint64_t{d(1)}+d(2)+d(3);
        if(!arg(0) || d(2)<16 || d(3)<16 || length>output_limit || !arg(4) || !arg(5) || !arg(6) || !arg(7))fail(10014);
        else{
            memory.check(GuestAddress{arg(0)},length,Access::Read);const auto local=arg(0)+d(1),remote=local+d(2);const auto a=memory.load(GuestAddress{local},32),b=memory.load(GuestAddress{remote},32);
            if(a>d(2)-16 || b>d(3)-16 || (a!=16 && a!=28) || (b!=16 && b!=28))fail(10022);
            else{memory.check(GuestAddress{arg(4)},unit,Access::Write);memory.check(GuestAddress{arg(5)},4,Access::Write);memory.check(GuestAddress{arg(6)},unit,Access::Write);memory.check(GuestAddress{arg(7)},4,Access::Write);
                memory.store(GuestAddress{arg(4)},bits,local+16);memory.store(GuestAddress{arg(5)},32,a);memory.store(GuestAddress{arg(6)},bits,remote+16);memory.store(GuestAddress{arg(7)},32,b);}
        }
    }else if(!network.startups && service.family!="kernel32.dll")fail(10093);
    else if(!network.sockets.contains(arg(0)))fail(service.family=="kernel32.dll"?6:10038);
    else if(symbol=="CancelIo" || symbol=="CancelIoEx"){
        const bool extended=symbol=="CancelIoEx";const bool found=cancel_network_io(memory,arg(0),extended?0:threads_.current,extended?arg(1):0);
        if(!found && extended)fail(1168);else result=1;
    }else if(symbol=="AcceptEx"){
        const auto listener=network.sockets.at(arg(0));const auto accepted=network.sockets.find(arg(1));const auto required=listener->family==AF_INET6?44U:32U;
        const auto size=std::uint64_t{d(3)}+d(4)+d(5);
        if(accepted==network.sockets.end())fail(10038);
        else if(arg(0)==arg(1) || listener->type!=SOCK_STREAM || accepted->second->type!=SOCK_STREAM || listener->family!=accepted->second->family || !listener->overlapped || !accepted->second->overlapped || !listener->listening)fail(10022);
        else if(!arg(2) || !arg(6) || !arg(7) || d(4)<required || d(5)<required || size>output_limit)fail(10014);
        else if(network.pending_accepts.size()>=1024)fail(10055);
        else if(std::any_of(network.pending_accepts.begin(),network.pending_accepts.end(),[&](const auto& io){return io.accepted==arg(1) || io.overlapped==arg(7);}))fail(10022);
        else{
            sockaddr_storage address{};socklen_t n=sizeof(address);const int queried=::getsockname(accepted->second->descriptor,reinterpret_cast<sockaddr*>(&address),&n);
            const auto occupied=address.ss_family==AF_INET?reinterpret_cast<const sockaddr_in*>(&address)->sin_port:reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port;
            if(queried<0 || (occupied && !accepted->second->reusable))fail(10022);
            else{memory.check(GuestAddress{arg(2)},size,Access::Write);memory.check(GuestAddress{arg(6)},4,Access::Write);memory.check(GuestAddress{arg(7)},x86_?20:32,Access::Write);
                const auto event=memory.load(GuestAddress{arg(7)+(x86_?16:24)},bits);const auto ev=runtime_.wait_objects.find(event&~UINT64_C(1));
                if(event && (ev==runtime_.wait_objects.end() || ev->second.kind!=WaitObject::Kind::Event || !(ev->second.access&2)))fail(6);
                else{if(event)ev->second.set_signal(false);network.completion_flags.erase(arg(7));network.completion_errors.erase(arg(7));memory.store(GuestAddress{arg(7)},bits,0x103);memory.store(GuestAddress{arg(7)+unit},bits,0);
                    PendingAccept io;io.listener=arg(0);io.accepted=arg(1);io.thread=threads_.current;io.buffer=arg(2);io.overlapped=arg(7);io.event=event;io.receive_size=d(3);io.local_size=d(4);io.remote_size=d(5);io.port=listener->completion_port;io.key=listener->completion_key;
                    set_file_io_signaled(arg(0),false);network.pending_accepts.push_back(io);poll_network_io(memory);
                    auto item=std::find_if(network.pending_accepts.begin(),network.pending_accepts.end(),[&](const auto& p){return p.overlapped==arg(7);});
                    if(item->done){if(item->error)fail(item->error);else{memory.store(GuestAddress{arg(6)},32,item->transferred);result=1;}network.pending_accepts.erase(item);}
                    else{item->notify=true;fail(997);}
                }
            }
        }
    }
    }catch(const MemoryFault&){fail(service.family=="kernel32.dll"?87:10014);}
    catch(const std::bad_alloc&){fail(10055);}
    catch(const std::system_error& error){fail(socket_error(error.code().value()));}
    complete_call(state,memory,result);return std::nullopt;
}

void WindowsServices::pump_gui_network(){
    // 每輪仍向核心查詢；有界批次避免每個 watch 都付出一次 poll 系統呼叫，
    // 不配置熱路徑記憶體，也不限制可監看的 socket 總數。
    std::array<pollfd,128> entries;std::array<std::uint64_t,128> handles;std::size_t count=0;
    const auto flush=[&]{
        if(!count)return;
        const auto ready=::poll(entries.data(),static_cast<nfds_t>(count),0);
        if(ready>=0)for(std::size_t index=0;index<count;++index){
            auto& entry=entries[index];
            const auto handle=handles[index];
            const auto monitored=gui_.socket_watches.find(handle);if(monitored==gui_.socket_watches.end())continue;auto& watch=monitored->second;
            const auto found=runtime_.network.sockets.find(handle);if(found==runtime_.network.sockets.end() || found->second->descriptor!=entry.fd)continue;
            if(socket_readiness(*found->second,entry))continue;
            const auto fd=entry.fd;auto& shared=found->second->shared->data();if(watch.generation!=shared.watch_generation.load())continue;
#ifdef __APPLE__
            if((watch.events&watch.armed&4) && socket_urgent_ready(fd))entry.revents|=POLLPRI;
#endif
            if(!entry.revents)continue;
            const auto post=[&](std::uint32_t event,std::uint32_t error=0){if(watch.generation!=shared.watch_generation.load() || !(watch.events&watch.armed&event))return;
                if(watch.event_handle){shared.watch_errors[static_cast<unsigned>(std::countr_zero(event))].store(error);shared.watch_recorded.fetch_or(event);const auto object=runtime_.wait_objects.find(watch.event_handle);if(object!=runtime_.wait_objects.end() && object->second.kind==WaitObject::Kind::Event)object->second.set_signal(true);}
                else{if(gui_.queue.size()>=10000)return;enqueue_gui_message({watch.window,handle,event|(static_cast<std::uint64_t>(error)<<16),watch.message});}watch.armed&=~event;};
            if(watch.connecting && (entry.revents&(POLLOUT|POLLERR|POLLHUP))){int error=0;socklen_t length=sizeof(error);if(::getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&length)<0)error=errno;watch.connecting=false;watch.connected=!error;if(!error)found->second->accepted_at=socket_clock_ticks();post(16,socket_error(error));}
            const bool datagram=found->second->type==SOCK_DGRAM;
            if((watch.connected || datagram) && (entry.revents&POLLOUT))post(2);
            if((entry.revents&POLLPRI) && !(entry.revents&(POLLERR|POLLHUP)))post(4);
            if(entry.revents&(POLLIN|POLLHUP|POLLERR)){
                const bool listening=found->second->listening;
                if(listening)post(8);
                else if(datagram)post(1);
                else if(watch.connected){char byte=0;const auto received=::recv(fd,&byte,1,MSG_PEEK|MSG_DONTWAIT);if(received>0)post(1);else if(!received || (received<0 && errno!=EAGAIN)){post(32,received<0?socket_error(errno):0);watch.connected=false;}}
            }
        }
        count=0;
    };
    for(auto& [handle,watch]:gui_.socket_watches){
        const auto found=runtime_.network.sockets.find(handle);if(found==runtime_.network.sockets.end())continue;auto& shared=found->second->shared->data();
        if(watch.generation!=shared.watch_generation.load())continue;watch.armed|=shared.watch_rearm.exchange(0);
        handles[count]=handle;entries[count++]={found->second->descriptor,static_cast<short>(POLLIN|((watch.events&watch.armed&4)?POLLPRI:0)|((watch.connecting || (watch.armed&2))?POLLOUT:0)),0};
        if(count==entries.size())flush();
    }
    flush();
}
std::span<const WindowsServiceDescriptor> network_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"iphlpapi.dll","GetAdaptersAddresses",5},
        {"iphlpapi.dll","GetExtendedTcpTable",6,0,0,60},{"iphlpapi.dll","GetTcpTable",3,0,0,4},
        {"iphlpapi.dll","GetAdaptersInfo",2},{"iphlpapi.dll","GetNetworkParams",2},
        {"iphlpapi.dll","IpReleaseAddress",1},{"iphlpapi.dll","IpRenewAddress",1},
        {"iphlpapi.dll","GetIfEntry",1},{"iphlpapi.dll","GetIfTable",3},
        {"iphlpapi.dll","GetIfTable2",1},{"iphlpapi.dll","GetIfTable2Ex",2},{"iphlpapi.dll","GetIfEntry2",1},{"iphlpapi.dll","FreeMibTable",1},{"iphlpapi.dll","GetInterfaceInfo",2},
        {"iphlpapi.dll","if_indextoname",2},{"iphlpapi.dll","if_nametoindex",1},
        {"iphlpapi.dll","GetUnicastIpAddressTable",2},{"iphlpapi.dll","GetUnicastIpAddressEntry",1},
        {"iphlpapi.dll","InitializeUnicastIpAddressEntry",1},
        {"iphlpapi.dll","ConvertInterfaceIndexToLuid",2},{"iphlpapi.dll","ConvertInterfaceLuidToIndex",2},
        {"iphlpapi.dll","ConvertLengthToIpv4Mask",2,0,0,1},{"iphlpapi.dll","ConvertIpv4MaskToLength",2,0,0,1},
        {"iphlpapi.dll","ConvertInterfaceLuidToNameA",3},{"iphlpapi.dll","ConvertInterfaceLuidToNameW",3},
        {"iphlpapi.dll","ConvertInterfaceNameToLuidA",2},{"iphlpapi.dll","ConvertInterfaceNameToLuidW",2},
        {"iphlpapi.dll","ConvertInterfaceAliasToLuid",2},{"iphlpapi.dll","ConvertInterfaceLuidToAlias",3},
        {"iphlpapi.dll","ConvertInterfaceLuidToGuid",2},{"iphlpapi.dll","ConvertInterfaceGuidToLuid",2},
        {"winhttp.dll","WinHttpGetIEProxyConfigForCurrentUser",1},
        {"winhttp.dll","WinHttpGetDefaultProxyConfiguration",1},{"winhttp.dll","WinHttpSetDefaultProxyConfiguration",1},
        {"iphlpapi.dll","GetBestRoute2",7},
        {"dhcpcsvc.dll","DhcpCApiInitialize",1},{"dhcpcsvc.dll","DhcpCApiCleanup",0},
        {"dhcpcsvc.dll","DhcpRequestParams",9,(1U<<4)|(1U<<5)},
        {"iphlpapi.dll","GetBestInterface",2},{"iphlpapi.dll","GetBestInterfaceEx",2},
        {"iphlpapi.dll","NotifyAddrChange",2},{"iphlpapi.dll","CancelIPChangeNotify",1},
        {"ws2_32.dll","WSAPoll",3},{"ws2_32.dll","WSASendDisconnect",2},
        {"ws2_32.dll","getprotobyname",1},{"ws2_32.dll","getprotobynumber",1},
        {"ws2_32.dll","WSAAsyncGetHostByName",5},{"ws2_32.dll","WSAAsyncGetHostByAddr",7},
        {"ws2_32.dll","WSAAsyncGetProtoByName",5},{"ws2_32.dll","WSAAsyncGetProtoByNumber",5},
        {"ws2_32.dll","WSAAsyncGetServByName",6},{"ws2_32.dll","WSAAsyncGetServByPort",6},{"ws2_32.dll","WSACancelAsyncRequest",1},
        {"ws2_32.dll","inet_addr",1},{"ws2_32.dll","inet_ntoa",1},{"ws2_32.dll","WSAAsyncSelect",4},{"ws2_32.dll","WSAEventSelect",3},{"ws2_32.dll","WSAEnumNetworkEvents",3},{"ws2_32.dll","WSACreateEvent",0},{"ws2_32.dll","WSAStartup",2},{"ws2_32.dll","WSACleanup",0},{"ws2_32.dll","WSAGetLastError",0},{"ws2_32.dll","WSASetLastError",1},
        {"mswsock.dll","AcceptEx",8},{"mswsock.dll","GetAcceptExSockaddrs",8},{"mswsock.dll","ConnectEx",7},{"mswsock.dll","DisconnectEx",4},
        {"ws2_32.dll","AcceptEx",8},{"ws2_32.dll","GetAcceptExSockaddrs",8},{"ws2_32.dll","WSAGetOverlappedResult",5},
        {"ws2_32.dll","WSAEnumProtocolsA",3},{"ws2_32.dll","WSAEnumProtocolsW",3},
        {"ws2_32.dll","WSCEnumProtocols",4},{"ws2_32.dll","WSCGetProviderPath",4},
        {"ws2_32.dll","WSADuplicateSocketA",3},{"ws2_32.dll","WSADuplicateSocketW",3},{"ws2_32.dll","WSASocketA",6},{"ws2_32.dll","WSASocketW",6},{"ws2_32.dll","WSAIoctl",9},
        {"ws2_32.dll","socket",3},{"ws2_32.dll","closesocket",1},{"ws2_32.dll","bind",3},{"ws2_32.dll","connect",3},
        {"ws2_32.dll","WSAConnect",7,0,0,4},
        {"ws2_32.dll","listen",2},{"ws2_32.dll","accept",3},{"ws2_32.dll","shutdown",2},{"ws2_32.dll","getsockname",3},{"ws2_32.dll","getpeername",3},
        {"ws2_32.dll","send",4},{"ws2_32.dll","recv",4},{"ws2_32.dll","sendto",6},{"ws2_32.dll","recvfrom",6},
        {"ws2_32.dll","select",5},{"ws2_32.dll","ioctlsocket",3},{"ws2_32.dll","setsockopt",5},{"ws2_32.dll","getsockopt",5},
        {"ws2_32.dll","htonl",1},{"ws2_32.dll","htons",1},{"ws2_32.dll","ntohl",1},{"ws2_32.dll","ntohs",1},
        {"ws2_32.dll","__WSAFDIsSet",2},{"ws2_32.dll","getaddrinfo",4},{"ws2_32.dll","freeaddrinfo",1},{"ws2_32.dll","gethostname",2},
        {"ws2_32.dll","GetAddrInfoW",4},{"ws2_32.dll","FreeAddrInfoW",1},{"ws2_32.dll","GetNameInfoW",7},
        {"ws2_32.dll","GetAddrInfoExW",10},{"ws2_32.dll","GetAddrInfoExA",10},{"ws2_32.dll","FreeAddrInfoExW",1},{"ws2_32.dll","FreeAddrInfoEx",1},
        {"ws2_32.dll","GetAddrInfoExCancel",1},{"ws2_32.dll","GetAddrInfoExOverlappedResult",1},
        {"ws2_32.dll","inet_ntop",4},{"ws2_32.dll","InetNtopW",4},{"ws2_32.dll","inet_pton",3},{"ws2_32.dll","InetPtonW",3},
        {"ws2_32.dll","WSAAddressToStringA",5},{"ws2_32.dll","WSAAddressToStringW",5},
        {"ws2_32.dll","WSAStringToAddressA",5},{"ws2_32.dll","WSAStringToAddressW",5},
        {"ws2_32.dll","gethostbyname",1},{"ws2_32.dll","gethostbyaddr",3},
        {"ws2_32.dll","getnameinfo",7},{"ws2_32.dll","getservbyname",2},{"ws2_32.dll","getservbyport",2},
        {"ws2_32.dll","WSASendMsg",6,0,0,4},{"mswsock.dll","WSASendMsg",6,0,0,4},
        {"mswsock.dll","WSARecvMsg",5,0,0,4},
        {"ws2_32.dll","WSASend",7},{"ws2_32.dll","WSARecv",7},{"ws2_32.dll","WSASendTo",9},{"ws2_32.dll","WSARecvFrom",9},
        {"ws2_32.dll","WSASetServiceA",3},{"ws2_32.dll","WSASetServiceW",3},
        {"ws2_32.dll","WSAEnumNameSpaceProvidersA",2},{"ws2_32.dll","WSAEnumNameSpaceProvidersW",2},
        {"ws2_32.dll","WSALookupServiceBeginA",3},{"ws2_32.dll","WSALookupServiceBeginW",3},
        {"ws2_32.dll","WSALookupServiceNextA",4},{"ws2_32.dll","WSALookupServiceNextW",4},{"ws2_32.dll","WSALookupServiceEnd",1}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_network(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(service.symbol.starts_with("WSALookupService") || service.symbol.starts_with("WSASetService") || service.symbol.starts_with("WSAEnumNameSpaceProviders"))return invoke_socket_namespace(state,memory,service);
    if(service.symbol=="WSAPoll" || service.symbol=="WSASendDisconnect")return invoke_socket_poll(state,memory,service);
    if(service.symbol.starts_with("WSAAsyncGet") || service.symbol=="WSACancelAsyncRequest" || service.symbol=="gethostbyname" || service.symbol=="gethostbyaddr" || service.symbol=="getservbyname" || service.symbol=="getservbyport" || service.symbol=="getprotobyname" || service.symbol=="getprotobynumber")return invoke_socket_database(state,memory,service);
    if(service.symbol=="AcceptEx" || service.symbol=="GetAcceptExSockaddrs" || service.symbol=="WSAGetOverlappedResult")return invoke_network_extension(state,memory,service);
    if(service.symbol=="ConnectEx" || service.symbol=="DisconnectEx")return invoke_socket_io(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;auto& network=runtime_.network;
    const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
    if(symbol=="WSAIoctl" && multicast_filter_ioctl(static_cast<std::uint32_t>(arg(1))))return invoke_socket_io(state,memory,service);
    if(symbol=="send" || symbol=="sendto" || symbol=="WSASend" || symbol=="WSASendTo" || symbol=="WSASendMsg")if(const auto socket=network.sockets.find(arg(0));network.startups && socket!=network.sockets.end() && socket->second->send_shutdown){last_error_=10058;arg.return_to(state,UINT32_MAX);return std::nullopt;}
    if(symbol=="WSASendMsg"){const auto socket=network.sockets.find(arg(0));if(socket!=network.sockets.end() && socket->second->overlapped && arg(4))return invoke_socket_io(state,memory,service);}
    if(symbol=="WSARecvMsg"){const auto socket=network.sockets.find(arg(0));if(socket!=network.sockets.end() && socket->second->overlapped && (arg(3) || arg(4)))return invoke_socket_io(state,memory,service);}
    if(symbol=="WSASend" || symbol=="WSARecv" || symbol=="WSASendTo" || symbol=="WSARecvFrom"){const bool addressed=symbol=="WSASendTo" || symbol=="WSARecvFrom";const auto socket=network.sockets.find(arg(0));if(socket!=network.sockets.end() && socket->second->overlapped && (arg(addressed?7:5) || arg(addressed?8:6)))return invoke_socket_io(state,memory,service);}
    const bool address_ex=symbol=="GetAddrInfoExW" || symbol=="GetAddrInfoExA";
    const bool wide=symbol.ends_with('W'),resolve_address=symbol=="getaddrinfo" || symbol=="GetAddrInfoW" || address_ex,resolve_name=symbol=="getnameinfo" || symbol=="GetNameInfoW";
    const bool host_lookup=symbol=="gethostbyname" || symbol=="gethostbyaddr";
    const bool ntop=symbol=="inet_ntop" || symbol=="InetNtopW",pton=symbol=="inet_pton" || symbol=="InetPtonW";
    if(!network.external_io && (symbol=="socket" || symbol.starts_with("WSASocket") || resolve_address || resolve_name || host_lookup || symbol=="gethostname" || symbol=="getservbyname" || symbol=="getservbyport"))
        throw HostCallFault("compare 不執行無法重播的宿主網路操作");
    std::uint64_t result=0;
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto sint=[&](unsigned i){return std::bit_cast<std::int32_t>(u32(i));};
    const bool socket_result=symbol=="socket" || symbol.starts_with("WSASocket") || symbol=="accept";
    const bool pointer_result=host_lookup || ntop || symbol=="inet_ntoa" || symbol=="getservbyname" || symbol=="getservbyport";
    const bool catalog_spi=symbol=="WSCEnumProtocols" || symbol=="WSCGetProviderPath";std::uint32_t catalog_error=0;
    const auto fail=[&](std::uint32_t error){if(catalog_spi)catalog_error=error;else last_error_=error;result=socket_result?UINT64_MAX:pointer_result?0:(resolve_address || resolve_name)?error:UINT32_MAX;};
    const auto retry_socket_operation=[&]{threads_.yield=true;schedule(state,memory);};
    const auto receive_error=[&](const NativeSocket& socket,int error)->std::optional<std::uint32_t>{
        if(socket.type!=SOCK_DGRAM && socket.type!=SOCK_RAW)return socket_error(error);
        if(error==EINPROGRESS){retry_socket_operation();return std::nullopt;}
        if(error!=EAGAIN && error!=EWOULDBLOCK)return socket_error(error);
        const auto flags=::fcntl(socket.descriptor,F_GETFL);if(flags<0)return socket_error(errno);
        if(flags&O_NONBLOCK)return socket_error(error);
        auto& thread=threads_.items.at(threads_.current);std::uint64_t deadline=UINT64_MAX;
        if(thread.wait.kind==ThreadWaitKind::SocketSelect)deadline=thread.wait.deadline;
        else{timeval timeout{};socklen_t size=sizeof(timeout);if(::getsockopt(socket.descriptor,SOL_SOCKET,SO_RCVTIMEO,&timeout,&size)<0)return socket_error(errno);
            if(timeout.tv_sec || timeout.tv_usec)deadline=runtime_.clock_ticks+static_cast<std::uint64_t>(timeout.tv_sec)*10000000+static_cast<std::uint64_t>(timeout.tv_usec)*10;}
        if(deadline<=runtime_.clock_ticks)return 10060;
        thread.wait={};thread.wait.kind=ThreadWaitKind::SocketSelect;thread.wait.deadline=deadline;thread.wait.handles={arg(0)};thread.wait.socket_events={POLLIN};thread.wait.alertable=true;thread.status=ThreadStatus::Waiting;schedule(state,memory);return std::nullopt;
    };
    const auto read_text=[&](std::uint64_t pointer){return wide?utf16_to_utf8(read_utf16(memory,GuestAddress{pointer})):read_ansi(memory,pointer,4096);};
    const auto encoded_text=[&](std::string_view text){if(wide)return utf16_bytes(utf8_to_utf16(text));std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);return bytes;};
    // 對客體仍是同步 API；等待時只暫停呼叫執行緒，由宿主工作執行緒解析。
    const auto resolver=[&](socket_database::Query query){
        auto found=network.resolvers.find(threads_.current);
        if(found==network.resolvers.end()){
            if(network.resolvers.size()>=64)throw std::bad_alloc{};
            if(std::getenv("X64ARM_TRACE_NETWORK_STATE"))std::fprintf(stderr,"[net] resolver thread=%llu kind=%u node=%s service=%s\n",static_cast<unsigned long long>(threads_.current),static_cast<unsigned>(query.kind),query.name.c_str(),query.protocol.c_str());
            auto job=socket_database::submit(std::move(query),mapping_wake());if(!job)throw std::bad_alloc{};
            try{found=network.resolvers.emplace(threads_.current,job).first;}
            catch(...){job->cancelled.store(true);throw;}
        }
        if(!found->second->ready.load(std::memory_order_acquire))return std::shared_ptr<socket_database::Job>{};
        auto job=std::move(found->second);network.resolvers.erase(found);return job;
    };
    const auto wait_resolver=[&]{auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::Resolver;schedule(state,memory);};
    const auto make_socket=[&](int fd,int family,int type,int protocol) {
        std::shared_ptr<NativeSocket> value;try{value=std::make_shared<NativeSocket>(fd,family,type,protocol);}catch(...){::close(fd);throw;}
        if (::fcntl(fd,F_SETFD,FD_CLOEXEC)<0) {fail(socket_error(errno));return;}
#ifdef SO_NOSIGPIPE
        const int enabled=1;
        if (::setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled))<0) {fail(socket_error(errno));return;}
#endif
        sockaddr_storage peer{};socklen_t peer_size=sizeof(peer);if(::getpeername(fd,reinterpret_cast<sockaddr*>(&peer),&peer_size)==0)value->accepted_at=socket_clock_ticks();
        result=network.next_socket++;network.sockets.emplace(result,std::move(value));runtime_.handle_flags[result]=1;
    };
    try {
        if(symbol=="WSAEnumProtocolsA" || symbol=="WSAEnumProtocolsW" || symbol=="WSCEnumProtocols"){
            if(catalog_spi){if(!arg(3)){arg.return_to(state,UINT32_MAX);return std::nullopt;}memory.check(GuestAddress{arg(3)},4,Access::Write);}
            if(!catalog_spi && !network.startups)fail(10093);
            else if(!arg(2))fail(10014);
            else {const auto capacity=memory.load(GuestAddress{arg(2)},32);memory.check(GuestAddress{arg(2)},4,Access::Write);std::vector<unsigned> filter;
                if(arg(0)){bool terminated=false;for(unsigned i=0;i<128;++i){const auto protocol=static_cast<unsigned>(memory.load(GuestAddress{arg(0)+i*4},32));if(!protocol){terminated=true;break;}filter.push_back(protocol);}if(!terminated)throw std::invalid_argument("協定過濾陣列未終止");}
                const bool unicode=catalog_spi || wide;std::vector<std::uint8_t> bytes;unsigned count=0;
                for(const auto& protocol:socket_protocols)if(!arg(0) || std::find(filter.begin(),filter.end(),protocol.protocol)!=filter.end()){const auto info=socket_protocol_info(protocol,unicode);bytes.insert(bytes.end(),info.begin(),info.end());++count;}
                memory.store(GuestAddress{arg(2)},32,bytes.size());if(capacity<bytes.size())fail(10055);else if(!bytes.empty() && !arg(1))fail(10014);else {if(!bytes.empty())memory.write(GuestAddress{arg(1)},bytes);result=count;}
            }
        }else if(symbol=="WSCGetProviderPath"){
            if(!arg(3)){arg.return_to(state,UINT32_MAX);return std::nullopt;}memory.check(GuestAddress{arg(3)},4,Access::Write);
            if(!arg(0) || !arg(2))fail(10014);else {bool matches=true;for(unsigned i=0;i<4;++i)if(memory.load(GuestAddress{arg(0)+i*4},32)!=socket_provider_id[i])matches=false;
                if(!matches)fail(10022);else {const auto capacity=static_cast<std::int32_t>(memory.load(GuestAddress{arg(2)},32));const auto path=utf16_bytes(u"C:\\Windows\\System32\\mswsock.dll");memory.store(GuestAddress{arg(2)},32,path.size()/2);
                    if(capacity<0 || static_cast<std::uint64_t>(capacity)<path.size()/2 || !arg(1))fail(10014);else memory.write(GuestAddress{arg(1)},path);}
            }
        }else if (symbol=="WSAStartup") {
            const auto major=u32(0)&255U,minor=(u32(0)>>8)&255U;
            if (!major) result=10092;
            else if (network.startups==UINT32_MAX) result=10067;
            else {
                std::vector<std::uint8_t> data(x86_?400U:408U);data[0]=major==1?1:2;data[1]=static_cast<std::uint8_t>(std::min(minor,major==1?1U:major==2?2U:255U));
                if (major>2) data[1]=2;
                data[2]=2;data[3]=2;
                const auto limits=x86_?390U:4U;data[limits]=0;data[limits+1]=4;data[limits+2]=0xe3;data[limits+3]=0xff;
                constexpr std::string_view description="x64arm Winsock",status="Running";
                std::copy(description.begin(),description.end(),data.begin()+(x86_?4:16));std::copy(status.begin(),status.end(),data.begin()+(x86_?261:273));
                memory.write(GuestAddress{arg(1)},data);++network.startups;
            }
        } else if (symbol=="WSAGetLastError") result=last_error_;
        else if (symbol=="WSASetLastError") last_error_=u32(0);
        else if (symbol=="htons" || symbol=="ntohs") result=htons(static_cast<std::uint16_t>(arg(0)));
        else if (symbol=="htonl" || symbol=="ntohl") result=htonl(u32(0));
        else if(ntop || pton){
            const auto family=host_family(u32(0));
            if(family!=AF_INET && family!=AF_INET6)fail(10047);
            else {
                std::array<std::uint8_t,16> bytes{};const std::size_t size=family==AF_INET?4:16;
                if(pton){memory.check(GuestAddress{arg(2)},size,Access::Write);const auto text=read_text(arg(1));const auto status=::inet_pton(family,text.c_str(),bytes.data());if(status<0)fail(socket_error(errno));else {result=static_cast<unsigned>(status);if(status)memory.write(GuestAddress{arg(2)},std::span(bytes).first(size));}}
                else {memory.read(GuestAddress{arg(1)},std::span(bytes).first(size));std::array<char,INET6_ADDRSTRLEN> text{};
                    if(!::inet_ntop(family,bytes.data(),text.data(),text.size()))fail(socket_error(errno));
                    else {const auto encoded=encoded_text(text.data());if(!arg(2) || arg(3)<encoded.size()/(wide?2:1))fail(87);else {memory.write(GuestAddress{arg(2)},encoded);result=arg(2);}}
                }
            }
        }
        else if(symbol=="WSACreateEvent"){
            if(runtime_.wait_objects.size()>=process_handle_limit){last_error_=8;result=0;}
            else {WaitObject object;object.kind=WaitObject::Kind::Event;object.maximum=1;object.manual_reset=true;result=runtime_.allocate_wait_handle();runtime_.wait_objects.emplace(result,std::move(object));}
        }
        else if (symbol=="__WSAFDIsSet") {
            const auto count=memory.load(GuestAddress{arg(1)},32);
            if (count>1024) fail(10022);
            else for (std::uint64_t i=0;i<count;++i) if (memory.load(GuestAddress{arg(1)+unit+i*unit},bits)==arg(0)) result=1;
        } else if (!network.startups) fail(10093);
    else if (symbol=="WSACleanup") {if (--network.startups==0) {cancel_socket_lookups();network.namespace_lookups.clear();cancel_network_io(memory);for(const auto& [handle,socket]:network.sockets){(void)socket;runtime_.handle_flags.erase(handle);}for(const auto& [handle,watch]:gui_.socket_watches){const auto socket=network.sockets.find(handle);if(socket!=network.sockets.end() && watch.generation==socket->second->shared->data().watch_generation.load())socket->second->shared->data().watch_generation.fetch_add(1);}network.sockets.clear();network.duplications.clear();network.completion_flags.clear();network.completion_errors.clear();gui_.socket_watches.clear();}}
        else if(symbol=="WSADuplicateSocketA" || symbol=="WSADuplicateSocketW"){
            const auto socket=network.sockets.find(arg(0));
            if(socket==network.sockets.end())fail(10038);
            else if(!arg(2))fail(10014);
            else{memory.check(GuestAddress{arg(2)},wide?628:372,Access::Write);std::uint32_t token=0;
                const auto value=socket->second;const auto family=guest_family(value->family);const auto protocol=static_cast<unsigned>(value->protocol?value->protocol:value->type==SOCK_STREAM?IPPROTO_TCP:IPPROTO_UDP);
                auto bytes=socket_protocol_info({family,static_cast<unsigned>(value->type),protocol,(family==23?3U:1U)+(value->type==SOCK_STREAM?0U:1U)},wide);
                const auto error=duplicate_socket_to_process(memory,arg(0),u32(1),token);if(error)fail(error);
                else{for(unsigned i=0;i<4;++i)bytes[112+i]=static_cast<std::uint8_t>(token>>(8*i));memory.write(GuestAddress{arg(2)},bytes);}}
        }
        else if (symbol=="socket" || symbol.starts_with("WSASocket")) {
            const bool extended=symbol!="socket";auto af=sint(0),type=sint(1),protocol=sint(2);bool valid=true;std::uint32_t token=0;
            if(extended && arg(3)){
                memory.check(GuestAddress{arg(3)},wide?628:372,Access::Read);
                for(unsigned i=0;i<4;++i)if(memory.load(GuestAddress{arg(3)+20+4*i},32)!=socket_provider_id[i])valid=false;
                if(memory.load(GuestAddress{arg(3)+40},32)!=1)valid=false;token=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+112},32));
                if(af==-1)af=static_cast<std::int32_t>(memory.load(GuestAddress{arg(3)+76},32));
                if(type==-1)type=static_cast<std::int32_t>(memory.load(GuestAddress{arg(3)+88},32));
                if(protocol==-1)protocol=static_cast<std::int32_t>(memory.load(GuestAddress{arg(3)+92},32));
            }
            const auto family=host_family(static_cast<unsigned>(af));
            if(extended && u32(4))fail(10022);
            else if(extended && (u32(5)&~0x81U))fail(10045);
            else if(!valid)fail(10022);
            else if(token){
                poll_handle_transfers(memory,true);const auto pending=network.duplications.find(token);
                if(pending==network.duplications.end())fail(10022);
                else{const auto socket=pending->second.socket;const auto actual_protocol=socket->protocol?socket->protocol:socket->type==SOCK_STREAM?IPPROTO_TCP:IPPROTO_UDP;
                    if(family!=socket->family || type!=socket->type || (protocol && protocol!=actual_protocol) || ((u32(5)&1)!=0)!=socket->overlapped)fail(10022);
                    else if(network.sockets.size()>=1024)fail(10024);
                    else{result=network.next_socket++;network.sockets.emplace(result,socket);runtime_.handle_flags[result]=(u32(5)&0x80)?0:1;network.duplications.erase(pending);}}
            }
            else if (family!=AF_INET && family!=AF_INET6) fail(10047);
            else if (type!=SOCK_STREAM && type!=SOCK_DGRAM) fail(10044);
            else if (network.sockets.size()>=1024) fail(10024);
            else {const auto fd=::socket(family,type,protocol);if (fd<0) fail(socket_error(errno));else{make_socket(fd,family,type,protocol);if(result!=UINT64_MAX && extended){network.sockets.at(result)->overlapped=(u32(5)&1)!=0;runtime_.handle_flags[result]=(u32(5)&0x80)?0:1;}}}
        } else if (symbol=="gethostname") {
            if (sint(1)<=0 || u32(1)>32768) fail(10014);
            else {
                memory.check(GuestAddress{arg(0)},u32(1),Access::Write);std::array<char,256> name{};
                if (::gethostname(name.data(),name.size()-1)<0) fail(socket_error(errno));
                else {const auto length=std::strlen(name.data())+1;if (length>u32(1)) fail(10014);else memory.write(GuestAddress{arg(0)},{reinterpret_cast<const std::uint8_t*>(name.data()),length});}
            }
        } else if(symbol=="inet_addr"){result=::inet_addr(read_ansi(memory,arg(0)).c_str());}
        else if(symbol=="inet_ntoa"){in_addr address{};address.s_addr=u32(0);std::array<char,INET_ADDRSTRLEN> text{};if(::inet_ntop(AF_INET,&address,text.data(),text.size())){auto& buffer=network.service_buffers[threads_.current+0x100000000ULL];if(!buffer)buffer=runtime_allocate(memory,{},INET_ADDRSTRLEN);memory.write(GuestAddress{buffer},std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));result=buffer;}else fail(socket_error(errno));}
        else if(symbol.starts_with("WSAStringToAddress")){
            const auto family=u32(1);const unsigned required=family==2?16:family==23?28:0;
            if(!required)fail(10022);
            else if(!arg(0) || !arg(4))fail(10014);
            else if(!socket_text_provider(memory,arg(2),family,wide))fail(10022);
            else{
                const auto capacity=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)},32)));
                memory.check(GuestAddress{arg(4)},4,Access::Write);
                if(capacity<static_cast<int>(required) || !arg(3)){memory.store(GuestAddress{arg(4)},32,required);fail(10014);}
                else{
                    memory.check(GuestAddress{arg(3)},required,Access::Write);
                    const auto address=parse_address_text(family,read_text(arg(0)));
                    if(!address)fail(10022);
                    else{const auto bytes=address_bytes(reinterpret_cast<const sockaddr*>(&address->storage));memory.write(GuestAddress{arg(3)},bytes);memory.store(GuestAddress{arg(4)},32,bytes.size());}
                }
            }
        }
        else if(symbol.starts_with("WSAAddressToString")){
            if(!arg(0) || !u32(1) || !arg(4))fail(10022);
            else {const auto address=read_address(memory,arg(0),u32(1));
                if(!address.size || !socket_text_provider(memory,arg(2),guest_family(address.storage.ss_family),wide))fail(10022);
                else {memory.check(GuestAddress{arg(4)},4,Access::Write);const auto capacity=memory.load(GuestAddress{arg(4)},32);
                    std::array<char,INET6_ADDRSTRLEN> host{};const bool ipv6=address.storage.ss_family==AF_INET6;
                    const auto& v4=*reinterpret_cast<const sockaddr_in*>(&address.storage);const auto& v6=*reinterpret_cast<const sockaddr_in6*>(&address.storage);
                    const auto* raw=ipv6?static_cast<const void*>(&v6.sin6_addr):static_cast<const void*>(&v4.sin_addr);
                    if(!::inet_ntop(address.storage.ss_family,raw,host.data(),host.size()))fail(socket_error(errno));
                    else {std::string text=host.data();if(ipv6 && v6.sin6_scope_id)text+="%"+std::to_string(v6.sin6_scope_id);const auto port=ntohs(ipv6?v6.sin6_port:v4.sin_port);
                        if(port){if(ipv6)text="["+text+"]";text+=":"+std::to_string(port);}
                        const auto encoded=encoded_text(text);memory.store(GuestAddress{arg(4)},32,encoded.size()/(wide?2:1));
                        if(!arg(3) || capacity<encoded.size()/(wide?2:1))fail(10014);else memory.write(GuestAddress{arg(3)},encoded);
                    }
                }
            }
        }
        else if (resolve_name) {
            const auto address=read_address(memory,arg(0),u32(1));const auto flags_value=u32(6);int native_flags=0;
            if (flags_value&1)native_flags|=NI_NOFQDN;if(flags_value&2)native_flags|=NI_NUMERICHOST;if(flags_value&4)native_flags|=NI_NAMEREQD;
            if (flags_value&8)native_flags|=NI_NUMERICSERV;if(flags_value&16)native_flags|=NI_DGRAM;
            if (flags_value&~31U || !address.size || (!arg(2) && !arg(4))) result=10022;
            else if (u32(3)>65536 || u32(5)>65536 || (arg(2) && !u32(3)) || (arg(4) && !u32(5))) result=10014;
            else {
                if (arg(2))memory.check(GuestAddress{arg(2)},u32(3)*(wide?2U:1U),Access::Write);if (arg(4))memory.check(GuestAddress{arg(4)},u32(5)*(wide?2U:1U),Access::Write);
                socket_database::Query query;query.kind=socket_database::Kind::NameInfo;query.flags=native_flags;
                const auto* bytes=reinterpret_cast<const std::uint8_t*>(&address.storage);query.address.assign(bytes,bytes+address.size);
                query.number=arg(2)?u32(3)*(wide?4U:1U):0;query.service_capacity=arg(4)?u32(5)*(wide?4U:1U):0;
                const auto job=resolver(std::move(query));if(!job){wait_resolver();return std::nullopt;}
                const auto& host=job->result.name;const auto& port=job->result.protocol;
                if (job->result.error)result=job->result.error;
                else {
                    const auto name=arg(2)?encoded_text(host):std::vector<std::uint8_t>{},service_name=arg(4)?encoded_text(port):std::vector<std::uint8_t>{};
                    if(name.size()/(wide?2:1)>u32(3) || service_name.size()/(wide?2:1)>u32(5))result=10014;
                    else {if(!name.empty())memory.write(GuestAddress{arg(2)},name);if(!service_name.empty())memory.write(GuestAddress{arg(4)},service_name);}
                }
            }
            if (result)last_error_=static_cast<std::uint32_t>(result);
        } else if (resolve_address) {
            // ADDRINFOEX 在 ai_addr 後多 ai_blob／ai_bloblen／ai_provider，ai_next 移到最後；其餘欄位位置相同。
            const unsigned hints_arg=address_ex?4:2,out_arg=address_ex?5:3;
            const unsigned entry_size=address_ex?(x86_?44:72):(x86_?32:48),canon=x86_?20:24,addr=x86_?24:32,next=address_ex?(x86_?40:64):(x86_?28:40);
            memory.store(GuestAddress{arg(out_arg)},bits,0);addrinfo hints{};std::uint32_t validation_error=0;
            // 只提供 DNS／全部命名空間；非同步 OVERLAPPED 由完成常式或事件回報。
            if(address_ex && u32(2)!=0 && u32(2)!=12)validation_error=10022;
            if (arg(hints_arg) && !validation_error) {
                const auto pointer=arg(hints_arg);const auto flags_value=memory.load(GuestAddress{pointer},32);
                hints.ai_family=host_family(static_cast<unsigned>(memory.load(GuestAddress{pointer+4},32)));
                hints.ai_socktype=static_cast<int>(memory.load(GuestAddress{pointer+8},32));hints.ai_protocol=static_cast<int>(memory.load(GuestAddress{pointer+12},32));
                if(flags_value&~UINT64_C(0xd0f))validation_error=10022;
                else if(hints.ai_family<0)validation_error=10047;
                else if(memory.load(GuestAddress{pointer+16},bits) || memory.load(GuestAddress{pointer+canon},bits) || memory.load(GuestAddress{pointer+addr},bits) || memory.load(GuestAddress{pointer+next},bits))validation_error=11003;
                else if(address_ex)for(unsigned field=addr+unit;field<next;field+=unit)if(memory.load(GuestAddress{pointer+field},bits))validation_error=11003;
                if (flags_value&1) hints.ai_flags|=AI_PASSIVE;if (flags_value&2) hints.ai_flags|=AI_CANONNAME;
                if (flags_value&4) hints.ai_flags|=AI_NUMERICHOST;if (flags_value&8) hints.ai_flags|=AI_NUMERICSERV;
                if (flags_value&0x100) hints.ai_flags|=AI_ALL;if (flags_value&0x400) hints.ai_flags|=AI_ADDRCONFIG;if (flags_value&0x800) hints.ai_flags|=AI_V4MAPPED;
            }
            if(validation_error)result=validation_error;
            else if(!arg(0) && !arg(1))result=11001;
            else {
                const auto node=arg(0)?read_text(arg(0)):std::string{},port=arg(1)?read_text(arg(1)):std::string{};
                socket_database::Query query;query.kind=socket_database::Kind::AddressInfo;query.name=node;query.protocol=port;query.has_name=arg(0)!=0;query.has_service=arg(1)!=0;
                query.family=static_cast<unsigned>(hints.ai_family);query.flags=hints.ai_flags;query.socket_type=hints.ai_socktype;query.protocol_number=hints.ai_protocol;
                const auto job=resolver(std::move(query));if(!job){wait_resolver();return std::nullopt;}
                auto* list=job->result.address_info.get();
                if (job->result.error) result=job->result.error;
                else {
                    std::vector<std::vector<std::uint8_t>> addresses,names;std::vector<const addrinfo*> sources;
                    for (auto* current=list;current && sources.size()<64;current=current->ai_next) {
                        auto bytes=address_bytes(current->ai_addr);if (bytes.empty()) continue;
                        sources.push_back(current);addresses.push_back(std::move(bytes));names.push_back(current->ai_canonname?encoded_text(current->ai_canonname):std::vector<std::uint8_t>{});
                    }
                    std::size_t size=sources.size()*entry_size;for (std::size_t i=0;i<sources.size();++i) size+=addresses[i].size()+names[i].size()+16;
                    const auto base=allocate_heap(memory,size);
                    if (!base || sources.empty()) {if (base) free_heap(memory,base,state);result=sources.empty()?11004U:8U;}
                    else {
                        memory.write(GuestAddress{base},std::vector<std::uint8_t>(size));
                        auto cursor=base+sources.size()*entry_size;
                        for (std::size_t i=0;i<sources.size();++i) {
                            const auto at=base+i*entry_size;const auto* current=sources[i];
                            memory.store(GuestAddress{at},32,0);memory.store(GuestAddress{at+4},32,guest_family(current->ai_family));
                            memory.store(GuestAddress{at+8},32,static_cast<unsigned>(current->ai_socktype));memory.store(GuestAddress{at+12},32,static_cast<unsigned>(current->ai_protocol));
                            cursor=(cursor+7)&~UINT64_C(7);memory.store(GuestAddress{at+16},bits,addresses[i].size());memory.store(GuestAddress{at+addr},bits,cursor);memory.write(GuestAddress{cursor},addresses[i]);cursor+=addresses[i].size();
                            if (!names[i].empty()) {cursor=(cursor+7)&~UINT64_C(7);memory.store(GuestAddress{at+canon},bits,cursor);memory.write(GuestAddress{cursor},names[i]);cursor+=names[i].size();}
                            memory.store(GuestAddress{at+next},bits,i+1<sources.size()?at+entry_size:0);
                        }
                        network.addresses.insert(base);memory.store(GuestAddress{arg(out_arg)},bits,base);
                    }
                }
            }
            if (result) {memory.store(GuestAddress{arg(out_arg)},bits,0);last_error_=static_cast<std::uint32_t>(result);}
            if(address_ex && arg(7) && !validation_error){
                // 查詢已在呼叫端執行緒完成；依非同步契約回 WSA_IO_PENDING，結果碼寫入 OVERLAPPED.Internal
                // （GetAddrInfoExOverlappedResult 讀取），再觸發完成常式或 hEvent。完成後的取消 handle 不再有效。
                const auto overlapped=arg(7);memory.store(GuestAddress{overlapped},bits,result);
                if(arg(9))memory.store(GuestAddress{arg(9)},bits,overlapped);
                if(arg(8)){
                    if(const auto error=queue_pool_callback(state,memory,{arg(8),{result,0,overlapped}})){
                        if(const auto base=memory.load(GuestAddress{arg(out_arg)},bits);base && network.addresses.erase(base))free_heap(memory,base,state);
                        memory.store(GuestAddress{arg(out_arg)},bits,0);result=error;last_error_=error;
                    }else{result=997;last_error_=997;}
                }else{
                    if(const auto event=memory.load(GuestAddress{overlapped+(x86_?16:24)},bits)){
                        const auto found=runtime_.wait_objects.find(event&~UINT64_C(1));
                        if(found!=runtime_.wait_objects.end() && found->second.kind==WaitObject::Kind::Event)found->second.set_signal(true);
                    }
                    result=997;last_error_=997;
                }
            }
        } else if (symbol=="GetAddrInfoExCancel") {
            // 查詢都在 GetAddrInfoExW 回傳前完成，沒有可取消的進行中操作。
            if(!arg(0)){result=10022;last_error_=10022;}else {(void)memory.load(GuestAddress{arg(0)},bits);result=6;last_error_=6;}
        } else if (symbol=="GetAddrInfoExOverlappedResult") {
            if(!arg(0)){result=10022;last_error_=10022;}else result=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)},bits));
        } else if (symbol=="freeaddrinfo" || symbol=="FreeAddrInfoW" || symbol=="FreeAddrInfoExW" || symbol=="FreeAddrInfoEx") {
            if (arg(0) && network.addresses.erase(arg(0))) free_heap(memory,arg(0),state);
            else if (arg(0)) throw HostCallFault("freeaddrinfo 收到非配置起點");
        } else if (symbol=="select") {
            std::array<std::vector<std::uint64_t>,3> sets;std::vector<pollfd> polls;std::vector<std::uint64_t> handles;
            bool valid=true;int timeout=-1;
            for (unsigned set=0;set<3;++set) if (arg(set+1)) {
                const auto pointer=arg(set+1),count=memory.load(GuestAddress{pointer},32);
                if (count>1024) {valid=false;break;}memory.check(GuestAddress{pointer},unit+count*unit,Access::Write);
                for (std::uint64_t i=0;i<count;++i) {
                    const auto handle=memory.load(GuestAddress{pointer+unit+i*unit},bits);const auto found=network.sockets.find(handle);
                    if (found==network.sockets.end()) {valid=false;break;}
                    sets[set].push_back(handle);auto entry=std::find(handles.begin(),handles.end(),handle);
                    if (entry==handles.end()) {handles.push_back(handle);polls.push_back({found->second->descriptor,0,0});entry=std::prev(handles.end());}
                    polls[static_cast<std::size_t>(entry-handles.begin())].events|=static_cast<short>(set==0?POLLIN:set==1?POLLOUT:POLLPRI);
                }
            }
            if (arg(4)) {
                const auto seconds=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)},32)));
                const auto micros=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+4},32)));
                if (seconds<0 || micros<0 || micros>=1000000) valid=false;
                else timeout=static_cast<int>(std::min<std::int64_t>(INT_MAX,std::int64_t{seconds}*1000+(micros+999)/1000));
            }
            if (!valid || polls.empty()) fail(valid?10022:10038);
            else {
                // 核對原生與已提交佇列；有界工作續作時保留原始呼叫框架。
                auto status=::poll(polls.data(),static_cast<nfds_t>(polls.size()),0);bool deferred=false;
                if(status>=0){status=0;for(std::size_t i=0;i<polls.size();++i){deferred=socket_readiness(*network.sockets.at(handles[i]),polls[i]) || deferred;if(polls[i].revents)++status;}}
                if(deferred){retry_socket_operation();return std::nullopt;}
                if(std::getenv("X64ARM_TRACE_NETWORK_STATE")){
                    std::fprintf(stderr,"[net] select thread=%llu ticks=%llu timeout_ms=%d status=%d",static_cast<unsigned long long>(threads_.current),static_cast<unsigned long long>(runtime_.clock_ticks),timeout,status);
                    for(std::size_t i=0;i<polls.size();++i)std::fprintf(stderr," socket=%llx/%x/%x",static_cast<unsigned long long>(handles[i]),static_cast<unsigned>(polls[i].events),static_cast<unsigned>(polls[i].revents));std::fprintf(stderr,"\n");
                }
                auto& thread=threads_.items.at(threads_.current);
                if(!status && timeout!=0 && !(thread.wait.kind==ThreadWaitKind::SocketSelect && thread.wait.deadline<=runtime_.clock_ticks)){
                    const auto deadline=thread.wait.kind==ThreadWaitKind::SocketSelect?thread.wait.deadline:timeout<0?UINT64_MAX:runtime_.clock_ticks+static_cast<std::uint64_t>(timeout)*10000;
                    thread.wait={};thread.wait.kind=ThreadWaitKind::SocketSelect;thread.wait.deadline=deadline;thread.wait.handles=handles;
                    for(const auto& entry:polls)thread.wait.socket_events.push_back(static_cast<std::uint32_t>(entry.events));
                    thread.status=ThreadStatus::Waiting;schedule(state,memory);return std::nullopt;
                }
                if(thread.wait.kind==ThreadWaitKind::SocketSelect)thread.wait={};
                if (status<0) fail(socket_error(errno));
                else for (unsigned set=0;set<3;++set) if (arg(set+1)) {
                    std::uint32_t count=0;
                    for (const auto handle : sets[set]) {
                        const auto at=static_cast<std::size_t>(std::find(handles.begin(),handles.end(),handle)-handles.begin());
                        const auto mask=set==0?POLLIN|POLLHUP:set==1?POLLOUT:POLLPRI|POLLERR;
                        if (polls[at].revents&mask) memory.store(GuestAddress{arg(set+1)+unit+count++*unit},bits,handle);
                    }
                    memory.store(GuestAddress{arg(set+1)},32,count);result+=count;
                }
            }
        } else {
            const auto found=network.sockets.find(arg(0));
            if (found==network.sockets.end()) fail(10038);
            else {
                const auto socket=found->second;const auto fd=socket->descriptor;
                const auto register_watch=[&]{auto& data=socket->shared->data();const auto generation=data.watch_generation.fetch_add(1)+1;data.watch_rearm.store(0);data.watch_recorded.store(0);for(auto& error:data.watch_errors)error.store(0);
                    std::erase_if(gui_.socket_watches,[&](const auto& entry){const auto other=network.sockets.find(entry.first);return entry.first!=arg(0) && other!=network.sockets.end() && other->second->shared==socket->shared;});return generation;};
                if(symbol=="WSAEventSelect"){
                    const auto events=u32(2);const auto object=runtime_.wait_objects.find(arg(1));
                    if(events&~0x3fU)fail(10045);
                    else if(events && (object==runtime_.wait_objects.end() || object->second.kind!=WaitObject::Kind::Event || !(object->second.access&2)))fail(10022);
                    else{const auto flags_value=::fcntl(fd,F_GETFL);if(flags_value<0 || ::fcntl(fd,F_SETFL,flags_value|O_NONBLOCK)<0)fail(socket_error(errno));
                        else{const auto generation=register_watch();if(!events)gui_.socket_watches.erase(arg(0));
                        else{auto& watch=gui_.socket_watches[arg(0)];const auto connecting=watch.connecting;watch={};watch.generation=generation;watch.connecting=connecting;watch.event_handle=arg(1);watch.events=watch.armed=events;sockaddr_storage peer{};socklen_t size=sizeof(peer);watch.connected=::getpeername(fd,reinterpret_cast<sockaddr*>(&peer),&size)==0;}}}
                }else if(symbol=="WSAEnumNetworkEvents"){
                    const auto object=runtime_.wait_objects.find(arg(1));if(arg(1) && (object==runtime_.wait_objects.end() || object->second.kind!=WaitObject::Kind::Event || !(object->second.access&2)))fail(10022);
                    else if(!arg(2))fail(10014);
                    else{memory.check(GuestAddress{arg(2)},44,Access::Write);pump_gui_network();std::array<std::uint8_t,44> bytes{};auto& data=socket->shared->data();const auto recorded=data.watch_recorded.exchange(0);for(unsigned i=0;i<4;++i)bytes[i]=static_cast<std::uint8_t>(recorded>>(i*8));
                        for(unsigned j=0;j<10;++j){const auto error=data.watch_errors[j].load();for(unsigned i=0;i<4;++i)bytes[4+j*4+i]=static_cast<std::uint8_t>(error>>(i*8));}memory.write(GuestAddress{arg(2)},bytes);if(arg(1))object->second.set_signal(false);
                        if(std::getenv("X64ARM_TRACE_NETWORK_STATE") && recorded){
                            const auto saved_errno=errno;
                            std::fprintf(stderr,"[net_event] pid=%u socket=%llx mask=%x connect_error=%u close_error=%u\n",process_id(),static_cast<unsigned long long>(arg(0)),recorded,data.watch_errors[4].load(),data.watch_errors[5].load());errno=saved_errno;
                        }}
                }else if(symbol=="WSAAsyncSelect"){
                    if(u32(3)&~0x3fU || (u32(3) && !gui_.windows.contains(arg(1))))fail(10022);
                    else {const auto flags_value=::fcntl(fd,F_GETFL);if(flags_value<0 || ::fcntl(fd,F_SETFL,flags_value|O_NONBLOCK)<0)fail(socket_error(errno));else{const auto generation=register_watch();if(!u32(3))gui_.socket_watches.erase(arg(0));else {auto& watch=gui_.socket_watches[arg(0)];const auto connecting=watch.connecting;watch={};watch.generation=generation;watch.connecting=connecting;watch.window=arg(1);watch.message=u32(2);watch.events=u32(3);watch.armed=u32(3);sockaddr_storage peer{};socklen_t peer_size=sizeof(peer);if(::getpeername(fd,reinterpret_cast<sockaddr*>(&peer),&peer_size)==0)watch.connected=true;}}}
                }
                else if(symbol=="WSARecvMsg"){
                    if(socket->type!=SOCK_DGRAM && socket->type!=SOCK_RAW)fail(10045);
                    else if(!arg(2))fail(10014);
                    else{
                        const auto received=read_receive_message(memory,arg(1),x86_,socket->family,output_limit);
                        memory.check(GuestAddress{arg(2)},4,Access::Write);
                        sockaddr_storage local{};socklen_t local_size=sizeof local;
                        if(::getsockname(fd,reinterpret_cast<sockaddr*>(&local),&local_size)<0)fail(socket_error(errno));
                        else if(socket->type==SOCK_DGRAM && !(socket->family==AF_INET?reinterpret_cast<sockaddr_in*>(&local)->sin_port:reinterpret_cast<sockaddr_in6*>(&local)->sin6_port))fail(10022);
                        else{
                            socket->shared->data().watch_rearm.fetch_or(1);
                            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(received.total)),control(std::min<std::uint32_t>(received.control_size,65536));
                            iovec vector{bytes.data(),bytes.size()};sockaddr_storage source{};msghdr message{};
                            message.msg_iov=&vector;message.msg_iovlen=1;message.msg_name=&source;message.msg_namelen=sizeof source;
                            std::size_t used{};std::uint32_t flags{};
                            const auto size=socket_receive_guest_message(*socket,message,MSG_DONTWAIT|(received.flags&2?MSG_PEEK:0),x86_,control,used,flags);
                            if(size<0){const auto error=receive_error(*socket,errno);if(!error)return std::nullopt;fail(*error);}
                            else{
                                std::size_t cursor=0;for(const auto& buffer:received.buffers){const auto count=std::min<std::size_t>(buffer.length,static_cast<std::size_t>(size)-cursor);if(count)memory.write(GuestAddress{buffer.pointer},{bytes.data()+cursor,count});cursor+=count;if(cursor==static_cast<std::size_t>(size))break;}
                                const auto address=address_bytes(reinterpret_cast<const sockaddr*>(&source));
                                write_receive_message(memory,received.pointer,x86_,received.name,received.control,address,std::span(control).first(used),flags);
                                memory.store(GuestAddress{arg(2)},32,static_cast<std::uint64_t>(size));
                                process_io::record(process_io::Kind::Read,static_cast<std::uint64_t>(size));record_network_transfer(received.flags&2?0:static_cast<std::uint64_t>(size),0);
                                if(flags&(guest_message_truncated|guest_control_truncated))fail(10040);
                            }
                        }
                    }
                }
                else if (symbol=="WSASend" || symbol=="WSARecv" || symbol=="WSASendTo" || symbol=="WSARecvFrom" || symbol=="WSASendMsg") {
                    const bool messaging=symbol=="WSASendMsg",sending=symbol.starts_with("WSASend"),addressed=messaging || symbol=="WSASendTo" || symbol=="WSARecvFrom";const auto sent=messaging?read_send_message(memory,arg(1),x86_):SendMessage{};const auto count=messaging?sent.count:u32(2),flags_value=messaging?u32(2):sending?u32(4):static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)},32));const auto buffer_pointer=messaging?sent.buffers:arg(1),name_pointer=messaging?sent.name:addressed?arg(5):0;const auto name_size=messaging?sent.name_size:addressed?u32(6):0;
                    if(!sending)socket->shared->data().watch_rearm.fetch_or((flags_value&1)?4U:1U);
                    if(messaging && socket->type!=SOCK_DGRAM && socket->type!=SOCK_RAW)fail(10045);
                    else if (!count || count>1024 || !arg(3) || (flags_value&~(sending?5U:3U))) fail(10022);
                    else if (!messaging && socket->overlapped && (arg(addressed?7:5) || arg(addressed?8:6))) fail(10045);
                    else {
                        memory.check(GuestAddress{arg(3)},4,Access::Write);if(!sending)memory.check(GuestAddress{arg(4)},4,Access::Write);
                        Address destination;sockaddr_storage source{};const bool peer=addressed && (socket->type==SOCK_DGRAM || messaging);
                        if(peer && sending && name_pointer){destination=read_address(memory,name_pointer,name_size);if(!destination.size){fail(10047);arg.return_to(state,result);return std::nullopt;}}
                        if(peer && !sending && arg(5)){const auto required=socket->family==AF_INET6?28U:16U;if(!arg(6) || memory.load(GuestAddress{arg(6)},32)<required){fail(10014);arg.return_to(state,result);return std::nullopt;}memory.check(GuestAddress{arg(5)},required,Access::Write);memory.check(GuestAddress{arg(6)},4,Access::Write);}
                        std::vector<std::vector<std::uint8_t>> buffers(count);std::vector<iovec> vectors(count);std::vector<std::uint64_t> pointers(count);std::uint64_t total=0;bool valid=true;
                        for(unsigned i=0;i<count;++i) {
                            const auto length=memory.load(GuestAddress{buffer_pointer+i*(x86_?8U:16U)},32);pointers[i]=memory.load(GuestAddress{buffer_pointer+i*(x86_?8U:16U)+(x86_?4U:8U)},x86_?32:64);
                            if(length>output_limit-total){valid=false;break;}total+=length;buffers[i].resize(static_cast<std::size_t>(length));memory.check(GuestAddress{pointers[i]},static_cast<std::size_t>(length),sending?Access::Read:Access::Write);
                            if(sending)memory.read(GuestAddress{pointers[i]},buffers[i]);vectors[i]={buffers[i].data(),buffers[i].size()};
                        }
                        if(!valid)fail(10055);
                        else {
                            msghdr message{};message.msg_iov=vectors.data();message.msg_iovlen=static_cast<decltype(message.msg_iovlen)>(vectors.size());if(messaging && !sent.control.empty()){message.msg_control=const_cast<std::uint8_t*>(sent.control.data());message.msg_controllen=static_cast<decltype(message.msg_controllen)>(sent.control.size());}int flags_host=(flags_value&1?MSG_OOB:0)|(sending?(flags_value&4?MSG_DONTROUTE:0):(flags_value&2?MSG_PEEK:0));
#ifdef MSG_NOSIGNAL
                            if(sending)flags_host|=MSG_NOSIGNAL;
#endif
                            if(peer && sending && destination.size){message.msg_name=&destination.storage;message.msg_namelen=destination.size;}
                            if(peer && !sending){message.msg_name=&source;message.msg_namelen=sizeof(source);}
                            const auto size=sending?send_message(fd,socket->type,message,flags_host,socket->protocol) : socket_receive_message(*socket,message,flags_host);
                            process_io::record(sending?process_io::Kind::Write:process_io::Kind::Read,size<0?0:static_cast<std::uint64_t>(size));
                            if(messaging && size<0 && std::getenv("X64ARM_TRACE_NETWORK_STATE"))std::fprintf(stderr,"[net] sendmsg native_error=%d family=%d control_size=%zu\n",errno,socket->family,sent.control.size());
                            if(sending && size<0 && errno==EAGAIN)socket->shared->data().watch_rearm.fetch_or(2);
                            if(size<0){if(!sending){const auto error=receive_error(*socket,errno);if(!error)return std::nullopt;fail(*error);}else fail(socket_error(errno));}
                            else {
                                record_network_transfer(!sending && !(flags_host&MSG_PEEK)?static_cast<std::uint64_t>(size):0,sending?static_cast<std::uint64_t>(size):0);
                                auto remaining=static_cast<std::size_t>(size);
                                if(!sending)for(unsigned i=0;i<count && remaining;++i){const auto copied=std::min(remaining,buffers[i].size());memory.write(GuestAddress{pointers[i]},std::span(buffers[i]).first(copied));remaining-=copied;}
                                memory.store(GuestAddress{arg(3)},32,static_cast<std::uint64_t>(size));if(!sending)memory.store(GuestAddress{arg(4)},32,message.msg_flags&MSG_OOB?1:0);
                                if(peer && !sending && arg(5)){const auto bytes=address_bytes(reinterpret_cast<const sockaddr*>(&source));memory.write(GuestAddress{arg(5)},bytes);memory.store(GuestAddress{arg(6)},32,bytes.size());}
                                if(message.msg_flags&MSG_TRUNC)fail(10040);
                            }
                        }
                    }
                } else if (symbol=="closesocket") {cancel_network_io(memory,arg(0));const auto watch=gui_.socket_watches.find(arg(0));if(watch!=gui_.socket_watches.end() && watch->second.generation==socket->shared->data().watch_generation.load())socket->shared->data().watch_generation.fetch_add(1);gui_.socket_watches.erase(arg(0));runtime_.handle_flags.erase(arg(0));network.sockets.erase(found);std::erase_if(network.completion_flags,[&](const auto& item){return item.second.first==arg(0);});std::erase_if(network.completion_errors,[&](const auto& item){return item.second.first==arg(0);});}
                else if (symbol=="bind" || symbol=="connect" || symbol=="WSAConnect") {
                    const bool connecting=symbol!="bind",extended=symbol=="WSAConnect";
                    std::uint32_t option_error=0;
                    if(extended){
                        // TCP／UDP 不支援 connect payload／QoS；不把資料誤當一般 send，也不忽略請求。
                        if(arg(6))option_error=10022;
                        if(!option_error && arg(3)){
                            memory.check(GuestAddress{arg(3)},2*unit,Access::Read);
                            const auto count=memory.load(GuestAddress{arg(3)},32);
                            const auto data=memory.load(GuestAddress{arg(3)+unit},bits);
                            if(count){if(!data)option_error=10014;else {memory.check(GuestAddress{data},count,Access::Read);option_error=10043;}}
                        }
                        if(!option_error && arg(5)){memory.check(GuestAddress{arg(5)},64+2*unit,Access::Read);option_error=10045;}
                        if(!option_error && arg(4)){
                            memory.check(GuestAddress{arg(4)},2*unit,Access::Read);
                            memory.check(GuestAddress{arg(4)},4,Access::Write);
                            const auto count=memory.load(GuestAddress{arg(4)},32);
                            const auto data=memory.load(GuestAddress{arg(4)+unit},bits);
                            if(count){if(!data)option_error=10014;else memory.check(GuestAddress{data},count,Access::Write);}
                        }
                    }
                    if(option_error){fail(option_error);arg.return_to(state,result);return std::nullopt;}
                    const auto address=read_address(memory,arg(1),u32(2));
                    if (!address.size) fail(10047);
                    else {const auto* pointer=reinterpret_cast<const sockaddr*>(&address.storage);
                        const auto status=symbol=="bind"?::bind(fd,pointer,address.size ) : ::connect(fd,pointer,address.size);
                        if(std::getenv("X64ARM_TRACE_NETWORK_STATE")){
                            const auto saved_errno=errno;char host[NI_MAXHOST]{},port[NI_MAXSERV]{};
                            if(!::getnameinfo(pointer,address.size,host,sizeof(host),port,sizeof(port),NI_NUMERICHOST|NI_NUMERICSERV))std::fprintf(stderr,"[net_endpoint] pid=%u ms=%lld api=%.*s socket=%llx host=%s port=%s status=%d native_error=%d\n",process_id(),static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()),static_cast<int>(symbol.size()),symbol.data(),static_cast<unsigned long long>(arg(0)),host,port,status,status<0?saved_errno:0);
                            errno=saved_errno;
                        }
                        if(extended && arg(4) && (!status || errno==EINPROGRESS))memory.store(GuestAddress{arg(4)},32,0);
                        if(connecting && gui_.socket_watches.contains(arg(0)) && (!status || errno==EINPROGRESS))gui_.socket_watches.at(arg(0)).connecting=true;
                        if (status<0) fail(errno==EINPROGRESS?10035U:socket_error(errno));else if(connecting)socket->accepted_at=socket_clock_ticks();else{sockaddr_storage local{};socklen_t size=sizeof(local);if(::getsockname(fd,reinterpret_cast<sockaddr*>(&local),&size)==0)socket->set_bound_address(address_bytes(reinterpret_cast<sockaddr*>(&local)));}}
                } else if (symbol=="listen") {if (::listen(fd,sint(1))<0) fail(socket_error(errno));else socket->listening=true;}
                else if (symbol=="shutdown") {if (u32(1)>2) fail(10022);else if (::shutdown(fd,sint(1))<0) fail(socket_error(errno));else if(u32(1))socket->send_shutdown=true;}
                else if (symbol=="getsockname" || symbol=="getpeername" || symbol=="accept") {
                    const bool accept=symbol=="accept";const auto pointer=arg(1),size_pointer=arg(2);
                    if(accept)socket->shared->data().watch_rearm.fetch_or(8);
                    const auto capacity=pointer?memory.load(GuestAddress{size_pointer},32):0;const auto required=socket->family==AF_INET6?28U:16U;
                    if ((!pointer && !accept) || (pointer && capacity<required)) fail(10014);
                    else {
                        if (pointer) {memory.check(GuestAddress{pointer},required,Access::Write);memory.check(GuestAddress{size_pointer},4,Access::Write);}
                        sockaddr_storage address{};socklen_t size=sizeof(address);auto* native=reinterpret_cast<sockaddr*>(&address);
                        const auto status=accept?::accept(fd,native,&size):symbol=="getsockname"?::getsockname(fd,native,&size ) : ::getpeername(fd,native,&size);
                        if (status<0) fail(socket_error(errno));
                        else {if (pointer) {const auto bytes=address_bytes(native);memory.write(GuestAddress{pointer},bytes);memory.store(GuestAddress{size_pointer},32,bytes.size());}
                            if (accept) {make_socket(status,socket->family,socket->type,socket->protocol);if(result!=UINT64_MAX && gui_.socket_watches.contains(arg(0))){auto watch=gui_.socket_watches.at(arg(0));watch.connecting=false;watch.connected=true;watch.armed=watch.events;watch.recorded=0;watch.errors={};watch.generation=network.sockets.at(result)->shared->data().watch_generation.fetch_add(1)+1;gui_.socket_watches[result]=watch;const auto flags_value=::fcntl(status,F_GETFL);if(flags_value>=0)(void)::fcntl(status,F_SETFL,flags_value|O_NONBLOCK);}}}
                    }
                } else if (symbol=="ioctlsocket" || symbol=="WSAIoctl") {
                    const bool extended=symbol=="WSAIoctl",input=u32(1)==0x8004667e;const auto pointer=extended && !input?arg(4):arg(2);
                    if(extended && socket->overlapped && (arg(7) || arg(8))){fail(10045);arg.return_to(state,result);return std::nullopt;}
                    if(extended){if(!arg(6) || ((u32(1)==0x8004667e || u32(1)==0x4004667f || u32(1)==0x40047307) && (!pointer || u32(input?3:5)<4))){fail(10014);arg.return_to(state,result);return std::nullopt;}memory.store(GuestAddress{arg(6)},32,0);}
                    if(extended && u32(1)==0xc8000006U){
                        const unsigned width=x86_?4:8;
                        if(!arg(2) || u32(3)!=16 || !arg(4) || u32(5)<width)fail(10014);
                        else{std::array<std::uint8_t,16> guid{};memory.read(GuestAddress{arg(2)},guid);
                            const std::array<std::uint8_t,16> accept{0xf1,0x7d,0x36,0xb5,0xac,0xcb,0xcf,0x11,0x95,0xca,0,0x80,0x5f,0x48,0xa1,0x92};auto addresses=accept;addresses[0]=0xf2;
                            const std::array<std::uint8_t,16> connect{0xb9,0x07,0xa2,0x25,0xf3,0xdd,0x60,0x46,0x8e,0xe9,0x76,0xe5,0x8c,0x74,0x06,0x3e},disconnect{0x11,0x2e,0xda,0x7f,0x30,0x86,0x6f,0x43,0xa0,0x31,0xf5,0x36,0xa6,0xee,0xc1,0x57};
                            const std::array<std::uint8_t,16> sendmsg{0x12,0xe7,0x41,0xa4,0x4f,0x75,0xca,0x43,0x84,0xa7,0x0d,0xee,0x44,0xcf,0x60,0x6d};
                            const std::array<std::uint8_t,16> recvmsg{0xc8,0xd7,0x89,0xf6,0x1f,0x6f,0x6b,0x43,0x8a,0x53,0xe5,0x4f,0xe3,0x51,0xc3,0x22};
                            const char* name=guid==recvmsg?"WSARecvMsg":guid==sendmsg?"WSASendMsg":guid==accept?"AcceptEx":guid==addresses?"GetAcceptExSockaddrs":guid==connect?"ConnectEx":guid==disconnect?"DisconnectEx":nullptr;
                            if(!name)fail(10045);else{memory.store(GuestAddress{arg(4)},x86_?32:64,resolve("mswsock.dll",name).value);memory.store(GuestAddress{arg(6)},32,width);}
                        }
                    } else if (u32(1)==0x8004667e) {
                        const auto mode=memory.load(GuestAddress{pointer},32);const auto flags_value=::fcntl(fd,F_GETFL);
                        if(!mode && gui_.socket_watches.contains(arg(0)))fail(10022);
                        else if (flags_value<0 || ::fcntl(fd,F_SETFL,mode?flags_value|O_NONBLOCK:flags_value&~O_NONBLOCK)<0) fail(socket_error(errno));
                    } else if (u32(1)==0x4004667f || u32(1)==0x40047307) {
                        memory.check(GuestAddress{pointer},4,Access::Write);int available=0;
                        if ((u32(1)==0x4004667f?readable_socket_bytes(*socket,available) : ::ioctl(fd,SIOCATMARK,&available))<0){if(errno==EINPROGRESS){retry_socket_operation();return std::nullopt;}fail(socket_error(errno));}else{memory.store(GuestAddress{pointer},32,static_cast<unsigned>(available));if(extended)memory.store(GuestAddress{arg(6)},32,4);}
                    } else if(extended && u32(1)==0x98000004U){
                        if(!arg(2) || u32(3)<12)fail(10014);
                        else if(u32(5))fail(10022);
                        else if(socket->type!=SOCK_STREAM)fail(10042);
                        else{
                            const int enabled=memory.load(GuestAddress{arg(2)},32)!=0;
                            const auto timeout=memory.load(GuestAddress{arg(2)+4},32),interval=memory.load(GuestAddress{arg(2)+8},32);
                            if(enabled && (!timeout || !interval))fail(10022);
                            else if(::setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&enabled,sizeof(enabled))<0)fail(socket_error(errno));
                            else if(enabled){
                                const auto seconds=static_cast<int>((timeout+999)/1000),period=static_cast<int>((interval+999)/1000);
#ifdef __APPLE__
                                constexpr int idle_option=TCP_KEEPALIVE;
#else
                                constexpr int idle_option=TCP_KEEPIDLE;
#endif
                                if(::setsockopt(fd,IPPROTO_TCP,idle_option,&seconds,sizeof(seconds))<0 || ::setsockopt(fd,IPPROTO_TCP,TCP_KEEPINTVL,&period,sizeof(period))<0)fail(socket_error(errno));
                            }
                        }
                    } else fail(extended?10045:10022);
                } else if(symbol=="setsockopt" && u32(1)==0xffff && u32(2)==0x700b){
                    if(!arg(3) || u32(4)!=(x86_?4U:8U))fail(10014);
                    else{const auto listening_handle=memory.load(GuestAddress{arg(3)},x86_?32:64);const auto listener=network.sockets.find(listening_handle);
                        if(!socket->accept_listener || socket->accept_listener!=listening_handle || listener==network.sockets.end())fail(10022);
                        else if(copy_accept_options(listener->second->descriptor,fd)<0)fail(socket_error(errno));
                        else{socket->accept_context=true;if(gui_.socket_watches.contains(listening_handle)){auto watch=gui_.socket_watches.at(listening_handle);watch.connecting=false;watch.connected=true;watch.armed=watch.events;watch.recorded=0;watch.errors={};watch.generation=socket->shared->data().watch_generation.fetch_add(1)+1;gui_.socket_watches[arg(0)]=watch;}}
                    }
                } else if(symbol=="setsockopt" && u32(1)==0xffff && u32(2)==0x7010){
                    if(arg(3) || u32(4))fail(10022);
                    else if(socket->accepted_at==UINT64_MAX)fail(10057);
                    else socket->connect_context=true;
                } else if(symbol=="getsockopt" && u32(1)==0xffff && (u32(2)==0x700c || u32(2)==2)){
                    if(!arg(3) || !arg(4) || memory.load(GuestAddress{arg(4)},32)<4)fail(10014);
                    else{memory.store(GuestAddress{arg(3)},32,u32(2)==2?static_cast<std::uint64_t>(socket->listening):socket->accepted_at==UINT64_MAX?UINT32_MAX:std::min<std::uint64_t>(UINT32_MAX-1,(socket_clock_ticks()-socket->accepted_at)/10000000));memory.store(GuestAddress{arg(4)},32,4);}
                } else if (symbol=="getsockopt" && u32(1)==0xffff && (u32(2)==0x2004 || u32(2)==0x2005)) {
                    const bool unicode=u32(2)==0x2005;const unsigned size=unicode?628U:372U;
                    if(!arg(3) || !arg(4))fail(10014);
                    else if(memory.load(GuestAddress{arg(4)},32)<size){memory.store(GuestAddress{arg(4)},32,size);fail(10014);}
                    else {
                        const bool tcp=socket->type==SOCK_STREAM,ipv6=socket->family==AF_INET6;
                        memory.write(GuestAddress{arg(3)},socket_protocol_info({guest_family(socket->family),static_cast<unsigned>(socket->type),static_cast<unsigned>(socket->protocol?socket->protocol:tcp?IPPROTO_TCP:IPPROTO_UDP),(ipv6?3U:1U)+(tcp?0U:1U)},unicode));
                        memory.store(GuestAddress{arg(4)},32,size);
                    }
                } else if ((symbol=="setsockopt" || symbol=="getsockopt") && socket_receive_option(*socket,u32(1),u32(2)).bit) {
                    const bool get=symbol=="getsockopt";const auto capacity=get?memory.load(GuestAddress{arg(4)},32):u32(4);
                    if(capacity<4)fail(10014);
                    else{
                        std::uint32_t value{};
                        if(get){memory.check(GuestAddress{arg(3)},4,Access::Write);memory.check(GuestAddress{arg(4)},4,Access::Write);}
                        else value=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));
                        const auto option=socket_receive_option(*socket,u32(1),u32(2));
                        const auto status=get?socket_receive_option_get(*socket,option,value):socket_receive_option_set(*socket,option,value);
                        if(status<0){if(errno==EINPROGRESS){retry_socket_operation();return std::nullopt;}fail(socket_error(errno));}
                        else if(get){memory.store(GuestAddress{arg(3)},32,value);memory.store(GuestAddress{arg(4)},32,4);}
                    }
                } else if ((symbol=="setsockopt" || symbol=="getsockopt") && multicast_option(u32(1),u32(2),socket->family,socket->type).option) {
                    const auto option=multicast_option(u32(1),u32(2),socket->family,socket->type);
                    const bool get=symbol=="getsockopt";
                    if(get && multicast_membership(option.kind))fail(10042);
                    else {
                        const auto capacity=get?memory.load(GuestAddress{arg(4)},32):u32(4);
                        const auto required=get?std::size_t{4}:option.size;
                        if(capacity<required)fail(10014);
                        else {
                            std::array<std::uint8_t,264> bytes{};
                            if(get){memory.check(GuestAddress{arg(3)},4,Access::Write);memory.check(GuestAddress{arg(4)},4,Access::Write);}
                            else memory.read(GuestAddress{arg(3)},std::span(bytes).first(required));
                            const auto status=get?multicast_get(fd,option,std::span(bytes).first(4)):
                                socket_multicast_change(*socket,option,std::span<const std::uint8_t>(bytes).first(required));
                            if(status<0){if(errno==EINPROGRESS){retry_socket_operation();return std::nullopt;}fail(socket_error(errno));}
                            else if(get){memory.write(GuestAddress{arg(3)},std::span<const std::uint8_t>(bytes).first(4));memory.store(GuestAddress{arg(4)},32,4);}
                        }
                    }
                } else if ((symbol=="setsockopt" || symbol=="getsockopt") && u32(1)==0xffff && u32(2)==0xfffffffbU) {
                    // SO_EXCLUSIVEADDRUSE（~SO_REUSEADDR）：須在 bind 前設定，且與 SO_REUSEADDR 互斥。Darwin 在雙方都未開
                    // SO_REUSEADDR／SO_REUSEPORT 時已拒絕重複綁定，因此保持宿主 socket 不可重用並記錄旗標即可。
                    const bool get=symbol=="getsockopt";auto& exclusive=socket->shared->data().exclusive_address;
                    const auto capacity=get?memory.load(GuestAddress{arg(4)},32):u32(4);
                    if(capacity<4)fail(10014);
                    else if(get){memory.check(GuestAddress{arg(3)},4,Access::Write);memory.store(GuestAddress{arg(3)},32,exclusive.load());memory.store(GuestAddress{arg(4)},32,4);}
                    else{
                        const bool enable=memory.load(GuestAddress{arg(3)},32)!=0;int reuse=0;socklen_t size=sizeof(reuse);
                        if(socket->shared->data().bound_size)fail(10022);
                        else if(enable && ::getsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,&size)<0)fail(socket_error(errno));
                        else if(enable && reuse)fail(10022);
                        else exclusive.store(enable?1U:0U);
                    }
                } else if (symbol=="setsockopt" || symbol=="getsockopt") {
                    const bool get=symbol=="getsockopt";int level=0,option=0;bool timeout=false,linger_value=false;
                    const auto fragmentation=fragmentation_option(u32(1),u32(2),socket->family,socket->type);
                    const auto randomize=randomize_port_option(u32(1),u32(2));
                    if (u32(1)==0xffff) {
                        level=SOL_SOCKET;
                        switch (u32(2)) {
                        case 4:option=SO_REUSEADDR;break;case 8:option=SO_KEEPALIVE;break;case 16:option=SO_DONTROUTE;break;case 32:option=SO_BROADCAST;break;
                        case 128:option=SO_LINGER;linger_value=true;break;case 256:option=SO_OOBINLINE;break;
                        case 0x1001:option=SO_SNDBUF;break;case 0x1002:option=SO_RCVBUF;break;
                        case 0x1005:option=SO_SNDTIMEO;timeout=true;break;case 0x1006:option=SO_RCVTIMEO;timeout=true;break;
                        case 0x1007:option=SO_ERROR;break;case 0x1008:option=SO_TYPE;break;
                        case 0x3005:option=randomize;break;default:break;
                        }
                    } else if (u32(1)==6 && u32(2)==1) {level=IPPROTO_TCP;option=TCP_NODELAY;}
                    else if (u32(1)==41 && u32(2)==27) {level=IPPROTO_IPV6;option=IPV6_V6ONLY;}
                    else if (fragmentation.option) {level=fragmentation.level;option=fragmentation.option;}
                    const auto capacity=get?memory.load(GuestAddress{arg(4)},32):u32(4);
                    if (!option) fail(10042);
                    else if (capacity<4) fail(10014);
                    else {
                        int integer=0;timeval time{};linger linger_option{};
                        void* value=timeout?static_cast<void*>(&time):linger_value?static_cast<void*>(&linger_option):static_cast<void*>(&integer);
                        socklen_t size=timeout?sizeof(time):linger_value?sizeof(linger_option):sizeof(integer);
                        if (get) {memory.check(GuestAddress{arg(3)},4,Access::Write);memory.check(GuestAddress{arg(4)},4,Access::Write);}
                        else if (timeout) {const auto millis=memory.load(GuestAddress{arg(3)},32);time.tv_sec=static_cast<time_t>(millis/1000);time.tv_usec=static_cast<decltype(time.tv_usec)>((millis%1000)*1000);}
                        else if (linger_value) {linger_option.l_onoff=static_cast<int>(memory.load(GuestAddress{arg(3)},16));linger_option.l_linger=static_cast<int>(memory.load(GuestAddress{arg(3)+2},16));}
                        else integer=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32)));
                        if((fragmentation.option || randomize) && !get)integer=socket_option_boolean(static_cast<std::uint32_t>(integer));
                        const bool send_buffer=level==SOL_SOCKET && option==SO_SNDBUF;
                        const bool zero_buffer=send_buffer && !get && integer==0;
                        // Winsock 的零緩衝由既有 pending I/O 持有客體資料直到宿主消耗。
                        // Darwin 沒有相同選項；1 byte 在連線後會逐位元組等待 ACK，導致 TLS 逾時。
                        // 保留可用的傳輸層佇列，客體 getsockopt 仍由共享狀態回報零。
                        if(zero_buffer)integer=65536;
                        // SO_REUSEADDR 與 SO_EXCLUSIVEADDRUSE 互斥（Winsock 回 WSAEINVAL）。
                        if(!get && level==SOL_SOCKET && option==SO_REUSEADDR && integer && socket->shared->data().exclusive_address.load()){fail(10022);arg.return_to(state,result);return std::nullopt;}
                        const auto status=get?::getsockopt(fd,level,option,value,&size ) : ::setsockopt(fd,level,option,value,size);
                        if (status<0) {
                            const auto native_error=errno;
                            if(std::getenv("X64ARM_TRACE_NETWORK_ERRORS"))std::fprintf(stderr,"[socket_option_error] pid=%u socket=%llx get=%u level=%x option=%x integer=%d capacity=%llu native_error=%d windows_error=%u\n",process_id(),static_cast<unsigned long long>(arg(0)),get?1U:0U,u32(1),u32(2),integer,static_cast<unsigned long long>(capacity),native_error,socket_error(native_error));
                            fail(socket_error(native_error));
                        }
                        else if (get) {
                            if(send_buffer && socket->shared->data().zero_send_buffer.load())integer=0;
                            const auto number=timeout?static_cast<std::uint32_t>(time.tv_sec*1000+time.tv_usec/1000):linger_value?
                                (static_cast<std::uint32_t>(linger_option.l_linger)<<16)|static_cast<std::uint16_t>(linger_option.l_onoff!=0):
                                level==SOL_SOCKET && option==SO_ERROR?socket_error(integer):
                                ((level==SOL_SOCKET && (u32(2)==4 || u32(2)==8 || u32(2)==16 || u32(2)==32 || u32(2)==256)) || level==IPPROTO_TCP || level==IPPROTO_IPV6 || fragmentation.option || randomize)?static_cast<std::uint32_t>(integer!=0):static_cast<std::uint32_t>(integer);
                            memory.store(GuestAddress{arg(3)},32,number);memory.store(GuestAddress{arg(4)},32,4);
                        }
                        else if(send_buffer)socket->shared->data().zero_send_buffer.store(zero_buffer?1U:0U);
                    }
                } else if (symbol=="send" || symbol=="sendto" || symbol=="recv" || symbol=="recvfrom") {
                    if(symbol=="recv" || symbol=="recvfrom")socket->shared->data().watch_rearm.fetch_or((u32(3)&1)?4U:1U);
                    const bool send=symbol=="send" || symbol=="sendto",addressed=symbol=="sendto" || symbol=="recvfrom";
                    const auto count=u32(2),flags_value=u32(3);
                    if (sint(2)<0 || count>output_limit || flags_value&~15U) fail(10022);
                    else {
                        int flags_host=0;if (flags_value&1) flags_host|=MSG_OOB;if (flags_value&2) flags_host|=MSG_PEEK;
                        if (flags_value&4) flags_host|=MSG_DONTROUTE;if (flags_value&8) flags_host|=MSG_WAITALL;
#ifdef MSG_NOSIGNAL
                        if (send) flags_host|=MSG_NOSIGNAL;
#endif
                        std::vector<std::uint8_t> bytes(count);memory.check(GuestAddress{arg(1)},count,send?Access::Read:Access::Write);
                        sockaddr_storage source{};socklen_t source_size=sizeof(source);Address destination;
                        if (addressed && send && socket->type!=SOCK_STREAM && arg(4)) {
                            destination=read_address(memory,arg(4),u32(5));
                            if(!destination.size){fail(10047);arg.return_to(state,result);return std::nullopt;}
                        }
                        if (addressed && !send && arg(4)) {
                            const auto capacity=memory.load(GuestAddress{arg(5)},32); const auto required=socket->family==AF_INET6?28U:16U;
                            if (capacity<required) {fail(10014);arg.return_to(state,result);return std::nullopt;}
                            memory.check(GuestAddress{arg(4)},required,Access::Write);memory.check(GuestAddress{arg(5)},4,Access::Write);
                        }
                        ssize_t size=-1;int received_flags=0;
                        if (send) {
                            memory.read(GuestAddress{arg(1)},bytes);
                            iovec vector{bytes.data(),bytes.size()};msghdr message{};
                            message.msg_iov=&vector;message.msg_iovlen=1;
                            if(destination.size){message.msg_name=&destination.storage;message.msg_namelen=destination.size;}
                            size=send_message(fd,socket->type,message,flags_host,socket->protocol);
                        } else size=socket_receive_buffer(*socket,bytes.data(),bytes.size(),flags_host,addressed?&source:nullptr,&source_size,received_flags);
                        process_io::record(send?process_io::Kind::Write:process_io::Kind::Read,size<0?0:static_cast<std::uint64_t>(size));
                        if (size<0) {if(send && errno==EAGAIN)socket->shared->data().watch_rearm.fetch_or(2);if(!send){const auto error=receive_error(*socket,errno);if(!error)return std::nullopt;fail(*error);}else fail(socket_error(errno));}
                        else {
                            record_network_transfer(!send && !(flags_host&MSG_PEEK)?static_cast<std::uint64_t>(size):0,send?static_cast<std::uint64_t>(size):0);
                            result=static_cast<std::uint64_t>(size);
                            if (!send) {memory.write(GuestAddress{arg(1)},std::span(bytes).first(static_cast<std::size_t>(size)));
                                if (addressed && arg(4)) {const auto address=address_bytes(reinterpret_cast<const sockaddr*>(&source));memory.write(GuestAddress{arg(4)},address);memory.store(GuestAddress{arg(5)},32,address.size());}
                                if(received_flags&MSG_TRUNC)fail(10040);}
                        }
                    }
                } else throw HostCallFault("尚未實作的 Winsock 服務");
            }
        }
    } catch (const MemoryFault&) {if (symbol=="WSAStartup") result=10014;else fail(10014);}
    catch (const MessageError& error) {fail(error.code);}
    catch (const std::bad_alloc&) {fail(symbol.starts_with("WSAStringToAddress")?8:10055);}
    catch (const std::invalid_argument&) {fail(10022);}
    catch (const std::system_error& error) {fail(socket_error(error.code().value()));}
    if(resolve_address || resolve_name){
        if(const auto pending=network.resolvers.find(threads_.current);pending!=network.resolvers.end()){
            pending->second->cancelled.store(true);network.resolvers.erase(pending);
        }
    }
    if((symbol=="select" || symbol=="recv" || symbol=="recvfrom" || symbol=="WSARecv" || symbol=="WSARecvFrom" || symbol=="WSARecvMsg") && threads_.items.at(threads_.current).wait.kind==ThreadWaitKind::SocketSelect)threads_.items.at(threads_.current).wait={};
    if(catalog_spi && catalog_error && arg(3)){try{memory.store(GuestAddress{arg(3)},32,catalog_error);}catch(const MemoryFault&) {}}
    arg.return_to(state,result);return std::nullopt;
}
}
