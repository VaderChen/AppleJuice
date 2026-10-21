#pragma once
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <sys/stat.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace x64arm::socket_detail {
// 同 inode 的任一 close 都會釋放本程序的 POSIX lock。狀態 FD 收尾共用
// 此 gate；同執行緒的 close 延至最外層操作結束，其他執行緒等待操作離開。
class SocketStateAccess {
    struct Lock {dev_t device;ino_t inode;int fd;unsigned users;};
    struct State {std::recursive_mutex mutex;std::vector<Lock> locks;std::vector<int> closes;unsigned depth{};};
    static State& state(){static State value;return value;}
    std::unique_lock<std::recursive_mutex> local_{state().mutex,std::try_to_lock};
    dev_t device_{};ino_t inode_{};bool joined_{},locked_{};
public:
    explicit SocketStateAccess(int fd){
        if(!local_.owns_lock()){errno=EINPROGRESS;return;}
        auto& shared=state();if(shared.depth==UINT32_MAX){errno=EOVERFLOW;return;}++shared.depth;joined_=true;
        if(std::find(shared.closes.begin(),shared.closes.end(),fd)!=shared.closes.end()){errno=EBADF;return;}
        struct stat info{};if(fd<0 || ::fstat(fd,&info)<0){if(fd<0)errno=EBADF;return;}
        if(!S_ISREG(info.st_mode)){errno=EINVAL;return;}device_=info.st_dev;inode_=info.st_ino;
        auto found=std::find_if(shared.locks.begin(),shared.locks.end(),[&](const auto& item){return item.device==device_ && item.inode==inode_;});
        if(found!=shared.locks.end()){if(found->users==UINT32_MAX){errno=EOVERFLOW;return;}++found->users;locked_=true;return;}
        try{shared.locks.push_back({device_,inode_,fd,1});}catch(const std::bad_alloc&){errno=ENOMEM;return;}
        struct flock lock{};lock.l_type=F_WRLCK;lock.l_whence=SEEK_SET;lock.l_start=0;lock.l_len=1;
        if(::fcntl(fd,F_SETLK,&lock)<0){shared.locks.pop_back();if(errno==EACCES || errno==EAGAIN)errno=EINPROGRESS;return;}
        locked_=true;
    }
    ~SocketStateAccess(){
        if(!joined_)return;const auto saved=errno;auto& shared=state();
        if(locked_){auto found=std::find_if(shared.locks.begin(),shared.locks.end(),[&](const auto& item){return item.device==device_ && item.inode==inode_;});
            if(found!=shared.locks.end() && !--found->users){struct flock lock{};lock.l_type=F_UNLCK;lock.l_whence=SEEK_SET;lock.l_start=0;lock.l_len=1;(void)::fcntl(found->fd,F_SETLK,&lock);shared.locks.erase(found);}}
        if(!--shared.depth){for(const auto fd:shared.closes)(void)::close(fd);shared.closes.clear();}
        errno=saved;
    }
    SocketStateAccess(const SocketStateAccess&)=delete;
    SocketStateAccess& operator=(const SocketStateAccess&)=delete;
    explicit operator bool()const{return locked_;}
    static int duplicate_descriptor(int fd) noexcept {
        try{auto& shared=state();std::lock_guard local(shared.mutex);
            if(std::find(shared.closes.begin(),shared.closes.end(),fd)!=shared.closes.end()){errno=EBADF;return -1;}
            return ::dup(fd);
        }catch(const std::system_error&){errno=EDEADLK;return -1;}
    }
    static ssize_t receive_rights(int fd,msghdr& message,int flags) noexcept {
        try{auto& shared=state();std::unique_lock local(shared.mutex,std::try_to_lock);
            if(!local.owns_lock() || shared.depth){errno=EINPROGRESS;return -1;}
            if(!message.msg_control && message.msg_controllen){errno=EFAULT;return -1;}
            // Darwin 先 externalize rights 再 copyout；小 control buffer 可能留下
            // 未交付的 FD。原生上限為 512 FD，先以有界完整 buffer 接收再交付。
            alignas(cmsghdr) std::array<std::uint8_t,8192> control{};
            auto received=message;received.msg_control=control.data();received.msg_controllen=control.size();
            const auto count=::recvmsg(fd,&received,flags);if(count<0)return count;const auto saved=errno;
            const bool fits=received.msg_controllen<=message.msg_controllen && !(received.msg_flags&MSG_CTRUNC);
            message.msg_namelen=received.msg_namelen;message.msg_flags=received.msg_flags;
            if(fits){if(received.msg_controllen)std::memcpy(message.msg_control,control.data(),received.msg_controllen);message.msg_controllen=received.msg_controllen;}
            else{
                // 交付不完整 rights 會讓 parser 誤讀長度；整份丟棄並收尾所有 FD。
                for(auto* header=CMSG_FIRSTHDR(&received);header;header=CMSG_NXTHDR(&received,header)){
                    const auto offset=reinterpret_cast<std::uint8_t*>(header)-control.data();
                    if(header->cmsg_len<CMSG_LEN(0) || std::size_t(offset)+header->cmsg_len>received.msg_controllen)break;
                    if(header->cmsg_level!=SOL_SOCKET || header->cmsg_type!=SCM_RIGHTS)continue;
                    const auto size=header->cmsg_len-CMSG_LEN(0);
                    for(std::size_t i=0;i+sizeof(int)<=size;i+=sizeof(int)){int descriptor=-1;std::memcpy(&descriptor,CMSG_DATA(header)+i,sizeof descriptor);(void)::close(descriptor);}
                }
                message.msg_controllen=0;message.msg_flags|=MSG_CTRUNC;
            }
            errno=saved;return count;
        }catch(const std::system_error&){errno=EDEADLK;return -1;}
    }
    static int close_descriptor(int fd) noexcept {
        try{
            auto& shared=state();std::lock_guard local(shared.mutex);
            if(!shared.depth)return ::close(fd);
            if(std::find(shared.closes.begin(),shared.closes.end(),fd)!=shared.closes.end()){errno=EBADF;return -1;}
            struct stat info{};if(::fstat(fd,&info)<0)return -1;
            const bool held=std::any_of(shared.locks.begin(),shared.locks.end(),[&](const auto& item){return item.device==info.st_dev && item.inode==info.st_ino;});
            if(!held)return ::close(fd);
            shared.closes.push_back(fd);return 0;
        }catch(const std::bad_alloc&){errno=ENOMEM;return -1;}
        catch(const std::system_error&){errno=EDEADLK;return -1;}
    }
};
} // namespace x64arm::socket_detail
