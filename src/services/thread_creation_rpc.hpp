#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm::thread_creation_rpc {
inline constexpr std::uint32_t family=2,create=1;
inline constexpr std::uint64_t signature=UINT64_C(0x3152435434363858);
inline constexpr std::size_t header_size=88,maximum_size=1024*1024;
struct Error {std::uint32_t code;};
inline void put(std::vector<std::uint8_t>& bytes,std::size_t at,std::uint64_t value,unsigned size=4){
    if(at>bytes.size() || size>bytes.size()-at)throw Error{13};
    for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));
}
inline std::uint64_t get(std::span<const std::uint8_t> bytes,std::size_t at,unsigned size=4){
    if(at>bytes.size() || size>bytes.size()-at)throw Error{13};
    std::uint64_t result=0;for(unsigned i=0;i<size;++i)result|=std::uint64_t{bytes[at+i]}<<(8*i);return result;
}
// 共用階段只允許 pending -> committed 或 cancelled；逾時與交付互斥。
// 呼叫端先探測／寫回輸出及取得真正 thread record，才提交；目標在提交前不執行客體碼。
class Channel {
    int descriptor_{-1};std::size_t size_{};void* mapping_{};
    std::pair<std::uint64_t,std::uint64_t> identity_{};
public:
    Channel(int descriptor,std::size_t size):size_(size){
        if(size<header_size || size>maximum_size)throw Error{87};
#if !defined(_WIN32)
        struct stat info{};const auto flags=::fcntl(descriptor,F_GETFL);
        if(flags<0 || (flags&O_ACCMODE)!=O_RDWR || (flags&O_APPEND) || ::fstat(descriptor,&info) ||
           !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink ||
           info.st_size<0 || static_cast<std::uint64_t>(info.st_size)!=size)throw Error{13};
        auto* mapping=::mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,descriptor,0);
        if(mapping==MAP_FAILED)throw Error{8};
        mapping_=mapping;descriptor_=descriptor;
        identity_={static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
#else
        (void)descriptor;throw Error{50};
#endif
    }
    ~Channel(){
#if !defined(_WIN32)
        if(mapping_)::munmap(mapping_,size_);if(descriptor_>=0)::close(descriptor_);
#endif
    }
    Channel(const Channel&)=delete;Channel& operator=(const Channel&)=delete;
    int descriptor()const{return descriptor_;}
    const auto& identity()const{return identity_;}
    std::atomic_ref<std::uint32_t> phase()const{
        static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
        return std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(mapping_)+12));
    }
    bool commit(){std::uint32_t pending=0;return phase().compare_exchange_strong(pending,1,std::memory_order_acq_rel);}
    void cancel(){std::uint32_t pending=0;(void)phase().compare_exchange_strong(pending,2,std::memory_order_acq_rel);}
    std::vector<std::uint8_t> read()const{
        std::vector<std::uint8_t> result(size_);
        // phase 是唯一可變欄位，不以非 atomic memcpy 與另一程序的提交競爭。
        std::memcpy(result.data(),mapping_,12);
        std::memcpy(result.data()+16,static_cast<const std::uint8_t*>(mapping_)+16,size_-16);
        return result;
    }
    static std::shared_ptr<Channel> create(const std::string& session,std::span<const std::uint8_t> bytes){
        if(bytes.size()<header_size || bytes.size()>maximum_size)throw Error{87};
#if !defined(_WIN32)
        struct File{int fd{-1};~File(){if(fd>=0)::close(fd);}} file;
        auto name=session+"/thread-create-XXXXXX";std::vector<char> path(name.begin(),name.end());path.push_back(0);
        file.fd=::mkstemp(path.data());if(file.fd<0)throw Error{8};
        if(::unlink(path.data()))throw Error{5};
        if(::fcntl(file.fd,F_SETFD,FD_CLOEXEC) || ::fchmod(file.fd,0600) || ::ftruncate(file.fd,static_cast<off_t>(bytes.size())))throw Error{8};
        for(std::size_t at=0;at<bytes.size();){const auto count=::pwrite(file.fd,bytes.data()+at,bytes.size()-at,static_cast<off_t>(at));
            if(count<0 && errno==EINTR)continue;if(count<=0)throw Error{13};at+=static_cast<std::size_t>(count);}
        auto result=std::make_shared<Channel>(file.fd,bytes.size());file.fd=-1;return result;
#else
        (void)session;throw Error{50};
#endif
    }
};
}
