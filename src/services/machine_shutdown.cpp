#include "x64arm/machine_shutdown.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/shared_event.hpp"
#include "x64arm/shared_wake.hpp"
#include "monotonic_clock.hpp"
#include <algorithm>
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
constexpr std::uint64_t signature=UINT64_C(0x314e445348343658);
constexpr std::size_t mapping_bytes=17*4096;
struct alignas(8) Header {
    std::uint64_t magic{signature},version{1},host{},phase{},deadline{},generation{},birth{};
    std::uint32_t pid{},seconds{},reason{},action{},message_size{};
    char16_t message[MachineShutdown::max_message+1]{};
};
static_assert(sizeof(Header)<=mapping_bytes);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
std::uint64_t load(std::uint64_t& value){return std::atomic_ref(value).load(std::memory_order_acquire);}
void store(std::uint64_t& value,std::uint64_t next){std::atomic_ref(value).store(next,std::memory_order_release);}
}
struct MachineShutdown::Storage {
    Header* data{};
    int file{-1};
    mutable std::mutex mutex;
    std::shared_ptr<SharedWake> wake;
    std::uint64_t now(std::uint64_t monotonic)const noexcept{return data->host?std::max(monotonic,detail::monotonic_ticks()):monotonic;}
    ShutdownPhase phase()const noexcept{return static_cast<ShutdownPhase>(load(data->phase));}
    void advance_locked(std::uint64_t monotonic){
        if(phase()==ShutdownPhase::Countdown && monotonic>=load(data->deadline)){
            store(data->phase,static_cast<std::uint64_t>(ShutdownPhase::Committed));
            if(wake)wake->notify();
        }
    }
    ~Storage(){
#if !defined(_WIN32)
        if(data)::munmap(data,mapping_bytes);
        if(file>=0)::close(file);
#endif
    }
};
MachineShutdown::MachineShutdown(std::unique_ptr<Storage> value):storage_(std::move(value)){}
MachineShutdown::~MachineShutdown()=default;
std::shared_ptr<MachineShutdown> MachineShutdown::open(const std::string& session,bool host_clock){
#if defined(_WIN32)
    (void)session;(void)host_clock;return {};
#else
    struct Directory {int value{-1};~Directory(){if(value>=0)::close(value);}};
    Directory directory{::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};
    struct stat info{};
    if(directory.value<0 || ::fstat(directory.value,&info) || !S_ISDIR(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077))throw SecurityDescriptorError{5};
    auto storage=std::make_unique<Storage>();
    storage->file=::openat(directory.value,".machine-shutdown-v1",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
    if(storage->file<0 || ::fstat(storage->file,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink!=1)throw SecurityDescriptorError{5};
    {
        SharedEventLock lock(storage->file);
        if(::fstat(storage->file,&info))throw SecurityDescriptorError{13};
        if(!info.st_size){
            Header initial;initial.host=host_clock?1:0;
            if(::ftruncate(storage->file,mapping_bytes))throw SecurityDescriptorError{8};
            std::size_t offset=0;
            while(offset<sizeof(initial)){
                const auto written=::pwrite(storage->file,reinterpret_cast<const char*>(&initial)+offset,sizeof(initial)-offset,static_cast<off_t>(offset));
                if(written<0 && errno==EINTR)continue;
                if(written<=0)throw SecurityDescriptorError{8};
                offset+=static_cast<std::size_t>(written);
            }
        }else if(info.st_size!=static_cast<off_t>(mapping_bytes))throw SecurityDescriptorError{13};
        const auto mapped=::mmap(nullptr,mapping_bytes,PROT_READ|PROT_WRITE,MAP_SHARED,storage->file,0);
        if(mapped==MAP_FAILED)throw SecurityDescriptorError{8};
        storage->data=static_cast<Header*>(mapped);
        const auto& data=*storage->data;
        if(data.magic!=signature || data.version!=1 || load(storage->data->phase)>static_cast<std::uint64_t>(ShutdownPhase::Aborted) ||
           data.action>static_cast<std::uint32_t>(ShutdownAction::PowerOff) || data.seconds>max_timeout || data.message_size>max_message || data.message[data.message_size])throw SecurityDescriptorError{13};
        if(data.host!=(host_clock?1U:0U))throw SecurityDescriptorError{87};
        if(storage->phase()!=ShutdownPhase::Idle && (!data.pid || !data.birth || !data.generation))throw SecurityDescriptorError{13};
    }
    storage->wake=SharedWake::open(session);
    return std::shared_ptr<MachineShutdown>(new MachineShutdown(std::move(storage)));
#endif
}
std::uint32_t MachineShutdown::initiate(const ShutdownRequest& request,std::uint64_t monotonic,bool override_grace,bool scheduled_error){
    if(!request.origin || request.seconds>max_timeout || request.message.size()>max_message ||
       std::find(request.message.begin(),request.message.end(),u'\0')!=request.message.end() ||
       static_cast<std::uint32_t>(request.action)>static_cast<std::uint32_t>(ShutdownAction::PowerOff) || (override_grace && request.seconds))return 87;
    std::lock_guard guard(storage_->mutex);SharedEventLock lock(storage_->file);
    const auto now=storage_->now(monotonic);storage_->advance_locked(now);
    const auto phase=storage_->phase();
    if(phase==ShutdownPhase::Committed)return 1115;
    if(phase==ShutdownPhase::Countdown && !override_grace)return scheduled_error?1190:1115;
    const auto delay=std::uint64_t{request.seconds}*10000000;
    if(now>UINT64_MAX-delay || storage_->data->generation==UINT64_MAX)return 534;
    auto& data=*storage_->data;
    ++data.generation;data.pid=request.origin.pid;data.birth=request.origin.birth;
    data.seconds=request.seconds;data.reason=request.reason;data.action=static_cast<std::uint32_t>(request.action);
    data.message_size=static_cast<std::uint32_t>(request.message.size());
    std::copy(request.message.begin(),request.message.end(),data.message);data.message[data.message_size]=0;
    store(data.deadline,now+delay);
    store(data.phase,static_cast<std::uint64_t>(request.seconds?ShutdownPhase::Countdown:ShutdownPhase::Committed));
    if(storage_->wake)storage_->wake->notify();
    return 0;
}
std::uint32_t MachineShutdown::abort(std::uint64_t monotonic){
    std::lock_guard guard(storage_->mutex);SharedEventLock lock(storage_->file);
    storage_->advance_locked(storage_->now(monotonic));
    if(storage_->phase()!=ShutdownPhase::Countdown)return 1116;
    store(storage_->data->phase,static_cast<std::uint64_t>(ShutdownPhase::Aborted));
    if(storage_->wake)storage_->wake->notify();
    return 0;
}
bool MachineShutdown::poll(std::uint64_t monotonic){
    auto phase=storage_->phase();
    if(phase==ShutdownPhase::Countdown && storage_->now(monotonic)>=load(storage_->data->deadline)){
        std::lock_guard guard(storage_->mutex);SharedEventLock lock(storage_->file);
        storage_->advance_locked(storage_->now(monotonic));phase=storage_->phase();
    }
    return phase==ShutdownPhase::Committed;
}
ShutdownPhase MachineShutdown::phase()const noexcept{return storage_->phase();}
std::uint64_t MachineShutdown::deadline()const noexcept{
    const auto phase=storage_->phase();
    return phase==ShutdownPhase::Committed?0:phase==ShutdownPhase::Countdown?load(storage_->data->deadline):UINT64_MAX;
}
ShutdownSnapshot MachineShutdown::snapshot()const{
    std::lock_guard guard(storage_->mutex);SharedEventLock lock(storage_->file);
    const auto& data=*storage_->data;
    return {storage_->phase(),data.generation,load(storage_->data->deadline),
        {{data.pid,data.birth},data.seconds,data.reason,static_cast<ShutdownAction>(data.action),std::u16string(data.message,data.message_size)}};
}
}
