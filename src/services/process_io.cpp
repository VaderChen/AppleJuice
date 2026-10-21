#include "x64arm/process_io.hpp"
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sched.h>
#include <unistd.h>
#endif
#include <cstring>

namespace x64arm::process_io {
namespace {
thread_local Counters* current{};
thread_local Batch* batch{};
constexpr std::uint64_t magic=UINT64_C(0x314f495034363458);
struct Packet {
    std::uint64_t signature{magic},birth{}; std::uint32_t pid{},version{1};
    Counters counters;std::uint64_t checksum{};
};
static_assert(sizeof(Packet)==80);
std::uint64_t checksum(const Packet& packet){
    auto copy=packet;copy.checksum=0;const auto bytes=std::bit_cast<std::array<std::uint8_t,sizeof(Packet)>>(copy);
    std::uint64_t value=UINT64_C(14695981039346656037);
    for(const auto byte:bytes){value^=byte;value*=UINT64_C(1099511628211);}return value;
}
std::string path(const std::string& session,std::uint32_t pid,std::uint64_t birth){return session+"/io-"+std::to_string(pid)+"-"+std::to_string(birth);}
#if !defined(_WIN32)
struct Descriptor {int value{-1};~Descriptor(){if(value>=0)::close(value);}};
std::uint32_t os_error(){return errno==EACCES || errno==EPERM?5U:errno==ENOENT?2U:31U;}
bool lock(int fd,int flags){int status;do{status=::flock(fd,flags);}while(status && errno==EINTR);return !status;}
bool valid_file(int fd){struct stat info{};return !::fstat(fd,&info) && S_ISREG(info.st_mode) && info.st_uid==::geteuid() && !(info.st_mode&077) && info.st_nlink==1 && info.st_size==sizeof(Packet);}
bool read_packet(int fd,Packet& packet){ssize_t size;do{size=::pread(fd,&packet,sizeof(packet),0);}while(size<0 && errno==EINTR);return size==sizeof(packet);}
bool write_packet(int fd,const Packet& packet){ssize_t size;do{size=::pwrite(fd,&packet,sizeof(packet),0);}while(size<0 && errno==EINTR);return size==sizeof(packet);}
#endif
}
// mapped：計數檔的共享映射。發布只寫映射（計數在前、校驗碼在後），不取 flock 也不進核心；
// 讀取端以校驗碼辨識寫入中途的內容並重讀。映射失敗時沿用 flock＋pwrite。
struct Publisher::State {std::string path;int descriptor{-1};Packet packet;std::uint32_t error{};void* mapped{};};
Publisher::Publisher(const std::string& session,std::uint32_t pid,std::uint64_t birth,const Counters& counters):state_(std::make_unique<State>()){
    if(!pid || !birth)throw std::invalid_argument("程序 I/O 發布需要原生出生身分");
    auto& state=*state_;state.path=path(session,pid,birth);state.packet.pid=pid;state.packet.birth=birth;state.packet.counters=counters;state.packet.checksum=checksum(state.packet);
#if !defined(_WIN32)
    Descriptor fd{::open(state.path.c_str(),O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600)};
    const bool created=fd.value>=0;
    if(!created && errno==EEXIST)fd.value=::open(state.path.c_str(),O_RDWR|O_NOFOLLOW|O_CLOEXEC);
    if(fd.value<0 || !lock(fd.value,LOCK_EX))throw std::runtime_error("無法建立程序 I/O 計數檔");
    Packet existing;
    if(!created && (!valid_file(fd.value) || !read_packet(fd.value,existing) || existing.signature!=magic || existing.version!=1 || existing.pid!=pid || existing.birth!=birth || existing.checksum!=checksum(existing)))throw std::runtime_error("程序 I/O 計數檔身分不符");
    if((created && ::ftruncate(fd.value,sizeof(Packet))) || !write_packet(fd.value,state.packet))throw std::runtime_error("無法初始化程序 I/O 計數檔");
    if(!lock(fd.value,LOCK_UN))throw std::runtime_error("無法解除程序 I/O 計數檔鎖");
    state.descriptor=fd.value;fd.value=-1;
    static const bool mapping=[] {const auto* setting=std::getenv("X64ARM_PROCESS_IO_MAP");return !setting || std::string_view(setting)!="0";}();
    if(mapping){const auto view=::mmap(nullptr,sizeof(Packet),PROT_READ|PROT_WRITE,MAP_SHARED,state.descriptor,0);if(view!=MAP_FAILED)state.mapped=view;}
#else
    throw std::runtime_error("此宿主尚未提供程序 I/O 計數分享");
#endif
}
Publisher::~Publisher(){
#if !defined(_WIN32)
    if(state_ && state_->mapped)::munmap(state_->mapped,sizeof(Packet));
    if(state_ && state_->descriptor>=0)::close(state_->descriptor);
#endif
}
void Publisher::publish(const Counters& counters) noexcept {
    auto& state=*state_;if(state.error || counters==state.packet.counters)return;
#if !defined(_WIN32)
    auto packet=state.packet;packet.counters=counters;packet.checksum=checksum(packet);
    if(state.mapped){
        // 校驗碼最後寫入：讀到新計數與舊校驗碼的組合必然不符，讀取端會重讀。
        auto* target=static_cast<unsigned char*>(state.mapped);constexpr auto tail=offsetof(Packet,checksum);
        std::memcpy(target,&packet,tail);std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(target+tail,&packet.checksum,sizeof(packet.checksum));state.packet=packet;return;
    }
    if(!lock(state.descriptor,LOCK_EX))state.error=os_error();
    else {if(!write_packet(state.descriptor,packet))state.error=31;if(!lock(state.descriptor,LOCK_UN))state.error=31;}
    if(state.error){(void)::unlink(state.path.c_str());return;}state.packet=packet;
#else
    (void)counters;state.error=50;
#endif
}
std::uint32_t Publisher::error() const noexcept{return state_->error;}
std::uint32_t Publisher::read(const std::string& session,std::uint32_t pid,std::uint64_t birth,Counters& counters){
#if !defined(_WIN32)
    if(!pid || !birth)return 87;
    Descriptor fd{::open(path(session,pid,birth).c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC)};if(fd.value<0)return os_error();
    if(!lock(fd.value,LOCK_SH))return os_error();
    // 發布端可能正在寫入共享映射：校驗碼不符時短暫讓出後重讀，連續不符才視為損毀。
    Packet packet;if(!valid_file(fd.value))return 13;
    for(unsigned attempt=0;;++attempt){
        if(!read_packet(fd.value,packet))return 13;
        if(packet.checksum==checksum(packet))break;
        if(attempt>=256)return 13;
        ::sched_yield();
    }
    if(packet.signature!=magic || packet.version!=1 || packet.pid!=pid || packet.birth!=birth)return 13;
    if(!lock(fd.value,LOCK_UN))return os_error();counters=packet.counters;return 0;
#else
    (void)session;(void)pid;(void)birth;(void)counters;return 50;
#endif
}
Scope::Scope(Counters& counters,Publisher* publisher) noexcept:counters_(&counters),previous_(current),publisher_(publisher),previous_batch_(batch){current=counters_;if(previous_!=current)batch=nullptr;}
Scope::~Scope(){if(publisher_ && previous_!=counters_)publisher_->publish(*counters_);current=previous_;batch=static_cast<Batch*>(previous_batch_);}
Batch::Batch(Kind kind) noexcept:kind_(kind),counters_(current),previous_(batch){batch=this;}
Batch::~Batch(){batch=previous_;}
void record(Kind kind,std::uint64_t bytes) noexcept {
    if(!current)return;const auto index=static_cast<unsigned>(kind);
    auto* request=batch;while(request && (request->kind_!=kind || request->counters_!=current))request=request->previous_;
    if(!request || !request->recorded_)++current->values[index];
    if(request)request->recorded_=true;current->values[index+3]+=bytes;
}
void record(const Counters& counters) noexcept {
    if(current)for(unsigned i=0;i<6;++i)current->values[i]+=counters.values[i];
}
void progress(Kind kind,std::uint64_t bytes,bool performed,bool& recorded,std::uint64_t& accounted) noexcept {
    if(!current || !performed)return;const auto index=static_cast<unsigned>(kind);
    if(!recorded){++current->values[index];recorded=true;}
    current->values[index+3]+=bytes-accounted;accounted=bytes;
}

}
