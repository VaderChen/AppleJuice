#include "x64arm/shared_file_state.hpp"
#include "x64arm/host_services.hpp"
#include "x64arm/process_debugger.hpp"
#include "x64arm/security_access.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#if defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace x64arm {
namespace {
// 共用範圍鎖加入程序與 Key；保留同一主檔名，混用舊成品時明確拒絕，不能繞過舊鎖表。
constexpr std::uint64_t magic=UINT64_C(0x3346494c45533458);
struct Owner {std::uint32_t pid{},count{};std::uint64_t birth{};};
struct Open {std::uint64_t object{},birth{};std::uint32_t pid{},count{},access{},sharing{},close_target{},disposition_target{};};
struct Target {std::uint64_t parent_device{},parent_inode{},device{},inode{},created{};std::uint32_t pending{},permanent_pending{},directory{};std::array<char,1024> parent{},leaf{};};
struct Header {std::uint64_t signature{},device{},inode{},created{};std::uint32_t owner_count{},open_count{},target_count{},pending{};std::array<Owner,64> owners{};std::array<Open,4096> opens{};std::array<Target,1024> targets{};};
constexpr std::uint64_t range_magic=UINT64_C(0x32524c4645533458);
struct Range {std::uint64_t object{},offset{},length{},birth{};std::uint32_t pid{},key{},exclusive{},reserved{};};
struct RangeHeader {std::uint64_t signature{},device{},inode{},created{};std::uint32_t count{},reserved{};std::array<Range,4096> locks{};};
#if defined(__APPLE__)
std::uint32_t error(){switch(errno){case ENOENT:return 2;case EACCES:case EPERM:case ELOOP:return 5;case EMFILE:case ENFILE:return 4;case ENOMEM:case ENOSPC:return 8;case EBADF:return 6;case ENAMETOOLONG:return 206;default:return 1117;}}
std::uint32_t category(std::uint32_t access){access=security::mapped_access(access,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});return ((access&0x21U)?1U:0U)|((access&6U)?2U:0U)|((access&0x10000U)?4U:0U);}
std::uint64_t created(const struct stat& value){return static_cast<std::uint64_t>(value.st_birthtimespec.tv_sec+INT64_C(11644473600))*UINT64_C(10000000)+static_cast<std::uint64_t>(value.st_birthtimespec.tv_nsec)/100;}
// 自身程序的出生識別在程序期間不變；每次開檔都 sysctl 查詢約占開檔成本四成，改為快取（pid 改變時重查）。
std::pair<std::uint32_t,std::uint64_t> identity(){
    static std::mutex mutex;static std::uint32_t cached_pid{};static std::uint64_t cached_birth{};
    const auto pid=static_cast<std::uint32_t>(::getpid());std::lock_guard<std::mutex> lock(mutex);
    if(pid!=cached_pid || !cached_birth){const auto result=process_debugger::query(pid);cached_pid=pid;cached_birth=result.error||result.exited?0:result.identity;}
    return {pid,cached_birth};
}
bool alive(std::uint32_t pid,std::uint64_t birth){const auto result=process_debugger::query(pid);return !result.error && !result.exited && birth && result.identity==birth;}
void update_pending(Header& h){
    h.pending=0;
    for(std::size_t i=0;i<h.target_count;++i)h.targets[i].pending=h.targets[i].permanent_pending;
    for(std::size_t i=0;i<h.open_count;++i){const auto& entry=h.opens[i];if(entry.count && entry.disposition_target)h.targets[entry.disposition_target-1].pending=1;}
    for(std::size_t i=0;i<h.target_count;++i)h.pending|=h.targets[i].pending;
}
void close_object(Header& h,std::uint64_t object,std::uint32_t close_target,std::uint32_t disposition_target){
    if(std::any_of(h.opens.begin(),h.opens.begin()+h.open_count,[&](const auto& entry){return entry.count && entry.object==object;}))return;
    if(close_target)h.targets[close_target-1].permanent_pending=1;
    if(disposition_target)h.targets[disposition_target-1].permanent_pending=1;
}
void reap(Header& h){
    std::map<std::pair<std::uint32_t,std::uint64_t>,bool> processes;
    const auto live=[&](std::uint32_t pid,std::uint64_t birth){const auto key=std::pair{pid,birth};const auto found=processes.find(key);if(found!=processes.end())return found->second;return processes.emplace(key,alive(pid,birth)).first->second;};
    for(std::size_t i=0;i<h.owner_count;++i){auto& owner=h.owners[i];if(owner.count && !live(owner.pid,owner.birth))owner.count=0;}
    for(std::size_t i=0;i<h.open_count;++i){auto& entry=h.opens[i];if(entry.count && !live(entry.pid,entry.birth)){entry.count=0;close_object(h,entry.object,entry.close_target,entry.disposition_target);}}
    update_pending(h);
}
#endif
}
struct SharedFileState::Storage {
#if defined(__APPLE__)
    int descriptor{-1};Header* data{};std::uint32_t pid{};std::uint64_t birth{};bool registered{};
    int range_directory{-1},range_descriptor{-1};RangeHeader* ranges{};
    std::mutex mutex;std::map<std::uint32_t,int> directories;
    struct Guard {Storage& storage;std::unique_lock<std::mutex> lock;Guard(Storage& s):storage(s),lock(s.mutex){while(::flock(s.descriptor,LOCK_EX)<0)if(errno!=EINTR)throw HostCallFault("無法鎖定共用檔案狀態");}~Guard(){(void)::flock(storage.descriptor,LOCK_UN);}};
    std::uint32_t open_ranges(){
        if(ranges)return 0;
        const auto name="file-ranges-v2-"+std::to_string(data->device)+"-"+std::to_string(data->inode)+"-"+std::to_string(data->created);
        bool fresh=false;
        range_descriptor=::openat(range_directory,name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);fresh=range_descriptor>=0;
        if(!fresh && errno==EEXIST)range_descriptor=::openat(range_directory,name.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
        if(range_descriptor<0)return error();
        struct FreshRange {Storage& storage;const std::string& name;bool fresh,keep{};~FreshRange(){if(keep || !fresh)return;struct stat opened{},entry{};if(!::fstat(storage.range_descriptor,&opened) && !::fstatat(storage.range_directory,name.c_str(),&entry,AT_SYMLINK_NOFOLLOW) && opened.st_dev==entry.st_dev && opened.st_ino==entry.st_ino)(void)::unlinkat(storage.range_directory,name.c_str(),0);}} cleanup{*this,name,fresh};
        struct stat record{};if(::fstat(range_descriptor,&record) || !S_ISREG(record.st_mode) || record.st_nlink!=1 || record.st_uid!=::geteuid() || (record.st_mode&077))return 5;
        if(fresh && ::ftruncate(range_descriptor,sizeof(RangeHeader)))return error();
        if(!fresh && record.st_size!=sizeof(RangeHeader))return 6;
        const auto mapped=::mmap(nullptr,sizeof(RangeHeader),PROT_READ|PROT_WRITE,MAP_SHARED,range_descriptor,0);if(mapped==MAP_FAILED)return 8;
        ranges=static_cast<RangeHeader*>(mapped);
        if(fresh){ranges->signature=range_magic;ranges->device=data->device;ranges->inode=data->inode;ranges->created=data->created;}
        if(ranges->signature!=range_magic || ranges->device!=data->device || ranges->inode!=data->inode || ranges->created!=data->created || ranges->count>ranges->locks.size() || ranges->reserved)return 6;
        for(std::size_t i=0;i<ranges->count;++i){const auto& r=ranges->locks[i];if(!r.object || !r.length || !r.pid || !r.birth || r.offset>INT64_MAX || r.length>UINT64_MAX-r.offset || r.exclusive>1 || r.reserved)return 6;}
        cleanup.keep=true;return 0;
    }
    void clean_ranges(){
        if(!ranges)return;auto count=std::atomic_ref(ranges->count).load(std::memory_order_relaxed);
        for(std::uint32_t i=0;i<count;){const auto& held=ranges->locks[i];
            if(std::any_of(data->opens.begin(),data->opens.begin()+data->open_count,[&](const auto& entry){return entry.count && entry.object==held.object && entry.pid==held.pid && entry.birth==held.birth;})){++i;continue;}
            ranges->locks[i]=ranges->locks[--count];ranges->locks[count]={};
        }
        std::atomic_ref(ranges->count).store(count,std::memory_order_release);
    }
    void refresh(){for(std::uint32_t i=0;i<data->target_count;++i){
        const auto& target=data->targets[i];struct stat parent{};
        // 另一程序可在同一 target index 上改名；不能繼續使用舊目錄描述元。
        if(const auto found=directories.find(i);found!=directories.end()){
            if(!::fstat(found->second,&parent) && target.parent_device==static_cast<std::uint64_t>(parent.st_dev) && target.parent_inode==parent.st_ino)continue;
            ::close(found->second);directories.erase(found);
        }
        const auto fd=::open(target.parent.data(),O_SEARCH|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(fd>=0 && !::fstat(fd,&parent) && target.parent_device==static_cast<std::uint64_t>(parent.st_dev) && target.parent_inode==parent.st_ino){
            try{directories.emplace(i,fd);}catch(...){::close(fd);throw;}
        }else if(fd>=0)::close(fd);
    }}
    void cleanup(){
        auto& h=*data;if(!h.pending || std::any_of(h.owners.begin(),h.owners.begin()+h.owner_count,[](const auto& v){return v.count;}) || std::any_of(h.opens.begin(),h.opens.begin()+h.open_count,[](const auto& v){return v.count;}))return;
        refresh();for(std::uint32_t i=0;i<h.target_count;++i){auto& target=h.targets[i];const auto found=directories.find(i);if(!target.pending || found==directories.end())continue;struct stat file{};
            const auto queried=::fstatat(found->second,target.leaf.data(),&file,AT_SYMLINK_NOFOLLOW);
            const bool replaced=!queried && (target.device!=static_cast<std::uint64_t>(file.st_dev) || target.inode!=file.st_ino || target.created!=created(file));
            const bool removed=queried?(errno==ENOENT):replaced || !::unlinkat(found->second,target.leaf.data(),target.directory?AT_REMOVEDIR:0);
            // 刪除完成的是這個名稱；清除已完成的 pending，其他硬連結仍可重新開啟或刪除。
            // 真正的權限／I/O 失敗保留 pending，不把未完成的刪除宣稱為成功。
            if(removed)target.pending=target.permanent_pending=0;
        }
        update_pending(h);
    }
    ~Storage(){
        if(registered && data)try{Guard guard(*this);reap(*data);for(std::size_t i=0;i<data->owner_count;++i){auto& owner=data->owners[i];if(owner.pid==pid && owner.birth==birth && owner.count){--owner.count;break;}}cleanup();}catch(...){}
        for(const auto& [index,fd]:directories){(void)index;::close(fd);}if(ranges)::munmap(ranges,sizeof(RangeHeader));if(range_descriptor>=0)::close(range_descriptor);if(range_directory>=0)::close(range_directory);if(data)::munmap(data,sizeof(Header));if(descriptor>=0)::close(descriptor);
    }
#endif
};
SharedFileState::SharedFileState(std::unique_ptr<Storage> storage):storage_(std::move(storage)){}
SharedFileState::~SharedFileState()=default;
std::pair<std::shared_ptr<SharedFileState>,std::uint32_t> SharedFileState::open(const std::string& session,const FileMetadata& info,bool create){
#if !defined(__APPLE__)
    (void)session;(void)info;(void)create;return {{},50};
#else
    if(session.empty())return {{},0};auto storage=std::make_unique<Storage>();
    struct Fd {int value;~Fd(){if(value>=0)::close(value);}} directory{::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};struct stat root{};
    if(directory.value<0 || ::fstat(directory.value,&root) || root.st_uid!=::geteuid() || (root.st_mode&077))return {{},5};
    // 同一命名空間的建立／載入序列化，接收端不會讀到半初始化的 record。
    struct DirectoryGuard {int fd;explicit DirectoryGuard(int value):fd(value){while(::flock(fd,LOCK_EX)<0)if(errno!=EINTR)throw HostCallFault("無法鎖定檔案共享命名空間");}~DirectoryGuard(){(void)::flock(fd,LOCK_UN);}} directory_guard(directory.value);
    const auto name="file-lifetime-"+std::to_string(info.device)+"-"+std::to_string(info.id);bool fresh=false;
    struct FreshCleanup {int directory;const std::string& name;const std::unique_ptr<Storage>& storage;const bool& fresh;bool keep{};~FreshCleanup(){if(keep || !fresh || !storage)return;struct stat opened{},entry{};if(!::fstat(storage->descriptor,&opened) && !::fstatat(directory,name.c_str(),&entry,AT_SYMLINK_NOFOLLOW) && opened.st_dev==entry.st_dev && opened.st_ino==entry.st_ino)(void)::unlinkat(directory,name.c_str(),0);}} fresh_cleanup{directory.value,name,storage,fresh};
    if(create){storage->descriptor=::openat(directory.value,name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);fresh=storage->descriptor>=0;if(!fresh && errno!=EEXIST)return {{},error()};}
    if(storage->descriptor<0)storage->descriptor=::openat(directory.value,name.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(storage->descriptor<0)return !create && errno==ENOENT?std::pair<std::shared_ptr<SharedFileState>,std::uint32_t>{{},0}:std::pair<std::shared_ptr<SharedFileState>,std::uint32_t>{{},error()};
    const auto [pid,birth]=identity();if(!birth)return {{},50};storage->pid=pid;storage->birth=birth;
    Storage::Guard guard(*storage);struct stat record{};
    storage->range_directory=::fcntl(directory.value,F_DUPFD_CLOEXEC,0);if(storage->range_directory<0)return {{},error()};
    if(::fstat(storage->descriptor,&record) || !S_ISREG(record.st_mode) || record.st_uid!=::geteuid() || (record.st_mode&077))return {{},5};
    if(fresh && ::ftruncate(storage->descriptor,sizeof(Header)))return {{},error()};
    if(!fresh && record.st_size!=sizeof(Header))return {{},6};const auto mapped=::mmap(nullptr,sizeof(Header),PROT_READ|PROT_WRITE,MAP_SHARED,storage->descriptor,0);if(mapped==MAP_FAILED)return {{},8};storage->data=static_cast<Header*>(mapped);auto& h=*storage->data;
    if(!fresh && (h.signature!=magic || h.device!=info.device || h.inode!=info.id || h.owner_count>h.owners.size() || h.open_count>h.opens.size() || h.target_count>h.targets.size() || h.pending>1))return {{},6};
    if(!fresh){for(std::size_t i=0;i<h.open_count;++i)if(h.opens[i].sharing>7 || h.opens[i].close_target>h.target_count || h.opens[i].disposition_target>h.target_count)return {{},6};for(std::size_t i=0;i<h.target_count;++i){const auto& t=h.targets[i];if(t.pending>1 || t.permanent_pending>1 || t.directory>1 || t.parent[0]!='/' || !std::memchr(t.parent.data(),0,t.parent.size()) || !std::memchr(t.leaf.data(),0,t.leaf.size()) || !t.leaf[0] || std::strcmp(t.leaf.data(),".")==0 || std::strcmp(t.leaf.data(),"..")==0 || std::strchr(t.leaf.data(),'/'))return {{},6};}reap(h);storage->cleanup();}
    if(fresh || h.created!=info.created){if(!fresh && (std::any_of(h.owners.begin(),h.owners.begin()+h.owner_count,[](const auto& v){return v.count;}) || std::any_of(h.opens.begin(),h.opens.begin()+h.open_count,[](const auto& v){return v.count;})))return {{},6};if(!fresh)std::memset(mapped,0,sizeof(Header));h.signature=magic;h.device=info.device;h.inode=info.id;h.created=info.created;}
    Owner* owner=nullptr;for(std::size_t i=0;i<h.owner_count;++i)if(h.owners[i].pid==pid && h.owners[i].birth==birth){owner=&h.owners[i];break;}
    if(!owner)for(std::size_t i=0;i<h.owner_count;++i)if(!h.owners[i].count){owner=&h.owners[i];break;}
    if(!owner){if(h.owner_count==h.owners.size())return {{},8};owner=&h.owners[h.owner_count++];}
    if(owner->count==UINT32_MAX)return {{},8};*owner={pid,owner->count+1,birth};storage->registered=true;storage->refresh();
    if(const auto code=storage->open_ranges())return {{},code};storage->clean_ranges();
    fresh_cleanup.keep=true;return {std::shared_ptr<SharedFileState>(new SharedFileState(std::move(storage))),0};
#endif
}
std::uint32_t SharedFileState::sharing(std::uint32_t access,std::uint32_t sharing,std::uint64_t ignore,bool allow_pending){
#if !defined(__APPLE__)
    (void)access;(void)sharing;(void)ignore;(void)allow_pending;return 50;
#else
    Storage::Guard guard(*storage_);auto& h=*storage_->data;reap(h);if(h.pending && !allow_pending)return 5;
    for(std::size_t i=0;i<h.open_count;++i){const auto& v=h.opens[i];if(v.count && v.object!=ignore && ((category(access)&~v.sharing) || (category(v.access)&~sharing)))return 32;}return 0;
#endif
}
bool SharedFileState::pending(){
#if !defined(__APPLE__)
    return false;
#else
    Storage::Guard guard(*storage_);reap(*storage_->data);return storage_->data->pending;
#endif
}
std::pair<std::shared_ptr<SharedFileLease>,std::uint32_t> SharedFileState::acquire(std::uint64_t object,std::uint32_t access,std::uint32_t sharing,bool existing,std::uint32_t close_target){
#if !defined(__APPLE__)
    (void)object;(void)access;(void)sharing;(void)existing;(void)close_target;return {{},50};
#else
    if(sharing>7)return {{},87};Storage::Guard guard(*storage_);auto& h=*storage_->data;reap(h);if(close_target>h.target_count)return {{},6};
    std::uint32_t disposition_target{};const Open* original=nullptr;if(object)for(std::size_t i=0;i<h.open_count;++i)if(h.opens[i].count && h.opens[i].object==object){original=&h.opens[i];break;}
    if(object && !original)return {{},6};
    if(original){access=original->access;sharing=original->sharing;close_target=original->close_target;disposition_target=original->disposition_target;}
    else if(!existing){if(h.pending)return {{},5};for(std::size_t i=0;i<h.open_count;++i){const auto& v=h.opens[i];if(v.count && ((category(access)&~v.sharing) || (category(v.access)&~sharing)))return {{},32};}}
    static std::atomic<std::uint32_t> sequence{1};if(!object)do{object=(static_cast<std::uint64_t>(storage_->pid)<<32)|sequence.fetch_add(1);}while(std::any_of(h.opens.begin(),h.opens.begin()+h.open_count,[&](const auto& v){return v.count && v.object==object;}));
    Open* slot=nullptr;for(std::size_t i=0;i<h.open_count;++i)if(h.opens[i].count && h.opens[i].object==object && h.opens[i].pid==storage_->pid && h.opens[i].birth==storage_->birth){slot=&h.opens[i];break;}
    if(!slot)for(std::size_t i=0;i<h.open_count;++i)if(!h.opens[i].count){slot=&h.opens[i];break;}
    if(!slot){if(h.open_count==h.opens.size())return {{},8};slot=&h.opens[h.open_count++];}
    const auto count=slot->count;if(count==UINT32_MAX)return {{},8};auto lease=std::shared_ptr<SharedFileLease>(new SharedFileLease(shared_from_this(),object));*slot={object,storage_->birth,storage_->pid,count+1,access,sharing,close_target,disposition_target};return {std::move(lease),0};
#endif
}
void SharedFileState::release(std::uint64_t object){
#if defined(__APPLE__)
    Storage::Guard guard(*storage_);auto& h=*storage_->data;reap(h);for(std::size_t i=0;i<h.open_count;++i){auto& v=h.opens[i];if(v.count && v.object==object && v.pid==storage_->pid && v.birth==storage_->birth){--v.count;close_object(h,object,v.close_target,v.disposition_target);update_pending(h);break;}}
    storage_->clean_ranges();
#else
    (void)object;
#endif
}
std::uint32_t SharedFileState::range_conflict(std::uint64_t object,std::uint64_t offset,std::uint64_t length,bool writing,std::uint32_t key){
#if !defined(__APPLE__)
    (void)object;(void)offset;(void)length;(void)writing;(void)key;return 50;
#else
    if(!length || !std::atomic_ref(storage_->ranges->count).load(std::memory_order_acquire))return 0;
    Storage::Guard guard(*storage_);
    const auto conflict=[&](){for(std::size_t i=0;i<storage_->ranges->count;++i){const auto& r=storage_->ranges->locks[i];
        const bool own=r.object==object && r.pid==storage_->pid && r.birth==storage_->birth && r.key==key;
        if(offset<r.offset+r.length && r.offset<offset+length && (own?writing && !r.exclusive:writing || r.exclusive))return true;
    }return false;};
    if(!conflict())return 0;reap(*storage_->data);storage_->clean_ranges();return conflict()?33U:0U;
#endif
}
std::uint32_t SharedFileState::mutate_size(const std::function<std::uint32_t()>& operation){
#if defined(__APPLE__)
    Storage::Guard guard(*storage_);
#endif
    return operation();
}
std::uint32_t SharedFileState::range_lock(std::uint64_t object,std::uint64_t offset,std::uint64_t length,bool exclusive,bool unlock,const std::function<std::uint32_t()>& native,bool adopt,std::uint32_t key){
#if !defined(__APPLE__)
    (void)object;(void)offset;(void)length;(void)exclusive;(void)unlock;(void)native;(void)adopt;(void)key;return 50;
#else
    if(!object || !length || offset>INT64_MAX || length>UINT64_MAX-offset)return 87;
    Storage::Guard guard(*storage_);reap(*storage_->data);storage_->clean_ranges();auto& h=*storage_->ranges;
    const auto own=[&](const Range& r){return r.object==object && r.pid==storage_->pid && r.birth==storage_->birth && r.key==key;};
    if(!unlock){
        for(std::size_t i=0;i<h.count;++i){const auto& r=h.locks[i];
            if(adopt && own(r) && r.offset==offset && r.length==length && r.exclusive==static_cast<unsigned>(exclusive))return 0;
            if(offset<r.offset+r.length && r.offset<offset+length && (exclusive || (r.exclusive && !own(r))))return 33;
        }
        if(h.count==h.locks.size())return 8;if(const auto code=native())return code;
        const auto count=h.count;h.locks[count]={object,offset,length,storage_->birth,storage_->pid,key,exclusive?1U:0U,0};std::atomic_ref(h.count).store(count+1,std::memory_order_release);return 0;
    }
    std::size_t index=h.count;for(std::size_t i=0;i<h.count;++i)if(own(h.locks[i]) && h.locks[i].offset==offset && h.locks[i].length==length){index=i;if(h.locks[i].exclusive)break;}
    if(index==h.count)return 158;if(const auto code=native())return code;const auto count=h.count-1;
    h.locks[index]=h.locks[count];h.locks[count]={};std::atomic_ref(h.count).store(count,std::memory_order_release);return 0;
#endif
}
SharedFileLease::~SharedFileLease(){try{state_->release(identity_);}catch(...) {}}
std::pair<std::uint32_t,std::uint32_t> SharedFileState::target(int descriptor,std::string_view leaf,const FileMetadata& info,bool directory){
#if !defined(__APPLE__)
    (void)descriptor;(void)leaf;(void)info;(void)directory;return {0,50};
#else
    if(leaf.empty() || leaf=="." || leaf==".." || leaf.find('/')!=leaf.npos || leaf.find('\0')!=leaf.npos || leaf.size()>=Target{}.leaf.size())return {0,87};std::array<char,1024> path{};struct stat parent{},file{};
    if(::fstat(descriptor,&parent) || !S_ISDIR(parent.st_mode) || ::fcntl(descriptor,F_GETPATH,path.data()))return {0,error()};
    (void)file;Storage::Guard guard(*storage_);auto& h=*storage_->data;if(info.device!=h.device || info.id!=h.inode || info.created!=h.created)return {0,6};std::uint32_t index=h.target_count;
    for(std::uint32_t i=0;i<h.target_count;++i){const auto& t=h.targets[i];if(t.parent_device==static_cast<std::uint64_t>(parent.st_dev) && t.parent_inode==parent.st_ino && std::string_view(t.leaf.data())==leaf){index=i;break;}}
    if(index==h.targets.size())return {0,8};const auto copy=::fcntl(descriptor,F_DUPFD_CLOEXEC,3);if(copy<0)return {0,error()};
    auto& t=h.targets[index];t.parent_device=static_cast<std::uint64_t>(parent.st_dev);t.parent_inode=parent.st_ino;t.device=info.device;t.inode=info.id;t.created=info.created;t.directory=directory;std::copy(path.begin(),path.end(),t.parent.begin());t.leaf.fill(0);std::copy(leaf.begin(),leaf.end(),t.leaf.begin());
    if(index==h.target_count)++h.target_count;const auto old=storage_->directories.find(index);if(old!=storage_->directories.end()){::close(old->second);old->second=copy;}else storage_->directories.emplace(index,copy);return {index+1,0};
#endif
}
std::uint32_t SharedFileState::relocate_targets(int source,std::string_view from,int destination,std::string_view to,const FileMetadata& info,const std::function<std::uint32_t()>& operation){
#if !defined(__APPLE__)
    (void)source;(void)from;(void)destination;(void)to;(void)info;return operation();
#else
    Storage::Guard guard(*storage_);auto& h=*storage_->data;
    if(info.device!=h.device || info.id!=h.inode || info.created!=h.created)return 6;
    struct stat parent{},next_parent{};if(::fstat(source,&parent))return error();
    bool matched=false;
    const auto matches=[&](const Target& target){return target.parent_device==static_cast<std::uint64_t>(parent.st_dev) && target.parent_inode==parent.st_ino && std::string_view(target.leaf.data())==from;};
    for(std::uint32_t i=0;i<h.target_count;++i)matched|=matches(h.targets[i]);
    if(!matched)return operation();
    if(to.empty() || to=="." || to==".." || to.find('/')!=to.npos || to.find('\0')!=to.npos || to.size()>=Target{}.leaf.size())return 87;
    std::array<char,1024> path{};
    if(::fstat(destination,&next_parent) || !S_ISDIR(next_parent.st_mode) || ::fcntl(destination,F_GETPATH,path.data()))return error();
    // 在更動檔案前完成所有配置。沿用原 target index，其他程序的 close／
    // disposition 引用與 pending 旗標不變，亦不因反覆改名增加紀錄。
    struct Prepared {std::map<std::uint32_t,int> values;~Prepared(){for(const auto& [index,fd]:values){(void)index;::close(fd);}}} prepared;
    for(std::uint32_t i=0;i<h.target_count;++i)if(matches(h.targets[i])){
        const auto fd=::fcntl(destination,F_DUPFD_CLOEXEC,3);if(fd<0)return error();
        try{prepared.values.emplace(i,fd);}catch(...){::close(fd);throw;}
    }
    if(const auto code=operation())return code;
    for(const auto& [index,fd]:prepared.values){
        (void)fd;auto& target=h.targets[index];target.parent_device=static_cast<std::uint64_t>(next_parent.st_dev);target.parent_inode=next_parent.st_ino;
        target.parent=path;target.leaf.fill(0);std::copy(to.begin(),to.end(),target.leaf.begin());
        if(const auto old=storage_->directories.find(index);old!=storage_->directories.end()){::close(old->second);storage_->directories.erase(old);}
    }
    storage_->directories.merge(prepared.values);return 0;
#endif
}
std::uint32_t SharedFileState::disposition(std::uint32_t index,bool pending,std::uint64_t object){
#if !defined(__APPLE__)
    (void)index;(void)pending;(void)object;return 50;
#else
    Storage::Guard guard(*storage_);auto& h=*storage_->data;reap(h);
    if(index>h.target_count || (pending && !index))return 6;
    if(object){
        bool found=false;
        for(std::size_t i=0;i<h.open_count;++i){auto& entry=h.opens[i];if(entry.count && entry.object==object){entry.disposition_target=pending?index:0;found=true;}}
        if(!found)return 6;
    }else{if(!index)return 6;h.targets[index-1].permanent_pending=pending;}
    update_pending(h);return 0;
#endif
}
std::uint32_t SharedFileState::close_target(std::uint64_t object,std::uint32_t target){
#if !defined(__APPLE__)
    (void)object;(void)target;return 50;
#else
    Storage::Guard guard(*storage_);auto& h=*storage_->data;if(target>h.target_count)return 6;bool found=false;for(std::size_t i=0;i<h.open_count;++i){auto& entry=h.opens[i];if(entry.count && entry.object==object){entry.close_target=target;found=true;}}return found?0U:6U;
#endif
}
}
