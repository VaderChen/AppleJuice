#include "shared_policy.hpp"
#include "x64arm/shared_event.hpp"
#include "x64arm/shared_wake.hpp"
#include "x64arm/security_descriptor.hpp"
#include <array>
#include <algorithm>
#include <cerrno>
#include <map>
#include <mutex>
#include <sstream>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm {
namespace {
constexpr std::uint64_t magic=0x31594c4f50413458ULL;
struct Header {std::uint64_t signature{magic};PolicyLease lease;std::uint32_t length{};std::array<char16_t,260> name{};};
std::mutex cache_mutex;
std::map<std::pair<std::uint64_t,std::uint64_t>,std::weak_ptr<SharedPolicy>> cache;
}
struct SharedPolicy::Storage {
    int directory{-1},file{-1},owner{-1};std::uint64_t generation{};std::string owner_name;std::mutex mutex;
    std::shared_ptr<SharedWake> wake;
    Header read(){
#if !defined(_WIN32)
        Header value;struct stat info{};
        if(::fstat(file,&info) || info.st_size!=sizeof(Header) || ::pread(file,&value,sizeof value,0)!=sizeof value || value.signature!=magic || !value.length || value.length>=260)throw SecurityDescriptorError{13};
        if(value.lease.depth && (!value.lease.process || !value.lease.thread || !value.lease.generation || !value.lease.deadline))throw SecurityDescriptorError{13};
        return value;
#else
        throw SecurityDescriptorError{50};
#endif
    }
    void write(const Header& value){
#if !defined(_WIN32)
        if(::pwrite(file,&value,sizeof value,0)!=sizeof value)throw SecurityDescriptorError{8};
#else
        (void)value;throw SecurityDescriptorError{50};
#endif
    }
    ~Storage(){
#if !defined(_WIN32)
        if(owner>=0){
            try{SharedEventLock lock(directory);auto value=read();
                if(value.lease.abandon(static_cast<std::uint64_t>(::getpid()),generation)){write(value);(void)::unlinkat(directory,owner_name.c_str(),0);if(wake)wake->notify();}
            }catch(...){}
            ::close(owner);
        }
        if(file>=0)::close(file);if(directory>=0)::close(directory);
#endif
    }
};
SharedPolicy::SharedPolicy(std::unique_ptr<Storage> value):storage_(std::move(value)){}
SharedPolicy::~SharedPolicy()=default;
std::shared_ptr<SharedPolicy> SharedPolicy::open(const std::string& session,const std::u16string& name){
#if defined(_WIN32)
    (void)session;(void)name;throw SecurityDescriptorError{50};
#else
    if(name.empty() || name.size()>=260)throw SecurityDescriptorError{87};
    std::uint64_t hash=14695981039346656037ULL;for(const auto c:name)hash=(hash^c)*1099511628211ULL;
    std::ostringstream text;text<<"policy-"<<std::hex<<hash;const auto filename=text.str();
    auto storage=std::make_unique<Storage>();storage->owner_name=filename+".owner";
    storage->directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat info{};
    if(storage->directory<0 || ::fstat(storage->directory,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))throw SecurityDescriptorError{5};
    std::lock_guard guard(cache_mutex);SharedEventLock lock(storage->directory);
    storage->file=::openat(storage->directory,filename.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    const bool created=storage->file>=0;
    if(!created){if(errno!=EEXIST)throw SecurityDescriptorError{5};storage->file=::openat(storage->directory,filename.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);}
    if(storage->file<0 || ::fstat(storage->file,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink!=1)throw SecurityDescriptorError{5};
    if(created){Header value;value.length=static_cast<std::uint32_t>(name.size());std::copy(name.begin(),name.end(),value.name.begin());storage->write(value);}
    const auto value=storage->read();if(std::u16string_view(value.name.data(),value.length)!=name)throw SecurityDescriptorError{13};
    const auto key=std::pair{static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
    if(const auto found=cache.find(key);found!=cache.end())if(auto existing=found->second.lock())return existing;
    std::erase_if(cache,[](const auto& entry){return entry.second.expired();});
    storage->wake=SharedWake::open(session);
    auto result=std::shared_ptr<SharedPolicy>(new SharedPolicy(std::move(storage)));cache[key]=result;return result;
#endif
}
PolicyLease::Acquisition SharedPolicy::acquire(std::uint64_t thread,std::uint64_t now){
#if defined(_WIN32)
    (void)thread;(void)now;return {0,50};
#else
    auto& s=*storage_;std::lock_guard guard(s.mutex);SharedEventLock lock(s.directory);auto value=s.read();
    const auto process=static_cast<std::uint64_t>(::getpid());
    if(value.lease.expired(now))value.lease.retire();
    if(value.lease.depth && !(s.owner>=0 && s.generation==value.lease.generation && value.lease.process==process)){
        const auto probe=::openat(s.directory,s.owner_name.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
        if(probe<0){if(errno!=ENOENT)throw SecurityDescriptorError{5};value.lease.retire();}
        else {struct stat info{};
            if(::fstat(probe,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink!=1){::close(probe);throw SecurityDescriptorError{5};}
            const auto acquired=::flock(probe,LOCK_EX|LOCK_NB);const auto error=errno;::close(probe);
            if(acquired==0)value.lease.retire();else if(error!=EWOULDBLOCK && error!=EAGAIN)throw SecurityDescriptorError{13};}
    }
    const bool fresh=!value.lease.depth;const auto result=value.lease.acquire(process,thread,now);if(result.error)return result;
    if(fresh){
        if(s.owner>=0){::close(s.owner);s.owner=-1;}
        // 逾時後替換 lease inode；舊程序只持有舊 inode，不能鎖住新代次。
        if(::unlinkat(s.directory,s.owner_name.c_str(),0)<0 && errno!=ENOENT)throw SecurityDescriptorError{5};
        s.owner=::openat(s.directory,s.owner_name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(s.owner<0 || ::flock(s.owner,LOCK_EX|LOCK_NB)<0)throw SecurityDescriptorError{5};
        s.generation=result.generation;
    }
    s.write(value);return result;
#endif
}
std::uint32_t SharedPolicy::release(std::uint64_t thread,std::uint64_t generation,std::uint64_t now){
#if defined(_WIN32)
    (void)thread;(void)generation;(void)now;return 50;
#else
    auto& s=*storage_;std::lock_guard guard(s.mutex);SharedEventLock lock(s.directory);auto value=s.read();
    const bool held=value.lease.depth!=0;
    const auto result=value.lease.release(static_cast<std::uint64_t>(::getpid()),thread,generation,now);s.write(value);
    if(s.owner>=0 && (!value.lease.depth || value.lease.generation!=s.generation)){
        if(value.lease.generation==s.generation)(void)::unlinkat(s.directory,s.owner_name.c_str(),0);
        ::close(s.owner);s.owner=-1;
    }
    if(held && !value.lease.depth && s.wake)s.wake->notify();
    return result;
#endif
}
}
