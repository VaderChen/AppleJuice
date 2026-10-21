#pragma once
#include "x64arm/com_rpc_wire.hpp"
#include "x64arm/process_identity.hpp"
#include <cerrno>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm::com_rpc {
struct TransportError{std::uint32_t code;};
// 純 transport：directory 由 Windows sandbox 的已釘選系統目錄取得。
// 不解參考客體位址、不做 COM 方法派送；Windows 身分與 capability 仍須由 driver 核對。
class Mailbox {
    static constexpr std::array<std::uint8_t,8> magic{'W','6','4','B','O','X','0','1'};
    static constexpr std::size_t header=64,entry_size=24,maximum_entries=64,maximum_bytes=64U*1024U*1024U;
    struct Item{std::uint64_t sequence{},size{},checksum{};};
    struct Queue{std::uint64_t next{1};std::vector<Item> entries;bool stopped{};};
    int parent_{-1},directory_{-1},lease_{-1},lock_{-1};Endpoint endpoint_;bool owner_{};
    std::uint32_t native_pid_{};std::string name_;
    std::function<void(int,bool,bool)> inspect_;
    struct Lock{
        int fd;
        explicit Lock(int value):fd(value){
#if !defined(_WIN32)
            while(::flock(fd,LOCK_EX)){if(errno!=EINTR)throw TransportError{5};}
#else
            throw TransportError{50};
#endif
        }
        ~Lock(){
#if !defined(_WIN32)
            (void)::flock(fd,LOCK_UN);
#endif
        }
    };
    struct File {
        int fd;
        explicit File(int value):fd(value){}
        ~File(){
#if !defined(_WIN32)
            if(fd>=0)::close(fd);
#endif
        }
    };
    static void require(bool value,std::uint32_t error=13){if(!value)throw TransportError{error};}
    static std::string name(const Endpoint& endpoint){constexpr char hex[]="0123456789abcdef";std::string value="com-rpc-";for(auto byte:endpoint.nonce){value+=hex[byte>>4];value+=hex[byte&15];}return value;}
    static std::string name(std::uint64_t sequence){char value[32]{};std::snprintf(value,sizeof(value),"message-%016llx",static_cast<unsigned long long>(sequence));return value;}
    static void validate(int fd,bool directory=false){
#if !defined(_WIN32)
        struct stat info{};require(!::fstat(fd,&info) && (directory?S_ISDIR(info.st_mode):S_ISREG(info.st_mode)) && info.st_uid==::geteuid() && !(info.st_mode&077) && (directory || info.st_nlink==1),5);
#else
        (void)fd;(void)directory;throw TransportError{50};
#endif
    }
    static Bytes read(int fd,std::size_t maximum){
#if !defined(_WIN32)
        validate(fd);struct stat info{};require(!::fstat(fd,&info) && info.st_size>=0 && static_cast<std::uint64_t>(info.st_size)<=maximum);Bytes bytes(static_cast<std::size_t>(info.st_size));
        std::size_t done=0;while(done<bytes.size()){const auto count=::pread(fd,bytes.data()+done,bytes.size()-done,static_cast<off_t>(done));if(count<0 && errno==EINTR)continue;require(count>0);done+=static_cast<std::size_t>(count);}return bytes;
#else
        (void)fd;(void)maximum;throw TransportError{50};
#endif
    }
    void inspect(int fd,bool directory=false,bool created=false)const{validate(fd,directory);if(inspect_)inspect_(fd,directory,created);}
    static void write(int fd,std::span<const std::uint8_t> bytes){
#if !defined(_WIN32)
        std::size_t done=0;while(done<bytes.size()){const auto count=::pwrite(fd,bytes.data()+done,bytes.size()-done,static_cast<off_t>(done));if(count<0 && errno==EINTR)continue;require(count>0,29);done+=static_cast<std::size_t>(count);}require(!::ftruncate(fd,static_cast<off_t>(bytes.size())),29);
#else
        (void)fd;(void)bytes;throw TransportError{50};
#endif
    }
    Bytes identity()const{
        Bytes bytes(header);std::copy(magic.begin(),magic.end(),bytes.begin());wire_detail::put(bytes,8,4,1);wire_detail::endpoint(bytes,16,endpoint_);return bytes;
    }
    void identity(std::span<const std::uint8_t> bytes)const{
        require(bytes.size()>=header && std::equal(magic.begin(),magic.end(),bytes.begin()) && wire_detail::get(bytes,8,4)==1 && !wire_detail::get(bytes,12,4) && wire_detail::endpoint(bytes,16)==endpoint_);
    }
    Queue load()const{
#if !defined(_WIN32)
        File file(::openat(directory_,"queue",O_RDONLY|O_CLOEXEC|O_NOFOLLOW));require(file.fd>=0,13);
        inspect(file.fd);
        const auto bytes=read(file.fd,header+8+maximum_entries*entry_size);identity(bytes);require(bytes.size()>=header+8);
        require(wire_detail::get(bytes,bytes.size()-8,8)==wire_detail::checksum(std::span(bytes).first(bytes.size()-8)));
        Queue value;value.next=wire_detail::get(bytes,48,8);const auto count=wire_detail::get(bytes,56,4),stopped=wire_detail::get(bytes,60,4);
        require(value.next && count<=maximum_entries && stopped<=1 && bytes.size()==header+8+count*entry_size);value.stopped=stopped!=0;value.entries.reserve(static_cast<std::size_t>(count));
        std::uint64_t previous=0,total=0;
        for(std::size_t i=0;i<count;++i){const auto at=header+i*entry_size;Item item{wire_detail::get(bytes,at,8),wire_detail::get(bytes,at+8,8),wire_detail::get(bytes,at+16,8)};
            require(item.sequence>previous && item.sequence<value.next && item.size>=wire_detail::header_size+8 && item.size<=Limits{}.bytes && item.size<=maximum_bytes-total);previous=item.sequence;total+=item.size;value.entries.push_back(item);}
        return value;
#else
        throw TransportError{50};
#endif
    }
    void save(const Queue& value){
        require(value.entries.size()<=maximum_entries && value.next);auto bytes=identity();bytes.resize(header+value.entries.size()*entry_size+8);
        wire_detail::put(bytes,48,8,value.next);wire_detail::put(bytes,56,4,value.entries.size());wire_detail::put(bytes,60,4,value.stopped?1:0);
        for(std::size_t i=0;i<value.entries.size();++i){const auto at=header+i*entry_size;const auto& item=value.entries[i];wire_detail::put(bytes,at,8,item.sequence);wire_detail::put(bytes,at+8,8,item.size);wire_detail::put(bytes,at+16,8,item.checksum);}
        wire_detail::put(bytes,bytes.size()-8,8,wire_detail::checksum(std::span(bytes).first(bytes.size()-8)));
#if !defined(_WIN32)
        // lock 是固定 inode；queue 可原子替換。持鎖後才重讀，避免別的程序沿用舊 queue。
        int descriptor=::openat(directory_,"queue-update",O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);const bool created=descriptor>=0;
        if(descriptor<0 && errno==EEXIST)descriptor=::openat(directory_,"queue-update",O_RDWR|O_CLOEXEC|O_NOFOLLOW);
        File file(descriptor);require(file.fd>=0,5);inspect(file.fd,false,created);
        try{write(file.fd,bytes);require(!::fsync(file.fd),29);require(!::renameat(directory_,"queue-update",directory_,"queue"),5);}
        catch(...){(void)::unlinkat(directory_,"queue-update",0);throw;}
#else
        throw TransportError{50};
#endif
    }
    bool alive()const{
#if !defined(_WIN32)
        const int fd=::openat(directory_,"lease",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0){if(errno==ENOENT)return false;throw TransportError{5};}
        try{inspect(fd);const auto bytes=read(fd,header);identity(bytes);require(bytes.size()==header && !wire_detail::get(bytes,48,8) && !wire_detail::get(bytes,56,8));
            const auto result=::flock(fd,LOCK_EX|LOCK_NB);const auto error=errno;require(!result || error==EWOULDBLOCK || error==EAGAIN,5);
            bool live=result!=0;
            if(live){const auto process=process_identity::query(endpoint_.native_pid);
                if(!process.error)live=!process.exited && (!endpoint_.birth || process.identity.birth==endpoint_.birth);
                else if(process.error==87)live=false;
            }
            ::close(fd);return live;
        }catch(...){::close(fd);throw;}
#else
        throw TransportError{50};
#endif
    }
    Mailbox(int directory,Endpoint endpoint,bool owner,std::function<void(int,bool,bool)> inspect):endpoint_(endpoint),owner_(owner),name_(name(endpoint)),inspect_(std::move(inspect)){
#if !defined(_WIN32)
        require(wire_detail::valid(endpoint),87);native_pid_=static_cast<std::uint32_t>(::getpid());if(owner){require(endpoint.native_pid==native_pid_,87);
            const auto process=process_identity::query(native_pid_);if(!process.error && endpoint.birth)require(endpoint.birth==process.identity.birth,87);}
        parent_=::fcntl(directory,F_DUPFD_CLOEXEC,0);if(parent_<0)throw TransportError{5};
        bool created=false;
        try{validate(parent_,true);if(owner){const auto result=::mkdirat(parent_,name_.c_str(),0700);const auto error=errno;require(!result,error==EEXIST?183U:5U);created=true;}
            directory_=::openat(parent_,name_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);require(directory_>=0,5);this->inspect(directory_,true,created);
            if(owner){lease_=::openat(directory_,"lease",O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);require(lease_>=0,5);this->inspect(lease_,false,true);require(!::flock(lease_,LOCK_EX|LOCK_NB),5);write(lease_,identity());}
            lock_=::openat(directory_,"lock",O_RDWR|O_CLOEXEC|O_NOFOLLOW|(owner?(O_CREAT|O_EXCL):0),0600);require(lock_>=0,5);this->inspect(lock_,false,owner);
            Lock lock(lock_);if(owner)save({});else{require(alive(),109);(void)load();}
        }catch(...){if(created && directory_>=0){(void)::unlinkat(directory_,"queue",0);(void)::unlinkat(directory_,"queue-update",0);if(lock_>=0)(void)::unlinkat(directory_,"lock",0);if(lease_>=0)(void)::unlinkat(directory_,"lease",0);}
            if(lock_>=0)::close(lock_);if(lease_>=0)::close(lease_);if(directory_>=0)::close(directory_);if(created)(void)::unlinkat(parent_,name_.c_str(),AT_REMOVEDIR);::close(parent_);throw;}
#else
        (void)directory;throw TransportError{50};
#endif
    }
public:
    static std::shared_ptr<Mailbox> create(int directory,Endpoint endpoint,std::function<void(int,bool,bool)> inspect={}){return std::shared_ptr<Mailbox>(new Mailbox(directory,endpoint,true,std::move(inspect)));}
    static std::shared_ptr<Mailbox> open(int directory,Endpoint endpoint,std::function<void(int,bool,bool)> inspect={}){return std::shared_ptr<Mailbox>(new Mailbox(directory,endpoint,false,std::move(inspect)));}
    Mailbox(const Mailbox&)=delete;Mailbox& operator=(const Mailbox&)=delete;
    ~Mailbox(){
#if !defined(_WIN32)
        if(owner_ && native_pid_==static_cast<std::uint32_t>(::getpid()) && lock_>=0)try{Lock lock(lock_);auto queue=load();queue.stopped=true;save(queue);
            for(const auto& item:queue.entries)(void)::unlinkat(directory_,name(item.sequence).c_str(),0);
            (void)::unlinkat(directory_,"queue",0);(void)::unlinkat(directory_,"queue-update",0);(void)::unlinkat(directory_,"lock",0);(void)::unlinkat(directory_,"lease",0);(void)::unlinkat(parent_,name_.c_str(),AT_REMOVEDIR);
        }catch(...){}
        if(lock_>=0)::close(lock_);if(lease_>=0)::close(lease_);if(directory_>=0)::close(directory_);if(parent_>=0)::close(parent_);
#endif
    }
    void send(const Message& message){
#if !defined(_WIN32)
        require(message.target==Interface{} || message.target.endpoint==endpoint_,87);
        const auto bytes=com_rpc::encode(message);Lock lock(lock_);require(alive(),109);auto queue=load();require(!queue.stopped,109);
        std::uint64_t total=0;for(const auto& item:queue.entries)total+=item.size;
        require(queue.entries.size()<maximum_entries && bytes.size()<=maximum_bytes-total && queue.next!=UINT64_MAX,8);
        const auto sequence=queue.next++;const auto filename=name(sequence);
        // index 尚未引用的同序號檔案只能是未提交的 payload；在同一把鎖內覆寫並核對 inode。
        int fd=::openat(directory_,filename.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);const bool created=fd>=0;
        if(fd<0 && errno==EEXIST)fd=::openat(directory_,filename.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);require(fd>=0,5);
        try{inspect(fd,false,created);write(fd,bytes);require(!::fsync(fd),29);queue.entries.push_back({sequence,bytes.size(),wire_detail::checksum(bytes)});save(queue);::close(fd);}
        catch(...){::close(fd);(void)::unlinkat(directory_,filename.c_str(),0);throw;}
#else
        (void)message;throw TransportError{50};
#endif
    }
    std::optional<Message> receive(){
#if !defined(_WIN32)
        require(owner_ && native_pid_==static_cast<std::uint32_t>(::getpid()),5);Lock lock(lock_);require(alive(),109);auto queue=load();require(!queue.stopped,109);if(queue.entries.empty())return std::nullopt;
        const auto item=queue.entries.front();const auto filename=name(item.sequence);const int fd=::openat(directory_,filename.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);require(fd>=0,13);
        try{inspect(fd);const auto bytes=read(fd,Limits{}.bytes);require(bytes.size()==item.size && wire_detail::checksum(bytes)==item.checksum);auto message=com_rpc::decode(bytes);
            require(message.target==Interface{} || message.target.endpoint==endpoint_,87);
            queue.entries.erase(queue.entries.begin());save(queue);(void)::unlinkat(directory_,filename.c_str(),0);::close(fd);return message;
        }catch(...){::close(fd);throw;}
#else
        throw TransportError{50};
#endif
    }
    const Endpoint& endpoint()const noexcept{return endpoint_;}
};
}
