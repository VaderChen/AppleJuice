#include "x64arm/debug_transport.hpp"
#include "x64arm/process_debugger.hpp"
#include "fast_poll.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/event.h>
#endif

namespace x64arm::debugger {
namespace {
[[noreturn]] void fail(int error){throw std::system_error(error,std::generic_category(),"MacWin 偵錯通道");}
void require(bool condition,int error){if(!condition)fail(error);}
void checked(int result){if(result<0)fail(errno);}
std::uint64_t number(std::span<const std::uint8_t> bytes,std::size_t at,unsigned size){
    std::uint64_t result=0;for(unsigned i=0;i<size;++i)result|=static_cast<std::uint64_t>(bytes[at+i])<<(8*i);return result;
}
bool valid(const Frame& frame){return frame.kind && frame.kind<=65535 && frame.sequence && frame.flags<=2 && frame.payload.size()<=wire_payload_limit;}
sockaddr_un address(const std::filesystem::path& path){
    const auto name=path.string();sockaddr_un result{};require(!name.empty() && name.size()<sizeof(result.sun_path),ENAMETOOLONG);
    result.sun_family=AF_UNIX;std::memcpy(result.sun_path,name.c_str(),name.size()+1);
#ifdef __APPLE__
    result.sun_len=static_cast<std::uint8_t>(sizeof(result));
#endif
    return result;
}
void configure(int fd){
    const auto flags=::fcntl(fd,F_GETFL);checked(flags);checked(::fcntl(fd,F_SETFL,flags|O_NONBLOCK));
    checked(::fcntl(fd,F_SETFD,FD_CLOEXEC));
#ifdef SO_NOSIGPIPE
    const int enabled=1;checked(::setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled)));
#endif
}
struct Descriptor {
    int value=-1;explicit Descriptor(int fd):value(fd){}~Descriptor(){if(value>=0)::close(value);}
    Descriptor(const Descriptor&)=delete;Descriptor& operator=(const Descriptor&)=delete;
    int release(){const auto result=value;value=-1;return result;}
};
Peer authenticate(int fd,std::uint64_t connection){
#ifdef __APPLE__
    uid_t uid{};gid_t gid{};checked(::getpeereid(fd,&uid,&gid));require(uid==::geteuid(),EACCES);
    pid_t native_pid{};socklen_t size=sizeof(native_pid);
    checked(::getsockopt(fd,SOL_LOCAL,LOCAL_PEERPID,&native_pid,&size));
    require(size==sizeof(native_pid) && native_pid>0,EPROTO);
    const auto pid=static_cast<std::uint32_t>(native_pid);const auto identity=process_debugger::query(pid);
    require(!identity.error && !identity.exited && identity.identity,ESRCH);return {connection,identity.identity,pid};
#else
    (void)fd;(void)connection;fail(EOPNOTSUPP);
#endif
}
}
std::vector<std::uint8_t> encode_frame(const Frame& frame){
    require(valid(frame),frame.payload.size()>wire_payload_limit?EMSGSIZE:EINVAL);
    std::vector<std::uint8_t> result(wire_header+frame.payload.size());
    const auto put=[&](std::size_t at,unsigned size,std::uint64_t value){for(unsigned i=0;i<size;++i)result[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
    put(0,8,wire_magic);put(8,4,frame.payload.size());put(12,4,frame.kind);put(16,8,frame.sequence);put(24,4,frame.error);put(28,4,frame.flags);
    std::copy(frame.payload.begin(),frame.payload.end(),result.begin()+static_cast<std::ptrdiff_t>(wire_header));return result;
}
struct Transport::Impl {
    struct Connection {
        int fd=-1;Peer peer;std::vector<std::uint8_t> input;std::deque<std::vector<std::uint8_t>> output;std::size_t offset{},queued{};
        // 對端出生識別的 sysctl 查詢約 8 µs；連線有事件時立即複查，空閒時最多每 50 ms 一次。
        std::chrono::steady_clock::time_point next_identity{};
    };
    std::filesystem::path root,endpoint;std::uint32_t own_pid{};std::uint64_t own_birth{},next=1;
    int listener=-1;dev_t device{};ino_t inode{};bool owns_endpoint{};
    int multiplexer=-1;
    std::map<int,short> interests;
    std::map<std::uint64_t,Connection> connections;std::deque<TransportEvent> events;
    explicit Impl(const std::filesystem::path& directory):root(directory),own_pid(static_cast<std::uint32_t>(::getpid())){
        struct stat status{};checked(::lstat(root.c_str(),&status));
        require(S_ISDIR(status.st_mode) && status.st_uid==::geteuid() && (status.st_mode&0777)==0700,EACCES);
        const auto identity=process_debugger::query(own_pid);require(!identity.error && !identity.exited && identity.identity,ESRCH);own_birth=identity.identity;
        Descriptor notification(-1);
#if defined(__APPLE__)
        notification.value=::kqueue();require(notification.value>=0,errno);
        checked(::fcntl(notification.value,F_SETFD,FD_CLOEXEC));
#endif
        endpoint=root/("debug-"+std::to_string(own_pid));const auto location=address(endpoint);
        Descriptor fd(::socket(AF_UNIX,SOCK_STREAM,0));require(fd.value>=0,errno);configure(fd.value);
        checked(::bind(fd.value,reinterpret_cast<const sockaddr*>(&location),sizeof(location)));
        // bind 成功後才取得端點所有權；失敗不能刪除另一個工作中的端點。
        if(::lstat(endpoint.c_str(),&status)!=0){const int error=errno;::unlink(endpoint.c_str());fail(error);}
        device=status.st_dev;inode=status.st_ino;owns_endpoint=true;
        if(::chmod(endpoint.c_str(),0600)!=0 || ::listen(fd.value,64)!=0){const int error=errno;remove_endpoint();fail(error);}
        listener=fd.release();multiplexer=notification.release();
    }
    void remove_endpoint()noexcept{
        struct stat status{};if(owns_endpoint && ::lstat(endpoint.c_str(),&status)==0 && status.st_dev==device && status.st_ino==inode && S_ISSOCK(status.st_mode))::unlink(endpoint.c_str());owns_endpoint=false;
    }
    ~Impl(){for(auto& [id,connection]:connections){(void)id;::close(connection.fd);}if(listener>=0)::close(listener);if(multiplexer>=0)::close(multiplexer);if(own_pid==static_cast<std::uint32_t>(::getpid()))remove_endpoint();}
    int wait(std::span<pollfd> descriptors,int milliseconds){
#if defined(__APPLE__)
        if(milliseconds==0){
            // 空輪詢的零逾時查詢：select 約 0.2 µs，kevent 約 13 µs（見 fast_poll.hpp）。
            // select 不回報 POLLHUP；對端關閉時 fd 仍為可讀，read() 以 recv()==0 斷線，行為與 poll 相同。
            fd_set readable,writable;FD_ZERO(&readable);FD_ZERO(&writable);int highest=-1;bool usable=true;
            for(const auto& descriptor:descriptors){
                if(descriptor.fd<0 || descriptor.fd>=FD_SETSIZE){usable=false;break;}
                if(descriptor.events&POLLIN)FD_SET(descriptor.fd,&readable);
                if(descriptor.events&POLLOUT)FD_SET(descriptor.fd,&writable);
                highest=std::max(highest,descriptor.fd);
            }
            if(usable){
                timeval zero{};
                const int selected=::select(highest+1,&readable,&writable,nullptr,&zero);
                if(selected<0)return selected;
                int ready=0;
                for(auto& descriptor:descriptors){
                    if((descriptor.events&POLLIN) && FD_ISSET(descriptor.fd,&readable))descriptor.revents|=POLLIN;
                    if((descriptor.events&POLLOUT) && FD_ISSET(descriptor.fd,&writable))descriptor.revents|=POLLOUT;
                    if(descriptor.revents)++ready;
                }
                return ready;
            }
        }
        // 保留核心事件集合；空閒查詢不反覆建立 poll 的暫時訂閱。
        // 使用 level-trigger，佇列滿而暫停讀取時仍會在下一輪收到可讀事件。
        std::array<struct kevent,130> changes,ready_events; // 只讀取已填入的 count／ready 筆，不需每次清零 8 KB
        unsigned count=0;
        for(const auto& descriptor:descriptors){
            auto& prior=interests[descriptor.fd];
            for(const auto [bit,filter]:std::array{std::pair{POLLIN,EVFILT_READ},std::pair{POLLOUT,EVFILT_WRITE}})
                if((prior&bit)!=(descriptor.events&bit)){
                    EV_SET(&changes[count],descriptor.fd,filter,(descriptor.events&bit)?EV_ADD:EV_DELETE,0,0,nullptr);++count;
                }
            prior=descriptor.events;
        }
        const timespec timeout{milliseconds/1000,(milliseconds%1000)*1000000L};
        const int ready=::kevent(multiplexer,changes.data(),static_cast<int>(count),ready_events.data(),static_cast<int>(ready_events.size()),&timeout);
        if(ready<0)return ready;
        for(int i=0;i<ready;++i){
            const auto& event=ready_events[static_cast<std::size_t>(i)];
            if(event.flags&EV_ERROR){if(event.data)fail(static_cast<int>(event.data));continue;}
            for(auto& descriptor:descriptors)if(event.ident==static_cast<std::uintptr_t>(descriptor.fd)){
                descriptor.revents|=event.filter==EVFILT_READ?POLLIN:POLLOUT;
                if(event.flags&EV_EOF)descriptor.revents|=POLLHUP;
                break;
            }
        }
        return ready;
#else
        return ::poll(descriptors.data(),static_cast<nfds_t>(descriptors.size()),milliseconds);
#endif
    }
    void check()const{require(own_pid==static_cast<std::uint32_t>(::getpid()),ECHILD);}
    Connection& find(const Peer& peer){const auto found=connections.find(peer.connection);require(found!=connections.end() && found->second.peer==peer,ENOENT);return found->second;}
    void disconnect(std::uint64_t id,int error){
        const auto found=connections.find(id);if(found==connections.end())return;
        events.push_back({TransportEvent::Kind::Disconnected,found->second.peer,{},error});interests.erase(found->second.fd);::close(found->second.fd);connections.erase(found);
    }
    Peer add(Descriptor& fd,Peer peer){
        require(connections.size()<64 && events.size()<128 && next!=UINT64_MAX,ENOBUFS);peer.connection=next;
        events.push_back({TransportEvent::Kind::Connected,peer,{},0});
        try{connections.emplace(next,Connection{fd.value,peer,{},{},0,0});}catch(...){events.pop_back();throw;}
        fd.release();++next;return peer;
    }
    bool parse(Connection& connection){
        while(connection.input.size()>=wire_header && events.size()<128){
            const std::span<const std::uint8_t> input(connection.input);const auto length=number(input,8,4);
            if(number(input,0,8)!=wire_magic || !number(input,12,4) || number(input,12,4)>65535 || !number(input,16,8) || number(input,28,4)>2){disconnect(connection.peer.connection,EPROTO);return false;}
            if(length>wire_payload_limit){disconnect(connection.peer.connection,EMSGSIZE);return false;}
            const auto total=wire_header+static_cast<std::size_t>(length);if(input.size()<total)return true;
            Frame frame{static_cast<std::uint32_t>(number(input,12,4)),static_cast<std::uint32_t>(number(input,24,4)),static_cast<std::uint32_t>(number(input,28,4)),number(input,16,8),{}};
            frame.payload.assign(input.begin()+static_cast<std::ptrdiff_t>(wire_header),input.begin()+static_cast<std::ptrdiff_t>(total));
            events.push_back({TransportEvent::Kind::Message,connection.peer,std::move(frame),0});
            connection.input.erase(connection.input.begin(),connection.input.begin()+static_cast<std::ptrdiff_t>(total));
        }return true;
    }
    void flush(Connection& connection){
        while(!connection.output.empty()){
            const auto& bytes=connection.output.front();
#ifdef MSG_NOSIGNAL
            constexpr int flags=MSG_NOSIGNAL;
#else
            constexpr int flags=0;
#endif
            const auto count=::send(connection.fd,bytes.data()+connection.offset,bytes.size()-connection.offset,flags);
            if(count<0){if(errno==EINTR)continue;if(errno==EAGAIN || errno==EWOULDBLOCK)return;disconnect(connection.peer.connection,errno);return;}
            if(!count){disconnect(connection.peer.connection,EPIPE);return;}
            const auto written=static_cast<std::size_t>(count);connection.offset+=written;connection.queued-=written;
            if(connection.offset==bytes.size()){connection.output.pop_front();connection.offset=0;}
        }
    }
    void read(Connection& connection){
        std::array<std::uint8_t,16384> bytes{};
        while(events.size()<128){
            const auto count=::recv(connection.fd,bytes.data(),bytes.size(),0);
            if(count<0){if(errno==EINTR)continue;if(errno==EAGAIN || errno==EWOULDBLOCK)return;disconnect(connection.peer.connection,errno);return;}
            if(!count){disconnect(connection.peer.connection,connection.input.empty()?0:EPROTO);return;}
            const auto received=static_cast<std::size_t>(count);
            if(connection.input.size()+received>wire_payload_limit+wire_header+bytes.size()){disconnect(connection.peer.connection,EMSGSIZE);return;}
            connection.input.insert(connection.input.end(),bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(received));
            if(!parse(connection))return;
        }
    }
};
Transport::Transport(const std::filesystem::path& directory):impl_(std::make_unique<Impl>(directory)){}
Transport::~Transport()=default;Transport::Transport(Transport&&)noexcept=default;Transport& Transport::operator=(Transport&&)noexcept=default;
Peer Transport::connect(std::uint32_t pid,std::uint64_t birth){
    require(impl_!=nullptr,EINVAL);impl_->check();require(pid && birth && pid!=impl_->own_pid,EINVAL);require(impl_->connections.size()<64,ENOBUFS);
    const auto identity=process_debugger::query(pid);require(!identity.error && !identity.exited && identity.identity==birth,ESRCH);
    const auto location=address(impl_->root/("debug-"+std::to_string(pid)));
    Descriptor fd(::socket(AF_UNIX,SOCK_STREAM,0));require(fd.value>=0,errno);configure(fd.value);
    if(::connect(fd.value,reinterpret_cast<const sockaddr*>(&location),sizeof(location))!=0){
        require(errno==EINPROGRESS || errno==EAGAIN,errno);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        for(;;){const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();require(left>0,ETIMEDOUT);
            pollfd item{fd.value,POLLOUT,0};const int result=::poll(&item,1,static_cast<int>(left));if(result<0 && errno==EINTR)continue;require(result>=0,errno);require(result>0,ETIMEDOUT);
            int error{};socklen_t size=sizeof(error);checked(::getsockopt(fd.value,SOL_SOCKET,SO_ERROR,&error,&size));require(!error,error);break;
        }
    }
    const auto peer=authenticate(fd.value,impl_->next);require(peer.pid==pid && peer.birth==birth,ESRCH);return impl_->add(fd,peer);
}
void Transport::send(const Peer& peer,const Frame& frame){
    require(impl_!=nullptr,EINVAL);impl_->check();auto bytes=encode_frame(frame);auto& connection=impl_->find(peer);
    require(bytes.size()<=wire_queue_limit-connection.queued,ENOBUFS);connection.output.push_back(std::move(bytes));connection.queued+=connection.output.back().size();
}
void Transport::close(const Peer& peer){require(impl_!=nullptr,EINVAL);impl_->check();(void)impl_->find(peer);impl_->disconnect(peer.connection,0);}
std::vector<TransportEvent> Transport::poll(unsigned milliseconds){
    require(impl_!=nullptr && milliseconds<=static_cast<unsigned>(INT_MAX),EINVAL);impl_->check();
    std::vector<pollfd> descriptors{{impl_->listener,POLLIN,0}};std::vector<std::uint64_t> ids{0};
    for(const auto& [id,connection]:impl_->connections){ids.push_back(id);descriptors.push_back({connection.fd,static_cast<short>(POLLIN|(connection.output.empty()?0:POLLOUT)),0});}
    const int ready=impl_->wait(descriptors,impl_->events.empty()?static_cast<int>(milliseconds):0);
    if(ready<0 && errno!=EINTR)fail(errno);
    if(ready>=0 && (descriptors[0].revents&POLLIN)){
        for(unsigned accepted=0;accepted<64 && impl_->connections.size()<64 && impl_->events.size()<128;++accepted){
            Descriptor fd(::accept(impl_->listener,nullptr,nullptr));if(fd.value<0){if(errno==EINTR)continue;if(errno==EAGAIN || errno==EWOULDBLOCK)break;fail(errno);}
            try{configure(fd.value);impl_->add(fd,authenticate(fd.value,impl_->next));}catch(const std::system_error&){/* 尚未建立的連線不能產生可信任的 guest 事件。 */}
        }
    }
    const auto identity_now=std::chrono::steady_clock::now();
    for(std::size_t i=1;i<ids.size();++i){
        auto found=impl_->connections.find(ids[i]);if(found==impl_->connections.end())continue;
        // 對端結束時核心會關閉其 socket 而使本端出現 POLLHUP／POLLERR；有任何事件的連線仍立即核對身分，
        // 沒有事件的連線改為最多每 50 ms 核對一次，避免每次空輪詢都為每條連線呼叫 sysctl。
        if(descriptors[i].revents || identity_now>=found->second.next_identity){
            const auto identity=process_debugger::query(found->second.peer.pid);
            if(identity.error || identity.exited || identity.identity!=found->second.peer.birth){impl_->disconnect(ids[i],ESRCH);continue;}
            found->second.next_identity=identity_now+std::chrono::milliseconds(50);
        }
        if(!impl_->parse(found->second))continue;
        if(descriptors[i].revents&(POLLIN|POLLHUP))impl_->read(found->second);
        found=impl_->connections.find(ids[i]);if(found==impl_->connections.end())continue;
        if(descriptors[i].revents&POLLOUT)impl_->flush(found->second);
        found=impl_->connections.find(ids[i]);if(found!=impl_->connections.end() && (descriptors[i].revents&(POLLERR|POLLNVAL)))impl_->disconnect(ids[i],EIO);
    }
    std::vector<TransportEvent> result;result.reserve(std::min<std::size_t>(64,impl_->events.size()));
    while(result.size()<64 && !impl_->events.empty()){result.push_back(std::move(impl_->events.front()));impl_->events.pop_front();}return result;
}
std::filesystem::path Transport::path()const{require(impl_!=nullptr,EINVAL);impl_->check();return impl_->endpoint;}
bool Transport::listener_only()const noexcept{return impl_ && impl_->connections.empty() && impl_->events.empty();}
int Transport::listener_descriptor()const noexcept{return impl_?impl_->listener:-1;}
std::uint32_t Transport::pid()const{require(impl_!=nullptr,EINVAL);impl_->check();return impl_->own_pid;}
std::uint64_t Transport::birth()const{require(impl_!=nullptr,EINVAL);impl_->check();return impl_->own_birth;}
}
