#include "x64arm/shared_event.hpp"
#include "x64arm/shared_wake.hpp"
#include "shared_object_lifetime.hpp"
#include "x64arm/host_services.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
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
constexpr std::uint64_t semaphore_magic=0x504d455352413458ULL;
constexpr std::uint64_t mutex_magic=0x5854554d52413458ULL;
constexpr std::size_t page_size=4096;
struct EventHeader {
    std::uint64_t signature{event_magic},references{};
    std::uint32_t manual{},signal{},characters{};
    std::array<char16_t,260> name{};
    std::uint32_t maximum{}; // event 原有尾端 padding；舊版 event 的欄位與大小不變。
};
static_assert(sizeof(EventHeader)==552 && offsetof(EventHeader,maximum)==548);
static_assert(sizeof(EventHeader)<=detail::named_object_page_hint_offset);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
std::uint32_t error_code(){return errno==ENOENT?2U:errno==EACCES || errno==EPERM?5U:errno==ENOMEM || errno==ENOSPC?8U:87U;}
std::string object_name(std::u16string_view name) {
    std::uint64_t a=14695981039346656037ULL,b=0x84222325cbf29ce4ULL;
    for(auto c:name){a=(a^c)*1099511628211ULL;b=(b^(c+1U))*0x100000001b3ULL;}
    std::ostringstream out;out<<"section-"<<std::hex<<std::setw(16)<<std::setfill('0')<<a<<std::setw(16)<<b;return out.str();
}
#if !defined(_WIN32)
// 呼叫端持有命名空間鎖；只核對正在開啟的物件，不掃描或清除整個 session。
// references 無法在 SIGKILL／_exit 時遞減。macOS 的核心檔案引用查詢同時辨識
// 舊版引擎的 fd、繼承／傳輸中的 fd 及映射，無須改變已執行程序的共享格式。
int open_object_file(const std::string& session,int directory,const std::string& name,bool create,bool& created) {
    created=false;
    int file=::openat(directory,name.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    file=detail::reopen_live_named_object(session,directory,name,file);
    if(file>=0 || errno!=ENOENT || !create)return file;
    file=::openat(directory,name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    created=file>=0;return file;
}
#endif
}
struct SharedEventStorage {
    std::shared_ptr<SharedWake> wake;
    int directory{-1},file{-1};EventHeader* data{};std::string name;bool counted{};
    std::array<std::uint64_t,2> identity{};
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
std::uint32_t SharedEvent::count()const noexcept{return std::atomic_ref(storage_->data->signal).load(std::memory_order_acquire);}
std::uint32_t SharedEvent::maximum()const noexcept{return storage_->data->maximum;}
bool SharedEvent::same_object(const SharedEvent& other)const noexcept{return storage_->identity==other.storage_->identity;}
std::uint32_t SharedEvent::release(std::uint32_t increment,std::uint32_t* previous,bool saturate){
    if(storage_->data->signature!=semaphore_magic)return 6;
    if(!increment || increment>INT32_MAX)return 87;
    {SharedEventLock lock(storage_->directory);const auto before=count(),available=maximum()-before;
     if(increment>available){if(!saturate)return 298;increment=available;}
     if(previous)*previous=before;
     std::atomic_ref(storage_->data->signal).store(before+increment,std::memory_order_release);}
    if(increment && storage_->wake)storage_->wake->notify();return 0;
}
int SharedEvent::descriptor()const noexcept{return storage_->file;}
int SharedEvent::lock_descriptor()const noexcept{return storage_->directory;}
void SharedEvent::set(bool value){
    {SharedEventLock lock(storage_->directory);std::atomic_ref(storage_->data->signal).store(value?1U:0U,std::memory_order_release);}
    if(value && storage_->wake)storage_->wake->notify();
    // 診斷：X64ARM_TRACE_SHARED_EVENTS=1 時記錄跨程序事件的 Set／Reset（含毫秒時間與名稱），用來找 Set 後立即 Reset
    // 使其他程序的等待者錯過訊號的情況。預設關閉。
    static const bool trace=std::getenv("X64ARM_TRACE_SHARED_EVENTS")!=nullptr;
    if(trace){
        const auto& header=*storage_->data;std::string name;
        for(std::uint32_t n=0;n<header.characters && n<header.name.size();++n)name.push_back(header.name[n]<0x80?static_cast<char>(header.name[n]):'?');
        const auto us=static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        std::fprintf(stderr,"[shared_event] pid=%d us=%lld op=%s manual=%u name=%s\n",static_cast<int>(::getpid()),us,value?"set":"reset",header.manual,name.c_str());
    }
}
void SharedEvent::consume_locked()noexcept{
    if(storage_->data->signature==semaphore_magic){const auto before=count();if(before)std::atomic_ref(storage_->data->signal).store(before-1,std::memory_order_release);}
    else if(!manual_reset())std::atomic_ref(storage_->data->signal).store(0,std::memory_order_release);
}
static OpenSharedEvent inherit_shared_signal(const std::string& session,int descriptor,bool semaphore) {
#if defined(_WIN32)
    (void)session;(void)descriptor;(void)semaphore;return {{},50};
#else
    auto storage=std::make_shared<SharedEventStorage>();storage->file=descriptor;(void)::fcntl(descriptor,F_SETFD,FD_CLOEXEC);
    storage->directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat info{};
    if(storage->directory<0 || ::fstat(storage->directory,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))return {{},5};
    SharedEventLock lock(storage->directory);EventHeader header;
    if(::fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_size!=page_size || ::pread(descriptor,&header,sizeof(header),0)!=sizeof(header) || header.signature!=(semaphore?semaphore_magic:event_magic) || !header.characters || header.characters>=260 || (semaphore && (!header.maximum || header.maximum>INT32_MAX || header.signal>header.maximum)))return {{},6};
    storage->identity={static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
    const auto mapped=::mmap(nullptr,page_size,PROT_READ|PROT_WRITE,MAP_SHARED,descriptor,0);if(mapped==MAP_FAILED)return {{},8};
    storage->data=static_cast<EventHeader*>(mapped);storage->name=object_name(std::u16string_view(header.name.data(),header.characters));
    ++storage->data->references;storage->counted=true;storage->wake=SharedWake::open(session);return {std::make_shared<SharedEvent>(std::move(storage)),0,false};
#endif
}
static OpenSharedEvent open_shared_signal(const std::string& session,std::u16string name,bool create,bool manual,std::uint32_t initial,std::uint32_t maximum,bool semaphore,const ObjectSecurityFactory& security) {
#if defined(_WIN32)
    (void)session;(void)name;(void)create;(void)manual;(void)initial;(void)maximum;(void)semaphore;(void)security;return {{},50};
#else
    if(name.starts_with(u"Local\\"))name.erase(0,6);
    if(name.empty() || name.size()>=260 || name.find(u'\\',name.starts_with(u"Global\\")?7:0)!=name.npos)return {{},123};
    auto storage=std::make_shared<SharedEventStorage>();storage->name=object_name(name);
    storage->directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat info{};
    if(storage->directory<0 || ::fstat(storage->directory,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))return {{},5};
    SharedEventLock lock(storage->directory);bool created=false;
    storage->file=open_object_file(session,storage->directory,storage->name,create,created);
    if(storage->file<0)return {{},error_code()};
    if(created) {
        if(semaphore && (!maximum || maximum>INT32_MAX || initial>maximum)){(void)::unlinkat(storage->directory,storage->name.c_str(),0);return {{},87};}
        EventHeader header;header.signature=semaphore?semaphore_magic:event_magic;header.manual=manual;header.signal=initial;header.maximum=maximum;header.characters=static_cast<std::uint32_t>(name.size());std::copy(name.begin(),name.end(),header.name.begin());
        if(::ftruncate(storage->file,page_size) || ::pwrite(storage->file,&header,sizeof(header),0)!=sizeof(header)) {const auto error=error_code();(void)::unlinkat(storage->directory,storage->name.c_str(),0);return {{},error};}
        try {if(security)set_shared_object_security(storage->file,security());}
        catch(...) {(void)::unlinkat(storage->directory,storage->name.c_str(),0);throw;}
        // 建立者即持有者：第一個從其他程序開啟的呼叫者可免完整掃描。
        detail::named_object_record_holder(storage->file,detail::named_object_page_hint_offset,::getpid(),storage->file);
    }
    if(::fstat(storage->file,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_size!=page_size)return {{},6};
    storage->identity={static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
    auto mapped=::mmap(nullptr,page_size,PROT_READ|PROT_WRITE,MAP_SHARED,storage->file,0);
    if(mapped==MAP_FAILED){const auto error=error_code();if(created)(void)::unlinkat(storage->directory,storage->name.c_str(),0);return {{},error};}
    storage->data=static_cast<EventHeader*>(mapped);const auto& header=*storage->data;
    if(header.signature!=(semaphore?semaphore_magic:event_magic) || header.characters!=name.size() || !std::equal(name.begin(),name.end(),header.name.begin()) || (semaphore && (!header.maximum || header.maximum>INT32_MAX || header.signal>header.maximum)))return {{},6};
    ++storage->data->references;storage->counted=true;storage->wake=SharedWake::open(session);
    return {std::make_shared<SharedEvent>(std::move(storage)),0,!created};
#endif
}
OpenSharedEvent inherit_shared_event(const std::string& session,int descriptor){return inherit_shared_signal(session,descriptor,false);}
OpenSharedEvent inherit_shared_semaphore(const std::string& session,int descriptor){return inherit_shared_signal(session,descriptor,true);}
OpenSharedEvent open_shared_event(const std::string& session,std::u16string name,bool create,bool manual,bool initial,const ObjectSecurityFactory& security){return open_shared_signal(session,std::move(name),create,manual,initial,1,false,security);}
OpenSharedEvent open_shared_semaphore(const std::string& session,std::u16string name,bool create,std::uint32_t initial,std::uint32_t maximum,const ObjectSecurityFactory& security){return open_shared_signal(session,std::move(name),create,false,initial,maximum,true,security);}
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
struct MutexHeader {
    std::uint64_t signature{mutex_magic},references{},process{},thread{};
    std::uint32_t recursion{},abandoned{},characters{};
    std::array<char16_t,260> name{};
};
static_assert(sizeof(MutexHeader)<=detail::named_object_page_hint_offset);
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
    storage->file=open_object_file(session,storage->directory,storage->name,create,created);
    if(storage->file<0)return {{},error_code()};
    if(created){
        MutexHeader header;header.characters=static_cast<std::uint32_t>(name.size());std::copy(name.begin(),name.end(),header.name.begin());
        if(::ftruncate(storage->file,page_size) || ::pwrite(storage->file,&header,sizeof header,0)!=sizeof header){const auto error=error_code();(void)::unlinkat(storage->directory,storage->name.c_str(),0);return {{},error};}
        try{if(security)set_shared_object_security(storage->file,security());}catch(...){(void)::unlinkat(storage->directory,storage->name.c_str(),0);throw;}
        detail::named_object_record_holder(storage->file,detail::named_object_page_hint_offset,::getpid(),storage->file);
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
