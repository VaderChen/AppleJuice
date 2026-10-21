#include "x64arm/network.hpp"
#include "socket_datagram_queue.hpp"
#include "socket_receive_options.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace x64arm {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(SharedSocketData)<=socket_detail::DatagramQueue::default_base);
static_assert(sizeof(SharedSocketData)<=socket_detail::receive_options_offset);
SharedSocketState::SharedSocketState(int inherited_descriptor):descriptor_(inherited_descriptor>=0?socket_detail::SocketStateAccess::duplicate_descriptor(inherited_descriptor):-1){
    const bool inherited=inherited_descriptor>=0;
    if(!inherited){char path[]="/tmp/macwin-socket-XXXXXX";descriptor_=::mkstemp(path);if(descriptor_>=0)(void)::unlink(path);}
    int error=descriptor_<0?errno:0;
    struct stat info{};
    if(!error && ::fcntl(descriptor_,F_SETFD,FD_CLOEXEC)<0)error=errno;
    if(!error && inherited){
        if(::fstat(descriptor_,&info)<0)error=errno;
        else if(!S_ISREG(info.st_mode) || info.st_size<0 || info.st_size<static_cast<off_t>(sizeof(SharedSocketData)))error=EINVAL;
        else if(info.st_size!=static_cast<off_t>(sizeof(SharedSocketData)) && !socket_detail::DatagramQueue::inspect_layout(descriptor_))error=errno;
    }else if(!error && ::ftruncate(descriptor_,sizeof(SharedSocketData))<0)error=errno;
    if(!error && !inherited && ::fstat(descriptor_,&info)<0)error=errno;
    if(!error)identity_={static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
    if(!error){void* mapped=::mmap(nullptr,sizeof(SharedSocketData),PROT_READ|PROT_WRITE,MAP_SHARED,descriptor_,0);
        if(mapped==MAP_FAILED)error=errno;else data_=static_cast<SharedSocketData*>(mapped);}
    if(!error){if(!inherited)::new(data_) SharedSocketData{};else if(data_->magic!=SharedSocketData{}.magic)error=EINVAL;}
    if(error){if(data_)(void)::munmap(data_,sizeof(SharedSocketData));if(descriptor_>=0)(void)socket_detail::SocketStateAccess::close_descriptor(descriptor_);throw std::system_error(error,std::generic_category(),"socket 共用狀態");}
}
SharedSocketState::~SharedSocketState(){if(data_)(void)::munmap(data_,sizeof(SharedSocketData));if(descriptor_>=0)(void)socket_detail::SocketStateAccess::close_descriptor(descriptor_);}
std::uint64_t socket_clock_ticks(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()/100);}
NativeSocket::NativeSocket(int fd,int af,int kind,int proto,std::shared_ptr<SharedSocketState> state):
    descriptor(fd),family(af),type(kind),protocol(proto),shared(state?std::move(state):std::make_shared<SharedSocketState>()),
    overlapped(shared->data().overlapped),accept_listener(shared->data().accept_listener),accepted_at(shared->data().accepted_at),
    accept_context(shared->data().accept_context),connect_context(shared->data().connect_context),listening(shared->data().listening),
    reusable(shared->data().reusable),send_shutdown(shared->data().send_shutdown){
    if((kind==SOCK_DGRAM || kind==SOCK_RAW) && socket_detail::multicast_receive_metadata(fd,af)<0)
        throw std::system_error(errno,std::generic_category(),"socket 內部封包資訊");
    if((kind==SOCK_DGRAM || kind==SOCK_RAW) && !socket_detail::DatagramQueue::inspect_layout(shared->descriptor())){
        socket_detail::DatagramQueue queue(shared->descriptor());if(!queue)throw std::system_error(errno,std::generic_category(),"socket 資料報佇列");
    }
}
NativeSocket::~NativeSocket(){if(descriptor>=0)::close(descriptor);}
std::vector<std::uint8_t> NativeSocket::bound_address()const{const auto& data=shared->data();const auto count=std::min<std::size_t>(data.bound_size,data.bound_address.size());return {data.bound_address.begin(),data.bound_address.begin()+static_cast<std::ptrdiff_t>(count)};}
void NativeSocket::set_bound_address(const std::vector<std::uint8_t>& bytes){auto& data=shared->data();if(bytes.size()>data.bound_address.size())throw std::invalid_argument("socket bind 位址過長");std::copy(bytes.begin(),bytes.end(),data.bound_address.begin());data.bound_size=static_cast<std::uint32_t>(bytes.size());}
}
