#include "perflib_catalog.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/shared_event.hpp"
#include "x64arm/shared_wake.hpp"
#include <atomic>
#include <charconv>
#include <cerrno>
#include <mutex>
#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm::perflib {
bool registration_compatible(const CounterSet& first,const CounterSet& second){
    if(first.id!=second.id || first.provider!=second.provider || first.instance_type!=second.instance_type || first.counters.size()!=second.counters.size())return false;
    std::map<std::uint32_t,Counter> counters;
    for(auto value:second.counters){value.offset=0;if(!counters.emplace(value.id,value).second)return false;}
    for(auto value:first.counters){value.offset=0;const auto found=counters.find(value.id);if(found==counters.end() || found->second!=value)return false;counters.erase(found);}
    return counters.empty();
}
namespace {
constexpr std::size_t maximum_bytes=16U*1024U*1024U,maximum_records=4096;
constexpr std::string_view prefix="perflib-provider-";
constexpr std::array<std::uint8_t,8> signature{'X','6','4','P','E','R','F','1'};
std::atomic<std::uint64_t> next_ordinal{1};
std::uint64_t ordinal(){const auto value=next_ordinal.fetch_add(1);if(!value || value==UINT64_MAX)throw Error(8);return value;}
Bytes encode_set(const CounterSet& set){
    if(set.counters.size()>65536)throw Error(8);
    Bytes bytes(40+32*set.counters.size());put_guid(bytes,0,set.id);put_guid(bytes,16,set.provider);
    put(bytes,32,set.counters.size());put(bytes,36,set.instance_type);
    for(std::size_t i=0;i<set.counters.size();++i){const auto& value=set.counters[i];const auto at=40+i*32;
        put(bytes,at,value.id);put(bytes,at+4,value.type);put(bytes,at+8,value.attributes,8);put(bytes,at+16,value.size);
        put(bytes,at+20,value.detail);put(bytes,at+24,static_cast<std::uint32_t>(value.scale));put(bytes,at+28,value.offset);
    }return bytes;
}
Bytes encode(const CatalogEntry& entry){
    if(!entry.origin || !entry.publication || !entry.handle || entry.sets.size()>1024 || (entry.pointer_bytes!=4 && entry.pointer_bytes!=8))throw Error(87);
    Bytes bytes(72);std::copy(signature.begin(),signature.end(),bytes.begin());put(bytes,12,1);put(bytes,16,entry.origin.pid);
    put(bytes,20,entry.sets.size());put(bytes,24,entry.origin.birth,8);put(bytes,32,entry.publication,8);put(bytes,40,entry.handle,8);
    put_guid(bytes,48,entry.provider);put(bytes,64,entry.pointer_bytes);
    for(const auto& [id,set]:entry.sets){
        if(id!=set.id || set.provider!=entry.provider)throw Error(87);
        const auto payload=encode_set(set);(void)counter_set(payload,entry.pointer_bytes);
        const auto at=bytes.size();bytes.resize(at+8);put(bytes,at,payload.size());append(bytes,payload);
    }
    put(bytes,8,bytes.size());return bytes;
}
CatalogEntry decode(std::span<const std::uint8_t> bytes){
    Reader reader{bytes};reader.check(0,72);
    if(!std::equal(signature.begin(),signature.end(),bytes.begin()) || reader.u32(8)!=bytes.size() || reader.u32(12)!=1 || reader.u32(68))throw Error(13);
    CatalogEntry result;result.origin={reader.u32(16),reader.integer(24,8)};result.publication=reader.integer(32,8);
    result.handle=reader.integer(40,8);result.provider=reader.guid(48);result.pointer_bytes=reader.u32(64);
    const auto count=reader.u32(20);if(!result.origin || !result.publication || !result.handle || count>1024 || (result.pointer_bytes!=4 && result.pointer_bytes!=8))throw Error(13);
    std::size_t at=72;
    for(std::uint32_t i=0;i<count;++i){reader.check(at,8);const auto size=reader.u32(at);if(reader.u32(at+4))throw Error(13);at+=8;reader.check(at,size);
        auto set=counter_set(bytes.subspan(at,size),result.pointer_bytes);
        if(set.provider!=result.provider || !result.sets.emplace(set.id,std::move(set)).second)throw Error(13);at+=size;
    }
    if(at!=bytes.size())throw Error(13);return result;
}
std::string name(const CatalogEntry& value){return std::string(prefix)+std::to_string(value.origin.pid)+"-"+std::to_string(value.origin.birth)+"-"+std::to_string(value.publication);}
bool parse_name(std::string_view text,CatalogEntry& value){
    if(!text.starts_with(prefix))return false;text.remove_prefix(prefix.size());
    std::array<std::uint64_t,3> parts{};
    for(unsigned i=0;i<parts.size();++i){const auto end=text.find('-');const auto part=text.substr(0,end);
        const auto parsed=std::from_chars(part.data(),part.data()+part.size(),parts[i]);
        if(part.empty() || parsed.ec!=std::errc{} || parsed.ptr!=part.data()+part.size() || !parts[i] || (i<2 && end==text.npos) || (i==2 && end!=text.npos))return false;
        if(i<2)text.remove_prefix(end+1);
    }
    if(parts[0]>UINT32_MAX)return false;value.origin={static_cast<std::uint32_t>(parts[0]),parts[1]};value.publication=parts[2];return true;
}
#if !defined(_WIN32)
struct File {int value{-1};~File(){if(value>=0)::close(value);}};
bool safe_file(int file,struct stat& info){return file>=0 && !::fstat(file,&info) && S_ISREG(info.st_mode) && info.st_uid==::geteuid() && !(info.st_mode&077) && info.st_nlink==1;}
std::uint32_t io_error(){return errno==EACCES || errno==EPERM?5:errno==ENOMEM || errno==ENOSPC?8:13;}
#endif
}
struct CatalogDirectory {
    int file{-1};std::shared_ptr<SharedWake> wake;
    ~CatalogDirectory(){
#if !defined(_WIN32)
        if(file>=0)::close(file);
#endif
    }
};
struct ProviderPublication::Storage {
    std::shared_ptr<CatalogDirectory> directory;
    CatalogEntry entry;Bytes security;std::string filename;int file{-1};std::mutex mutex;
    ~Storage(){
#if !defined(_WIN32)
        if(file<0)return;
        // 只移除本物件最後發佈的 inode，不根據名稱清理其他 publication。
        try{SharedEventLock lock(directory->file);struct stat held{},current{};
            if(!::fstat(file,&held) && !::fstatat(directory->file,filename.c_str(),&current,AT_SYMLINK_NOFOLLOW) &&
               held.st_dev==current.st_dev && held.st_ino==current.st_ino)(void)::unlinkat(directory->file,filename.c_str(),0);
        }catch(...){} // 關閉 descriptor 仍會解除擁有者 lease，讀者將忽略留下的檔案。
        ::close(file);if(directory->wake)directory->wake->notify();
#endif
    }
};
ProviderPublication::ProviderPublication(std::unique_ptr<Storage> value):storage_(std::move(value)){}
ProviderPublication::~ProviderPublication()=default;
std::uint64_t ProviderPublication::publication()const{
    std::lock_guard guard(storage_->mutex);return storage_->entry.publication;
}
ProviderCatalog::ProviderCatalog(std::shared_ptr<CatalogDirectory> value):directory_(std::move(value)){}
std::shared_ptr<ProviderCatalog> ProviderCatalog::open(const std::string& session){
#if defined(_WIN32)
    (void)session;return {};
#else
    auto directory=std::make_shared<CatalogDirectory>();directory->file=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat info{};
    if(directory->file<0 || ::fstat(directory->file,&info) || !S_ISDIR(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077))throw Error(5);
    directory->wake=SharedWake::open(session);return std::shared_ptr<ProviderCatalog>(new ProviderCatalog(std::move(directory)));
#endif
}
std::shared_ptr<ProviderPublication> ProviderCatalog::publish(std::uint64_t handle,Guid provider,unsigned pointer_bytes,
    const std::map<Guid,CounterSet>& sets,const Bytes& descriptor){
#if defined(_WIN32)
    (void)handle;(void)provider;(void)pointer_bytes;(void)sets;(void)descriptor;throw Error(50);
#else
    // 登錄的擁有者是實際承載引擎的宿主程序，不依賴尚未設定的客體 PID／record。
    const auto self=process_identity::query(static_cast<std::uint32_t>(::getpid()));
    if(self.error || self.exited || !self.identity || descriptor.empty())throw Error(5);
    const auto origin=self.identity;
    (void)security::parse_relative_descriptor(descriptor);
    auto storage=std::make_unique<ProviderPublication::Storage>();storage->directory=directory_;storage->security=descriptor;
    storage->entry={origin,ordinal(),handle,pointer_bytes,provider,{}};storage->filename=name(storage->entry);
    auto result=std::shared_ptr<ProviderPublication>(new ProviderPublication(std::move(storage)));result->update(sets);return result;
#endif
}
void ProviderPublication::update(const std::map<Guid,CounterSet>& sets){
#if defined(_WIN32)
    (void)sets;throw Error(50);
#else
    std::lock_guard guard(storage_->mutex);auto next=storage_->entry;next.sets=sets;const auto bytes=encode(next);
    const auto temporary="perflib-pending-"+std::to_string(next.origin.pid)+"-"+std::to_string(next.origin.birth)+"-"+std::to_string(ordinal());
    struct Pending {
        int directory;std::string name;dev_t device{};ino_t inode{};bool owned{};
        ~Pending(){struct stat info{};if(owned && !name.empty() && !::fstatat(directory,name.c_str(),&info,AT_SYMLINK_NOFOLLOW) &&
            info.st_dev==device && info.st_ino==inode)(void)::unlinkat(directory,name.c_str(),0);}
    } pending{storage_->directory->file,temporary};
    File file{::openat(pending.directory,pending.name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600)};
    struct stat info{};if(!safe_file(file.value,info))throw Error(io_error());
    pending.device=info.st_dev;pending.inode=info.st_ino;pending.owned=true;
    // 擁有者持有 exclusive lease；讀者只嘗試 shared lease，不持鎖讀取不可變資料。
    while(::flock(file.value,LOCK_EX)<0)if(errno!=EINTR)throw Error(io_error());
    set_shared_object_security(file.value,storage_->security);
    std::size_t at=0;
    while(at<bytes.size()){
        const auto written=::write(file.value,bytes.data()+at,bytes.size()-at);
        if(written<0 && errno==EINTR)continue;if(written<=0)throw Error(io_error());at+=static_cast<std::size_t>(written);
    }
    SharedEventLock lock(storage_->directory->file);
    struct stat current{},held{};
    if(storage_->file<0){
        if(!::fstatat(pending.directory,storage_->filename.c_str(),&current,AT_SYMLINK_NOFOLLOW))throw Error(183);
        if(errno!=ENOENT)throw Error(io_error());
    }else if(::fstat(storage_->file,&held) || ::fstatat(pending.directory,storage_->filename.c_str(),&current,AT_SYMLINK_NOFOLLOW) ||
             held.st_dev!=current.st_dev || held.st_ino!=current.st_ino)throw Error(6);
    if(::renameat(pending.directory,pending.name.c_str(),pending.directory,storage_->filename.c_str()))throw Error(io_error());
    pending.name.clear();const auto old=storage_->file;storage_->file=file.value;file.value=-1;storage_->entry=std::move(next);
    if(old>=0)::close(old);if(storage_->directory->wake)storage_->directory->wake->notify();
#endif
}
std::vector<CatalogEntry> ProviderCatalog::snapshot(const AccessCheck& check)const{
#if defined(_WIN32)
    (void)check;return {};
#else
    if(!check)throw Error(87);
    const auto descriptor=::openat(directory_->file,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if(descriptor<0)throw Error(io_error());
    struct Listing {DIR* value;~Listing(){if(value)::closedir(value);}} listing{::fdopendir(descriptor)};
    if(!listing.value){const auto error=io_error();::close(descriptor);throw Error(error);}
    std::vector<CatalogEntry> result;std::map<std::uint32_t,process_identity::Status> processes;std::size_t candidates=0,total=0;
    for(;;){errno=0;const auto item=::readdir(listing.value);if(!item){if(errno)throw Error(io_error());break;}
        CatalogEntry expected;if(!parse_name(item->d_name,expected))continue;if(++candidates>maximum_records)throw Error(8);
        auto found=processes.find(expected.origin.pid);if(found==processes.end())found=processes.emplace(expected.origin.pid,process_identity::query(expected.origin.pid)).first;
        if(!found->second.live(expected.origin))continue;
        File file{::openat(directory_->file,item->d_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};struct stat info{};
        if(!safe_file(file.value,info) || info.st_size<72 || info.st_size>static_cast<off_t>(maximum_bytes))continue;
        int lease;
        do{lease=::flock(file.value,LOCK_SH|LOCK_NB);}while(lease<0 && errno==EINTR);
        if(!lease){(void)::flock(file.value,LOCK_UN);continue;}
        if(errno!=EWOULDBLOCK && errno!=EAGAIN)continue;
        const auto sd=shared_object_security(file.value);if(sd.empty())continue;
        (void)security::parse_relative_descriptor(sd);if(!check(sd))continue;
        const auto size=static_cast<std::size_t>(info.st_size);if(size>64U*1024U*1024U-total)throw Error(8);
        Bytes bytes(size);std::size_t at=0;
        while(at<size){const auto amount=::pread(file.value,bytes.data()+at,size-at,static_cast<off_t>(at));if(amount<0 && errno==EINTR)continue;if(amount<=0)break;at+=static_cast<std::size_t>(amount);}
        if(at!=size)continue;
        try{auto value=decode(bytes);if(value.origin!=expected.origin || value.publication!=expected.publication)continue;total+=size;result.push_back(std::move(value));}
        catch(const Error&){continue;}
    }
    return result;
#endif
}
}
