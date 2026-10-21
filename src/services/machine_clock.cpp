#include "x64arm/machine_clock.hpp"
#include "x64arm/shared_event.hpp"
#include "x64arm/shared_wake.hpp"
#include "monotonic_clock.hpp"
#include "x64arm/security_descriptor.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <mutex>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm {
namespace {
constexpr std::uint64_t signature=UINT64_C(0x314b4c434d343658),limit=INT64_MAX;
constexpr std::size_t page_size=4096;
using Record=std::array<std::uint64_t,5>; // UTC、單調參考、固定增量、調整量、停用旗標。
struct alignas(8) Header {
    std::uint64_t magic{signature},version{1},host{},published{},virtual_ticks{};
    std::array<Record,2> records{};
};
static_assert(sizeof(Header)<=page_size);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
std::uint64_t load(std::uint64_t& word){return std::atomic_ref(word).load(std::memory_order_acquire);}
void store(std::uint64_t& word,std::uint64_t value){std::atomic_ref(word).store(value,std::memory_order_release);}
bool valid(const Record& value){return value[0]<=limit && value[2] && value[2]<=UINT32_MAX && value[3]<=UINT32_MAX && value[4]<=1;}
std::uint64_t time_at(const Record& value,std::uint64_t now){
    const auto elapsed=now>=value[1]?now-value[1]:0,rate=value[4]?value[2]:value[3];if(!rate)return value[0];
    const auto periods=elapsed/value[2];if(periods>(limit-value[0])/rate)return limit;
    const auto base=value[0]+periods*rate,remainder=(elapsed%value[2])*rate/value[2];return remainder>limit-base?limit:base+remainder;
}
}
struct MachineClock::Storage {
    Header* data{};int file{-1};mutable std::mutex mutex;mutable Record cached{};mutable std::uint64_t cached_revision{};
    std::shared_ptr<SharedWake> wake;
    Record read_locked()const noexcept{
        // mutex 保護本實體的快照；相同發布版本沿用已驗證資料。
        // 每次仍以 acquire 觀測共用版本，其他程序的時間／速率修改立即可見。
        if(load(data->published)==cached_revision)return cached;
        // 原子化每個欄位，避免另一 writer 重新使用舊 bank 時形成 C++ data race。
        for(unsigned attempt=0;attempt<32;++attempt){const auto before=load(data->published);Record value;
            for(std::size_t i=0;i<value.size();++i)value[i]=load(data->records[before&1U][i]);
            if(before==load(data->published) && valid(value)){cached=value;cached_revision=before;return value;}
        }return cached;
    }
    void publish_locked(const Record& value){const auto next=load(data->published)+1;for(std::size_t i=0;i<value.size();++i)store(data->records[next&1U][i],value[i]);store(data->published,next);cached=value;cached_revision=next;}
    ~Storage(){
#if !defined(_WIN32)
        if(data)::munmap(data,page_size);if(file>=0)::close(file);
#endif
    }
};
MachineClock::MachineClock(std::unique_ptr<Storage> value):storage_(std::move(value)){}
MachineClock::~MachineClock()=default;
std::shared_ptr<MachineClock> MachineClock::open(const std::string& session,bool host,std::uint64_t monotonic,std::uint64_t filetime){
#if defined(_WIN32)
    (void)session;(void)host;(void)monotonic;(void)filetime;return {};
#else
    if(filetime>limit)throw SecurityDescriptorError{87};
    struct Directory{int value{-1};~Directory(){if(value>=0)::close(value);}} directory{::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};
    struct stat info{};if(directory.value<0 || ::fstat(directory.value,&info) || !S_ISDIR(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077))throw SecurityDescriptorError{5};
    auto storage=std::make_unique<Storage>();storage->file=::openat(directory.value,".machine-clock-v1",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(storage->file<0 || ::fstat(storage->file,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink!=1)throw SecurityDescriptorError{5};
    {
        SharedEventLock lock(storage->file);if(::fstat(storage->file,&info))throw SecurityDescriptorError{13};
        if(!info.st_size){Header initial;initial.host=host?1:0;initial.virtual_ticks=monotonic;initial.records[0]={filetime,monotonic,10000,10000,1};initial.records[1]=initial.records[0];
            if(::ftruncate(storage->file,page_size))throw SecurityDescriptorError{8};std::size_t offset=0;
            while(offset<sizeof(initial)){const auto written=::pwrite(storage->file,reinterpret_cast<const char*>(&initial)+offset,sizeof(initial)-offset,static_cast<off_t>(offset));
                if(written<0 && errno==EINTR)continue;if(written<=0)throw SecurityDescriptorError{8};offset+=static_cast<std::size_t>(written);}
        }else if(info.st_size!=static_cast<off_t>(page_size))throw SecurityDescriptorError{13};
        const auto mapped=::mmap(nullptr,page_size,PROT_READ|PROT_WRITE,MAP_SHARED,storage->file,0);if(mapped==MAP_FAILED)throw SecurityDescriptorError{8};storage->data=static_cast<Header*>(mapped);
        if(storage->data->magic!=signature || storage->data->version!=1)throw SecurityDescriptorError{13};if(storage->data->host!=(host?1U:0U))throw SecurityDescriptorError{87};
        storage->cached_revision=load(storage->data->published);const auto active=storage->cached_revision&1U;for(std::size_t i=0;i<storage->cached.size();++i)storage->cached[i]=load(storage->data->records[active][i]);if(!valid(storage->cached))throw SecurityDescriptorError{13};
    }
    storage->wake=SharedWake::open(session);return std::shared_ptr<MachineClock>(new MachineClock(std::move(storage)));
#endif
}
std::uint64_t MachineClock::synchronize(std::uint64_t monotonic)const noexcept{
    // 宿主時鐘：所有程序直接讀同一個硬體計數器；不採用呼叫端可能已超前的程序內數值。
    auto& data=*storage_->data;if(data.host)return detail::monotonic_ticks();
    auto prior=load(data.virtual_ticks);while(prior<monotonic && !std::atomic_ref(data.virtual_ticks).compare_exchange_weak(prior,monotonic,std::memory_order_acq_rel,std::memory_order_acquire)){}
    return std::max(prior,monotonic);
}
MachineTime MachineClock::snapshot(std::uint64_t monotonic)const noexcept{
    std::lock_guard guard(storage_->mutex);const auto value=storage_->read_locked();const auto now=synchronize(monotonic);
    return {time_at(value,now),now,storage_->cached_revision,static_cast<std::uint32_t>(value[2]),static_cast<std::uint32_t>(value[3]),value[4]!=0};
}
void MachineClock::set_time(std::uint64_t monotonic,std::uint64_t filetime){
    if(filetime>limit)throw SecurityDescriptorError{87};std::lock_guard guard(storage_->mutex);SharedEventLock lock(storage_->file);const auto now=synchronize(monotonic);
    auto value=storage_->read_locked();value[0]=filetime;value[1]=now;storage_->publish_locked(value);if(storage_->wake)storage_->wake->notify();
}
void MachineClock::set_adjustment(std::uint64_t monotonic,std::uint32_t adjustment,bool disabled){
    std::lock_guard guard(storage_->mutex);SharedEventLock lock(storage_->file);const auto now=synchronize(monotonic);auto value=storage_->read_locked();
    value[0]=time_at(value,now);value[1]=now;value[3]=disabled?value[2]:adjustment;value[4]=disabled?1:0;storage_->publish_locked(value);if(storage_->wake)storage_->wake->notify();
}
}
