#pragma once
// 命名 section 傳遞真正的檔案描述符，保留 rename／unlink 後的 inode。
// 此服務僅等待 FD 請求；不掃描檔案、不執行背景清理。
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>

namespace x64arm::mapping_detail {
struct FileRecord {
    std::uint64_t signature{},device{},inode{};
    std::array<char16_t,1024> path{};
    std::array<std::array<char,32>,64> endpoints{};
};
constexpr std::uint64_t file_magic=UINT64_C(0x31464d4d52413458);
constexpr off_t file_record_offset=4096;
struct Fd {
    int value{-1};
    explicit Fd(int v=-1):value(v){}
    ~Fd(){if(value>=0)::close(value);}
    Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;
    int take(){return std::exchange(value,-1);}
};
inline bool address(const std::filesystem::path& path,sockaddr_un& out){
    const auto text=path.string();if(text.size()>=sizeof(out.sun_path))return false;
    out={};out.sun_family=AF_UNIX;std::memcpy(out.sun_path,text.c_str(),text.size()+1);return true;
}
inline void socket_options(int fd){
    (void)::fcntl(fd,F_SETFD,FD_CLOEXEC);
    const timeval timeout{1,0};(void)::setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    (void)::setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
#ifdef SO_NOSIGPIPE
    const int enabled=1;(void)::setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&enabled,sizeof(enabled));
#endif
}
struct Broker {
    Fd server,read_stop,write_stop;
    std::filesystem::path path;
    std::string name;
    std::thread worker;
    Broker(const std::filesystem::path& root,int file){
        static std::atomic<std::uint64_t> serial{};
        name="fd-"+std::to_string(::getpid())+"-"+std::to_string(++serial);path=root/name;
        sockaddr_un location{};
        if(name.size()>=32 || !address(path,location))throw std::runtime_error("section FD socket 路徑過長");
        server.value=::socket(AF_UNIX,SOCK_STREAM,0);if(server.value<0)throw std::runtime_error("section FD socket 建立失敗");socket_options(server.value);
        if(::bind(server.value,reinterpret_cast<sockaddr*>(&location),sizeof(location)) || ::listen(server.value,16))throw std::runtime_error("section FD socket 監聽失敗");
        int stop[2];if(::pipe(stop)){::unlink(path.c_str());throw std::runtime_error("section FD 停止通道建立失敗");}
        read_stop.value=stop[0];write_stop.value=stop[1];(void)::fcntl(stop[0],F_SETFD,FD_CLOEXEC);(void)::fcntl(stop[1],F_SETFD,FD_CLOEXEC);
        try{worker=std::thread([this,file]{
            std::array<pollfd,2> descriptors{{{server.value,POLLIN,0},{read_stop.value,POLLIN,0}}};
            for(;;){
                if(::poll(descriptors.data(),descriptors.size(),-1)<0){if(errno==EINTR)continue;return;}
                if(descriptors[1].revents)return;if(!(descriptors[0].revents&POLLIN))return;
                Fd client{::accept(server.value,nullptr,nullptr)};if(client.value<0)continue;socket_options(client.value);
#if defined(__APPLE__)
                uid_t uid{};gid_t gid{};if(::getpeereid(client.value,&uid,&gid) || uid!=::geteuid())continue;
#elif defined(__linux__)
                ucred peer{};socklen_t length=sizeof(peer);if(::getsockopt(client.value,SOL_SOCKET,SO_PEERCRED,&peer,&length) || peer.uid!=::geteuid())continue;
#endif
                char marker='F';iovec io{&marker,1};alignas(cmsghdr) std::array<char,CMSG_SPACE(sizeof(int))> control{};
                msghdr message{};message.msg_iov=&io;message.msg_iovlen=1;message.msg_control=control.data();message.msg_controllen=control.size();
                auto* header=CMSG_FIRSTHDR(&message);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;header->cmsg_len=CMSG_LEN(sizeof(int));std::memcpy(CMSG_DATA(header),&file,sizeof(file));
#ifdef MSG_NOSIGNAL
                constexpr int flags=MSG_NOSIGNAL;
#else
                constexpr int flags=0;
#endif
                while(::sendmsg(client.value,&message,flags)<0 && errno==EINTR){}
            }
        });}catch(...){::unlink(path.c_str());throw;}
    }
    ~Broker(){if(worker.joinable()){const char stop=1;(void)::write(write_stop.value,&stop,1);worker.join();}::unlink(path.c_str());}
};
inline int receive(const std::filesystem::path& root,const FileRecord& record){
    for(const auto& endpoint:record.endpoints){
        const auto end=std::find(endpoint.begin(),endpoint.end(),'\0');
        if(end==endpoint.begin() || end==endpoint.end())continue;
        const std::string name(endpoint.begin(),end);if(!name.starts_with("fd-") || name.find('/')!=name.npos)continue;
        sockaddr_un location{};if(!address(root/name,location))continue;
        Fd socket{::socket(AF_UNIX,SOCK_STREAM,0)};if(socket.value<0)continue;socket_options(socket.value);
        if(::connect(socket.value,reinterpret_cast<sockaddr*>(&location),sizeof(location)))continue;
        char marker{};iovec io{&marker,1};alignas(cmsghdr) std::array<char,CMSG_SPACE(sizeof(int))> control{};
        msghdr message{};message.msg_iov=&io;message.msg_iovlen=1;message.msg_control=control.data();message.msg_controllen=control.size();
        ssize_t count;do{count=::recvmsg(socket.value,&message,0);}while(count<0 && errno==EINTR);
        Fd received;
        for(auto* h=CMSG_FIRSTHDR(&message);h;h=CMSG_NXTHDR(&message,h))if(h->cmsg_level==SOL_SOCKET && h->cmsg_type==SCM_RIGHTS && h->cmsg_len>=CMSG_LEN(sizeof(int))){int fd;std::memcpy(&fd,CMSG_DATA(h),sizeof(fd));if(received.value>=0)::close(fd);else received.value=fd;}
        if(count!=1 || marker!='F' || (message.msg_flags&MSG_CTRUNC) || received.value<0)continue;
        struct stat info{};if(::fstat(received.value,&info) || !S_ISREG(info.st_mode) || std::uint64_t(info.st_dev)!=record.device || std::uint64_t(info.st_ino)!=record.inode)continue;
        (void)::fcntl(received.value,F_SETFD,FD_CLOEXEC);return received.take();
    }
    return -1;
}
}
