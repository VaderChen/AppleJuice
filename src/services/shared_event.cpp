#include "x64arm/shared_event.hpp"
#include "x64arm/shared_wake.hpp"
#include "x64arm/host_services.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <iomanip>
#include <sstream>
#include <map>
#include <mutex>
#include "x64arm/security_descriptor.hpp"
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/xattr.h>
#endif
namespace x64arm {
namespace {
constexpr std::uint64_t event_magic=0x544e564552413458ULL;
constexpr std::size_t page_size=4096;
struct EventHeader {
    std::uint64_t signature{event_magic},references{};
    std::uint32_t manual{},signal{},characters{};
    std::array<char16_t,260> name{};
};
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
std::uint32_t error_code(){return errno==ENOENT?2U:errno==EACCES || errno==EPERM?5U:errno==ENOMEM || errno==ENOSPC?8U:87U;}
std::string object_name(std::u16string_view name) {
    std::uint64_t a=14695981039346656037ULL,b=0x84222325cbf29ce4ULL;
    for(auto c:name){a=(a^c)*1099511628211ULL;b=(b^(c+1U))*0x100000001b3ULL;}
    std::ostringstream out;out<<"section-"<<std::hex<<std::setw(16)<<std::setfill('0')<<a<<std::setw(16)<<b;return out.str();
}
}
struct SharedEventStorage {
    std::shared_ptr<SharedWake> wake;
    int directory{-1},file{-1};EventHeader* data{};std::string name;bool counted{};
    ~SharedEventStorage() {
#if !defined(_WIN32)
        if(data) {
            try {SharedEventLock lock(directory);if(counted && data->references && !--data->references)(void)::unlinkat(directory,name.c_str(),0);}catch(...){}
            ::munmap(data,page_size);
        }
        if(file>=0)::close(file);if(directory>=0)::close(directory);
#endif
    }
};
SharedEventLock::SharedEventLock(int value):descriptor(value) {
#if !defined(_WIN32)
    if(descriptor>=0)while(::flock(descriptor,LOCK_EX)<0)if(errno!=EINTR)throw HostCallFault("無法鎖定共用事件命名空間");
#endif
}
SharedEventLock::~SharedEventLock(){
#if !defined(_WIN32)
    if(descriptor>=0)(void)::flock(descriptor,LOCK_UN);
#endif
}
bool SharedEvent::manual_reset()const noexcept{return storage_->data->manual!=0;}
bool SharedEvent::signaled()const noexcept{return std::atomic_ref(storage_->data->signal).load(std::memory_order_acquire)!=0;}
int SharedEvent::descriptor()const noexcept{return storage_->file;}
int SharedEvent::lock_descriptor()const noexcept{return storage_->directory;}
void SharedEvent::set(bool value){
    {SharedEventLock lock(storage_->directory);std::atomic_ref(storage_->data->signal).store(value?1U:0U,std::memory_order_release);}
    if(value && storage_->wake)storage_->wake->notify();
}
void SharedEvent::consume_locked()noexcept{if(!manual_reset())std::atomic_ref(storage_->data->signal).store(0,std::memory_order_release);}
OpenSharedEvent inherit_shared_event(const std::string& session,int descriptor) {
#if defined(_WIN32)
    (void)session;(void)descriptor;return {{},50};
#else
    auto storage=std::make_shared<SharedEventStorage>();storage->file=descriptor;(void)::fcntl(descriptor,F_SETFD,FD_CLOEXEC);
    storage->directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat info{};
    if(storage->directory<0 || ::fstat(storage->directory,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))return {{},5};
    SharedEventLock lock(storage->directory);EventHeader header;
    if(::fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_size!=page_size || ::pread(descriptor,&header,sizeof(header),0)!=sizeof(header) || header.signature!=event_magic || !header.characters || header.characters>=260)return {{},6};
    const auto mapped=::mmap(nullptr,page_size,PROT_READ|PROT_WRITE,MAP_SHARED,descriptor,0);if(mapped==MAP_FAILED)return {{},8};
    storage->data=static_cast<EventHeader*>(mapped);storage->name=object_name(std::u16string_view(header.name.data(),header.characters));
    ++storage->data->references;storage->counted=true;storage->wake=SharedWake::open(session);return {std::make_shared<SharedEvent>(std::move(storage)),0,false};
#endif
}
OpenSharedEvent open_shared_event(const std::string& session,std::u16string name,bool create,bool manual,bool initial,const ObjectSecurityFactory& security) {
#if defined(_WIN32)
    (void)session;(void)name;(void)create;(void)manual;(void)initial;(void)security;return {{},50};
#else
    if(name.starts_with(u"Local\\"))name.erase(0,6);
    if(name.empty() || name.size()>=260 || name.find(u'\\',name.starts_with(u"Global\\")?7:0)!=name.npos)return {{},123};
    auto storage=std::make_shared<SharedEventStorage>();storage->name=object_name(name);
    storage->directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat info{};
    if(storage->directory<0 || ::fstat(storage->directory,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))return {{},5};
    SharedEventLock lock(storage->directory);bool created=false;
    if(create){storage->file=::openat(storage->directory,storage->name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);created=storage->file>=0;if(!created && errno!=EEXIST)return {{},error_code()};}
    if(storage->file<0)storage->file=::openat(storage->directory,storage->name.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(storage->file<0)return {{},error_code()};
    if(created) {
        EventHeader header;header.manual=manual;header.signal=initial;header.characters=static_cast<std::uint32_t>(name.size());std::copy(name.begin(),name.end(),header.name.begin());
        if(::ftruncate(storage->file,page_size) || ::pwrite(storage->file,&header,sizeof(header),0)!=sizeof(header)) {const auto error=error_code();(void)::unlinkat(storage->directory,storage->name.c_str(),0);return {{},error};}
        try {if(security)set_shared_object_security(storage->file,security());}
        catch(...) {(void)::unlinkat(storage->directory,storage->name.c_str(),0);throw;}
    }
    if(::fstat(storage->file,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_size!=page_size)return {{},6};
    auto mapped=::mmap(nullptr,page_size,PROT_READ|PROT_WRITE,MAP_SHARED,storage->file,0);
    if(mapped==MAP_FAILED){const auto error=error_code();if(created)(void)::unlinkat(storage->directory,storage->name.c_str(),0);return {{},error};}
    storage->data=static_cast<EventHeader*>(mapped);const auto& header=*storage->data;
    if(header.signature!=event_magic || header.characters!=name.size() || !std::equal(name.begin(),name.end(),header.name.begin()))return {{},6};
    ++storage->data->references;storage->counted=true;storage->wake=SharedWake::open(session);
    return {std::make_shared<SharedEvent>(std::move(storage)),0,!created};
#endif
}
std::vector<std::uint8_t> shared_object_security(int descriptor,bool thread) {
#if defined(__APPLE__) || defined(__linux__)
    const auto name=thread?"user.x64arm.thread-security-v1":"user.x64arm.object-security-v1";
#if defined(__APPLE__)
    const auto size=::fgetxattr(descriptor,name,nullptr,0,0,0);
#else
    const auto size=::fgetxattr(descriptor,name,nullptr,0);
#endif
    if(size<0 && (errno==ENODATA
#if defined(__APPLE__)
       || errno==ENOATTR
#endif
       ))return {};
    if(size<0)throw SecurityDescriptorError{error_code()};
    if(size>131072)throw SecurityDescriptorError{1338};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
#if defined(__APPLE__)
    const auto read=::fgetxattr(descriptor,name,bytes.data(),bytes.size(),0,0);
#else
    const auto read=::fgetxattr(descriptor,name,bytes.data(),bytes.size());
#endif
    if(read!=size)throw SecurityDescriptorError{1237};return bytes;
#else
    (void)descriptor;(void)thread;throw SecurityDescriptorError{50};
#endif
}
void set_shared_object_security(int descriptor,const std::vector<std::uint8_t>& bytes,bool thread) {
#if defined(__APPLE__) || defined(__linux__)
    if(bytes.size()>131072)throw SecurityDescriptorError{8};
    const auto name=thread?"user.x64arm.thread-security-v1":"user.x64arm.object-security-v1";
#if defined(__APPLE__)
    const auto result=::fsetxattr(descriptor,name,bytes.data(),bytes.size(),0,0);
#else
    const auto result=::fsetxattr(descriptor,name,bytes.data(),bytes.size(),0);
#endif
    if(result<0)throw SecurityDescriptorError{error_code()};
#else
    (void)descriptor;(void)bytes;(void)thread;throw SecurityDescriptorError{50};
#endif
}
namespace {
constexpr std::uint64_t mutex_magic=0x5854554d52413458ULL;
struct MutexHeader {
    std::uint64_t signature{mutex_magic},references{},process{},thread{};
    std::uint32_t recursion{},abandoned{},characters{};
    std::array<char16_t,260> name{};
};
}
struct SharedMutexStorage {
    int directory{-1},file{-1};MutexHeader* data{};std::string name;bool counted{},lease{};
    ~SharedMutexStorage(){
#if !defined(_WIN32)
        if(data){try{SharedEventLock lock(directory);if(counted && data->references && !--data->references)(void)::unlinkat(directory,name.c_str(),0);}catch(...){}::munmap(data,page_size);}
        if(file>=0)::close(file);if(directory>=0)::close(directory);
#endif
    }
};
namespace {
std::mutex mutex_cache_lock;
std::map<std::array<std::uint64_t,3>,std::weak_ptr<SharedMutexStorage>> mutex_cache;
}
int SharedMutex::descriptor()const noexcept{return storage_->file;}
int SharedMutex::lock_descriptor()const noexcept{return storage_->directory;}
bool SharedMutex::owned()const noexcept{return storage_->lease;}
void SharedMutex::detach_handles(){
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);if(storage_->counted){if(storage_->data->references && !--storage_->data->references)(void)::unlinkat(storage_->directory,storage_->name.c_str(),0);storage_->counted=false;}
#endif
}
bool SharedMutex::available_locked(std::uint64_t thread){
#if !defined(_WIN32)
    auto& data=*storage_->data;
    if(storage_->lease)return data.thread==thread;
    if(!data.process)return true;
    // ownership lease 由核心維護；程序退出或遭終止後自動釋放，不依賴 PID 存活猜測。
    if(::flock(storage_->file,LOCK_EX|LOCK_NB)<0){if(errno==EWOULDBLOCK || errno==EAGAIN)return false;throw HostCallFault("無法探查共用 mutex ownership");}
    (void)::flock(storage_->file,LOCK_UN);data.process=0;data.thread=0;data.recursion=0;data.abandoned=1;return true;
#else
    (void)thread;return false;
#endif
}
bool SharedMutex::acquire_locked(std::uint64_t thread){
#if !defined(_WIN32)
    if(!available_locked(thread))throw HostCallFault("共用 mutex 尚未釋放");
    auto& data=*storage_->data;if(data.recursion==UINT32_MAX)throw HostCallFault("Mutex 遞迴持有次數超過上限");
    if(!storage_->lease){if(::flock(storage_->file,LOCK_EX|LOCK_NB)<0)throw HostCallFault("共用 mutex ownership 競爭");storage_->lease=true;}
    const bool abandoned=data.abandoned!=0;data.process=static_cast<std::uint64_t>(::getpid());data.thread=thread;++data.recursion;data.abandoned=0;return abandoned;
#else
    (void)thread;throw HostCallFault("此平台沒有共用 mutex 後端");
#endif
}
std::uint32_t SharedMutex::release_locked(std::uint64_t thread){
#if !defined(_WIN32)
    auto& data=*storage_->data;if(!storage_->lease || data.thread!=thread || !data.recursion)return 288;
    if(!--data.recursion){data.process=0;data.thread=0;storage_->lease=false;(void)::flock(storage_->file,LOCK_UN);}return 0;
#else
    (void)thread;return 50;
#endif
}
SharedMutexState SharedMutex::state_locked(){
#if !defined(_WIN32)
    (void)available_locked(0); // 擁有者程序已結束時，依 ownership lease 轉為 abandoned。
    const auto& data=*storage_->data;return {data.process,data.thread,data.recursion,data.abandoned!=0};
#else
    return {};
#endif
}
void SharedMutex::abandon(std::uint64_t thread){
#if !defined(_WIN32)
    SharedEventLock lock(storage_->directory);auto& data=*storage_->data;
    if(storage_->lease && data.thread==thread){data.process=0;data.thread=0;data.recursion=0;data.abandoned=1;storage_->lease=false;(void)::flock(storage_->file,LOCK_UN);}
#else
    (void)thread;
#endif
}
OpenSharedMutex open_shared_mutex(const std::string& session,std::u16string name,bool create,std::uint64_t owner,std::uint32_t recursion,const ObjectSecurityFactory& security){
#if defined(_WIN32)
    (void)session;(void)name;(void)create;(void)owner;(void)recursion;(void)security;return {{},50};
#else
    if(name.starts_with(u"Local\\"))name.erase(0,6);
    if(name.empty() || name.size()>=260 || name.find(u'\\',name.starts_with(u"Global\\")?7:0)!=name.npos)return {{},123};
    auto storage=std::make_shared<SharedMutexStorage>();storage->name=object_name(name);
    storage->directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat info{};
    if(storage->directory<0 || ::fstat(storage->directory,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))return {{},5};
    SharedEventLock lock(storage->directory);bool created=false;
    if(create){storage->file=::openat(storage->directory,storage->name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);created=storage->file>=0;if(!created && errno!=EEXIST)return {{},error_code()};}
    if(storage->file<0)storage->file=::openat(storage->directory,storage->name.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(storage->file<0)return {{},error_code()};
    if(created){
        MutexHeader header;header.characters=static_cast<std::uint32_t>(name.size());std::copy(name.begin(),name.end(),header.name.begin());
        if(::ftruncate(storage->file,page_size) || ::pwrite(storage->file,&header,sizeof header,0)!=sizeof header){const auto error=error_code();(void)::unlinkat(storage->directory,storage->name.c_str(),0);return {{},error};}
        try{if(security)set_shared_object_security(storage->file,security());}catch(...){(void)::unlinkat(storage->directory,storage->name.c_str(),0);throw;}
    }
    MutexHeader header;
    if(::fstat(storage->file,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_size!=page_size || ::pread(storage->file,&header,sizeof header,0)!=sizeof header || header.signature!=mutex_magic || header.characters!=name.size() || !std::equal(name.begin(),name.end(),header.name.begin()))return {{},6};
    const std::array key{static_cast<std::uint64_t>(::getpid()),static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
    std::unique_lock cache_lock(mutex_cache_lock);
    std::erase_if(mutex_cache,[](const auto& item){return item.second.expired();});
    if(const auto found=mutex_cache.find(key);found!=mutex_cache.end())if(auto cached=found->second.lock()){
        if(!cached->counted){++cached->data->references;cached->counted=true;}
        return {std::make_shared<SharedMutex>(std::move(cached)),0,true};
    }
    const auto mapped=::mmap(nullptr,page_size,PROT_READ|PROT_WRITE,MAP_SHARED,storage->file,0);if(mapped==MAP_FAILED)return {{},8};
    storage->data=static_cast<MutexHeader*>(mapped);++storage->data->references;storage->counted=true;
    mutex_cache[key]=storage;cache_lock.unlock();
    auto mutex=std::make_shared<SharedMutex>(storage);
    if(created && owner){(void)mutex->acquire_locked(owner);storage->data->recursion=recursion;}
    return {std::move(mutex),0,!created};
#endif
}
OpenSharedMutex inherit_shared_mutex(const std::string& session,int descriptor){
#if defined(_WIN32)
    (void)session;(void)descriptor;return {{},50};
#else
    struct Close{int fd;~Close(){::close(fd);}} incoming{descriptor};struct stat before{};MutexHeader header;
    if(::fstat(descriptor,&before) || !S_ISREG(before.st_mode) || before.st_uid!=::geteuid() || (before.st_mode&077) || before.st_size!=page_size || ::pread(descriptor,&header,sizeof header,0)!=sizeof header || header.signature!=mutex_magic || !header.characters || header.characters>=260)return {{},6};
    // 重新開啟獨立 file description；SCM_RIGHTS／繼承 fd 不可共享父程序的 ownership lease。
    auto result=open_shared_mutex(session,std::u16string(header.name.data(),header.characters),false);if(result.error)return result;
    struct stat after{};if(::fstat(result.mutex->descriptor(),&after) || before.st_dev!=after.st_dev || before.st_ino!=after.st_ino)return {{},6};return result;
#endif
}
}
