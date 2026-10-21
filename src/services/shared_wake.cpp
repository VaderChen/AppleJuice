#include "x64arm/startup_profile.hpp"
#include "x64arm/shared_wake.hpp"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
#endif

namespace x64arm {
namespace {
constexpr std::uint64_t signature=UINT64_C(0x31574b5741523458);
struct Header {std::uint64_t magic{signature},epoch{};std::uint32_t waiters{},reserved{};};
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
bool enabled() noexcept {
    static const bool value=[] {const auto* raw=std::getenv("X64ARM_SHARED_WAKE");return !raw || std::strcmp(raw,"0")!=0;}();
    return value;
}
}
struct SharedWake::Storage {
    Header* data{};
    ~Storage(){
#if !defined(_WIN32)
        if(data)::munmap(data,4096);
#endif
    }
};
SharedWake::SharedWake(std::unique_ptr<Storage> storage):storage_(std::move(storage)){}
SharedWake::~SharedWake()=default;
std::shared_ptr<SharedWake> SharedWake::open(const std::string& session) {
    if(!enabled())return {};
#if !defined(__APPLE__)
    (void)session;return {};
#else
    if(__builtin_available(macOS 14.4,*)){}else return {};
    static std::mutex mutex;static std::map<std::string,std::weak_ptr<SharedWake>> cache;
    const std::lock_guard guard(mutex);
    if(const auto found=cache.find(session);found!=cache.end())if(auto value=found->second.lock())return value;
    struct Descriptor {int value{-1};~Descriptor(){if(value>=0)::close(value);}};
    Descriptor directory{::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};struct stat info{};
    if(directory.value<0 || ::fstat(directory.value,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))return {};
    // 只鎖通知檔，不再次取得物件的 session flock；建立端與繼承端共用相同 inode。
    Descriptor file{::openat(directory.value,".wake-v1",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600)};
    if(file.value<0 || ::fstat(file.value,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077))return {};
    while(::flock(file.value,LOCK_EX)<0)if(errno!=EINTR)return {};
    if(::fstat(file.value,&info))return {};
    if(info.st_size==0){
        const Header initial;
        if(::ftruncate(file.value,4096) || ::pwrite(file.value,&initial,sizeof(initial),0)!=sizeof(initial))return {};
    }else if(info.st_size!=4096)return {};
    Header header;
    if(::pread(file.value,&header,sizeof(header),0)!=sizeof(header) || header.magic!=signature)return {};
    auto storage=std::make_unique<Storage>();
    const auto mapped=::mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_SHARED,file.value,0);
    if(mapped==MAP_FAILED)return {};
    storage->data=static_cast<Header*>(mapped);
    auto result=std::shared_ptr<SharedWake>(new SharedWake(std::move(storage)));
    std::erase_if(cache,[](const auto& item){return item.second.expired();});cache[session]=result;return result;
#endif
}
std::uint64_t SharedWake::snapshot()const noexcept{return std::atomic_ref(storage_->data->epoch).load(std::memory_order_acquire);}
void SharedWake::notify()noexcept {
    auto& data=*storage_->data;std::atomic_ref(data.epoch).fetch_add(1,std::memory_order_seq_cst);
#if defined(__APPLE__)
    if(__builtin_available(macOS 14.4,*))if(std::atomic_ref(data.waiters).load(std::memory_order_seq_cst))
        (void)os_sync_wake_by_address_all(&data.epoch,sizeof(data.epoch),OS_SYNC_WAKE_BY_ADDRESS_SHARED);
#endif
}
bool SharedWake::wait_for(std::uint64_t expected,std::uint64_t nanoseconds)noexcept {
    startup_profile::Scope measurement("scheduler","shared_wait");
#if defined(__APPLE__)
    if(__builtin_available(macOS 14.4,*)){
        if(!nanoseconds || snapshot()!=expected)return true;
        auto& data=*storage_->data;std::atomic_ref(data.waiters).fetch_add(1,std::memory_order_seq_cst);
        const auto result=os_sync_wait_on_address_with_timeout(&data.epoch,expected,sizeof(data.epoch),
            OS_SYNC_WAIT_ON_ADDRESS_SHARED,OS_CLOCK_MACH_ABSOLUTE_TIME,nanoseconds);
        const auto error=errno;std::atomic_ref(data.waiters).fetch_sub(1,std::memory_order_release);
        return result>=0 || error==EINTR || error==ETIMEDOUT;
    }
#endif
    (void)expected;(void)nanoseconds;return false;
}
}
