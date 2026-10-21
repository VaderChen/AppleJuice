#include "x64arm/windows_services.hpp"
#include "socket_address.hpp"
#include "socket_ingress.hpp"
#include "socket_poll_groups.hpp"
#include <bit>
#include <algorithm>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#ifdef __APPLE__
#include <sys/event.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace x64arm {
std::optional<std::uint32_t> WindowsServices::invoke_socket_poll(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);auto& network=runtime_.network;
    const auto finish=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto failed=[&](std::uint32_t error){last_error_=error;if(service.symbol=="WSAPoll"){auto& wait=threads_.items.at(threads_.current).wait;if(wait.kind==ThreadWaitKind::SocketSelect)wait={};}return finish(UINT32_MAX);};
    if(!network.startups)return failed(10093);
    try{
        if(service.symbol=="WSASendDisconnect"){
            const auto found=network.sockets.find(arg(0));if(found==network.sockets.end())return failed(10038);
            if(arg(1)){memory.check(GuestAddress{arg(1)},x86_?8:16,Access::Read);return failed(10042);}
            if(::shutdown(found->second->descriptor,SHUT_WR)<0)return failed(socket_detail::socket_error(errno));found->second->send_shutdown=true;return finish(0);
        }
        constexpr unsigned read_normal=0x100,read_band=0x200,write_normal=0x10,error_bit=1,hangup=2,invalid=4;
        const auto count=static_cast<std::uint32_t>(arg(1));const unsigned unit=x86_?4:8,entry_size=x86_?8:16;const auto bits=static_cast<std::uint8_t>(unit*8);
        if(!count)return failed(10022);if(count>1024)return failed(10055);if(!arg(0))return failed(10014);memory.check(GuestAddress{arg(0)},static_cast<std::uint64_t>(count)*entry_size,Access::Read);memory.check(GuestAddress{arg(0)},static_cast<std::uint64_t>(count)*entry_size,Access::Write);
        std::vector<pollfd> native;std::vector<std::uint64_t> handles;std::vector<unsigned> requested;unsigned valid=0;
        native.reserve(count);handles.reserve(count);requested.reserve(count);
        for(unsigned i=0;i<count;++i){const auto at=arg(0)+i*entry_size;const auto handle=memory.load(GuestAddress{at},bits);const auto events=static_cast<unsigned>(memory.load(GuestAddress{at+unit},16));if(events&~(read_normal|read_band|write_normal))return failed(10022);
            const auto found=network.sockets.find(handle);if(found==network.sockets.end()){native.push_back({-1,0,0});handles.push_back(handle);}
            else{++valid;native.push_back({found->second->descriptor,static_cast<short>(((events&read_normal)?POLLIN:0)|((events&read_band)?POLLPRI:0)|((events&write_normal)?POLLOUT:0)),0});handles.push_back(handle);}
            requested.push_back(events);
        }
        if(!valid)return failed(10022);
        // Darwin poll 以 fd 合併重複項目；Windows 要回填及計算每個陣列項目。
        auto groups=socket_detail::group_poll_descriptors(native,handles);
        auto& queried=groups.queried;auto& indices=groups.indices;auto& waiting_handles=groups.handles;
        const auto status=::poll(queried.data(),static_cast<nfds_t>(queried.size()),0);if(status<0 && errno!=EINTR)return failed(socket_detail::socket_error(errno));
        bool deferred=false;for(std::size_t i=0;i<queried.size();++i)deferred=socket_detail::socket_readiness(*network.sockets.at(waiting_handles[i]),queried[i]) || deferred;
#ifdef __APPLE__
        for(auto& entry:queried)if(entry.events&POLLPRI){const auto queue=::kqueue();if(queue<0)return failed(socket_detail::socket_error(errno));(void)::fcntl(queue,F_SETFD,FD_CLOEXEC);struct kevent change{},event{};EV_SET(&change,static_cast<std::uintptr_t>(entry.fd),EVFILT_EXCEPT,EV_ADD|EV_ENABLE|EV_ONESHOT,NOTE_OOB,0,nullptr);const timespec immediate{};const auto ready=::kevent(queue,&change,1,&event,1,&immediate);::close(queue);if(ready>0 && !(event.flags&EV_ERROR) && (event.fflags&NOTE_OOB))entry.revents|=POLLPRI;}
#endif
        unsigned result=0;std::vector<unsigned> returned;returned.reserve(count);
        for(unsigned i=0;i<count;++i){const auto flags=indices[i]==SIZE_MAX?0:queried[indices[i]].revents;unsigned events=indices[i]==SIZE_MAX?invalid:0;
            if(flags&POLLNVAL)events|=invalid;if(flags&POLLERR)events|=error_bit;if(flags&POLLHUP)events|=hangup;
            if(flags&POLLIN)events|=requested[i]&read_normal;if(flags&POLLOUT)events|=requested[i]&write_normal;if(flags&POLLPRI)events|=requested[i]&read_band;
            if(events)++result;returned.push_back(events);
        }
        auto& thread=threads_.items.at(threads_.current);const auto timeout=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2)));
        if(deferred){threads_.yield=true;schedule(state,memory);return std::nullopt;}
        if(!result && timeout && !(thread.wait.kind==ThreadWaitKind::SocketSelect && thread.wait.deadline<=runtime_.clock_ticks)){
            const auto deadline=thread.wait.kind==ThreadWaitKind::SocketSelect?thread.wait.deadline:timeout<0?UINT64_MAX:runtime_.clock_ticks+static_cast<std::uint64_t>(timeout)*10000;
            thread.wait={};thread.wait.kind=ThreadWaitKind::SocketSelect;thread.wait.deadline=deadline;thread.wait.handles=std::move(waiting_handles);thread.wait.alertable=true;for(const auto& entry:queried)thread.wait.socket_events.push_back(static_cast<std::uint32_t>(entry.events));thread.status=ThreadStatus::Waiting;schedule(state,memory);return std::nullopt;
        }
        thread.wait={};for(unsigned i=0;i<count;++i)memory.store(GuestAddress{arg(0)+i*entry_size+unit+2},16,returned[i]);return finish(result);
    }catch(const MemoryFault&){return failed(10014);}catch(const std::bad_alloc&){return failed(10055);}
}
}
