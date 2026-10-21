#include "x64arm/file_volumes.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <tuple>
#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <sys/xattr.h>
#endif

namespace x64arm {
namespace {
bool letter(char c){return c>='A' && c<='Z';}
[[maybe_unused]] std::string lower(std::string value){for(auto& c:value)if(c>='A' && c<='Z')c=static_cast<char>(c+32);return value;}
bool relative_path(std::string_view path){
    if(path.empty() || path.size()>32767 || path.front()=='/' || path.back()=='/' || path.find_first_of(":\\")!=path.npos || path.find('\0')!=path.npos)return false;
    for(std::size_t at=0;at<path.size();){auto end=path.find('/',at);if(end==path.npos)end=path.size();const auto part=path.substr(at,end-at);if(part.empty() || part=="." || part=="..")return false;at=end+1;}return true;
}
#if !defined(_WIN32)
struct Fd {
    int value{-1};
    ~Fd(){if(value>=0)::close(value);}
    Fd()=default;
    explicit Fd(int fd):value(fd){}
    Fd(const Fd&)=delete;
    Fd& operator=(const Fd&)=delete;
};
std::uint32_t error(){switch(errno){case ENOENT:return 2;case ENOTDIR:return 3;case EACCES:case EPERM:case ELOOP:return 5;case EINVAL:return 87;case ENOTSUP:return 50;case ENOMEM:return 8;case ENOSPC:case EDQUOT:return 112;case ENAMETOOLONG:return 206;case EEXIST:return 183;case EBADF:return 6;default:return 1117;}}
#endif
#if defined(__APPLE__)
constexpr auto namespace_attribute="com.x64arm.volumes";
constexpr auto mount_attribute="com.x64arm.volume-mount";
constexpr std::array<std::uint8_t,8> namespace_magic{'x','6','4','v','o','l',1,0};
constexpr std::array<std::uint8_t,8> mount_magic{'x','6','4','m','n','t',1,0};
constexpr std::size_t max_bytes=65536,max_mounts=1024;
void put(std::vector<std::uint8_t>& bytes,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));}
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    std::span<const std::uint8_t> take(std::size_t size){if(size>bytes.size()-at)throw std::invalid_argument("磁碟中繼資料截斷");const auto part=bytes.subspan(at,size);at+=size;return part;}
    std::uint64_t get(unsigned width){const auto part=take(width);std::uint64_t result=0;for(unsigned i=0;i<width;++i)result|=std::uint64_t{part[i]}<<(8*i);return result;}
};
std::pair<std::vector<std::uint8_t>,std::uint32_t> attribute(int descriptor,const char* name){
    const auto size=::fgetxattr(descriptor,name,nullptr,0,0,0);
    if(size<0)return {{},errno==ENOATTR?4390U:error()};
    if(size<=0 || static_cast<std::size_t>(size)>max_bytes)return {{},13};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if(::fgetxattr(descriptor,name,bytes.data(),bytes.size(),0,0)!=size)return {{},13};return {std::move(bytes),0};
}
// 同一複本以 mutex 串行化；不同服務／程序以原生 flock 共享持久狀態。
struct Lock {
    std::unique_lock<std::mutex> mutex;
    int descriptor{-1};
    std::uint32_t error{};
    Lock(std::mutex& guard,int fd,bool writing):mutex(guard),descriptor(fd){if(fd<0)return;while(::flock(fd,writing?LOCK_EX:LOCK_SH)<0){if(errno==EINTR)continue;error=x64arm::error();descriptor=-1;break;}}
    ~Lock(){if(descriptor>=0)(void)::flock(descriptor,LOCK_UN);}
};
#endif
}

struct FileVolumes::Impl {
    struct Root {
        FileVolume volume;
#if !defined(_WIN32)
        Fd descriptor;
#endif
    };
    std::map<char,std::shared_ptr<Root>> roots;
    mutable std::mutex mutex;
    // reparse 快取（持 mutex 時存取）：命名空間狀態以根目錄 ctime 驗證；沒有掛載標記的目錄以
    // (裝置, inode, ctime) 記錄。fsetxattr／fremovexattr 會更新 ctime，狀態重載時整批清空。
    struct ReparseCache {
        bool valid{};
        std::int64_t root_changed_sec{},root_changed_nsec{};
        FileVolumeState state;
        std::set<std::tuple<std::uint64_t,std::uint64_t,std::int64_t,std::int64_t>> no_marker;
    };
    mutable ReparseCache reparse_cache;
    void invalidate_reparse_cache() const {reparse_cache.valid=false;reparse_cache.no_marker.clear();}
    int owner() const {
#if !defined(_WIN32)
        return roots.empty()?-1:roots.begin()->second->descriptor.value;
#else
        return -1;
#endif
    }
    FileVolumeState defaults() const{FileVolumeState state;for(const auto& [id,root]:roots){(void)root;state.drives.emplace(id,id);}return state;}
#if defined(__APPLE__)
    FileVolumeState load() const try {
        auto state=defaults();if(roots.empty())return state;
        const auto stored=attribute(owner(),namespace_attribute);if(stored.second==4390)return state;if(stored.second)return {{},{},stored.second};
        Reader reader{stored.first};const auto magic=reader.take(8);if(!std::equal(magic.begin(),magic.end(),namespace_magic.begin()))return {{},{},13};
        if(reader.get(4)!=roots.size())return {{},{},13};
        for(const auto& [id,root]:roots){if(reader.get(1)!=static_cast<unsigned char>(id) || reader.get(8)!=root->volume.device || reader.get(8)!=root->volume.inode)return {{},{},13};}
        state.drives.clear();
        for(char drive='A';drive<='Z';++drive){const auto target=static_cast<char>(reader.get(1));if(target && !roots.contains(target))return {{},{},13};if(target)state.drives.emplace(drive,target);}
        const auto count=reader.get(4);if(count>max_mounts)return {{},{},13};std::set<std::pair<char,std::string>> paths;std::set<std::pair<std::uint64_t,std::uint64_t>> identities;
        for(std::uint64_t i=0;i<count;++i){FileVolumeMount mount;mount.source=static_cast<char>(reader.get(1));mount.target=static_cast<char>(reader.get(1));mount.device=reader.get(8);mount.inode=reader.get(8);const auto length=reader.get(4);const auto path=reader.take(static_cast<std::size_t>(length));mount.path.assign(reinterpret_cast<const char*>(path.data()),path.size());
            if(!roots.contains(mount.source) || !roots.contains(mount.target) || !relative_path(mount.path) || !mount.inode || !paths.emplace(mount.source,lower(mount.path)).second || !identities.emplace(mount.device,mount.inode).second)return {{},{},13};state.folders.push_back(std::move(mount));}
        if(reader.at!=reader.bytes.size())return {{},{},13};return state;
    }catch(const std::invalid_argument&){return {{},{},13};}
    std::vector<std::uint8_t> encode(const FileVolumeState& state) const {
        std::vector<std::uint8_t> bytes(namespace_magic.begin(),namespace_magic.end());put(bytes,roots.size(),4);
        for(const auto& [id,root]:roots){put(bytes,static_cast<unsigned char>(id),1);put(bytes,root->volume.device,8);put(bytes,root->volume.inode,8);}
        for(char drive='A';drive<='Z';++drive)put(bytes,state.drives.contains(drive)?static_cast<unsigned char>(state.drives.at(drive)):0U,1);
        put(bytes,state.folders.size(),4);
        for(const auto& mount:state.folders){put(bytes,static_cast<unsigned char>(mount.source),1);put(bytes,static_cast<unsigned char>(mount.target),1);put(bytes,mount.device,8);put(bytes,mount.inode,8);put(bytes,mount.path.size(),4);bytes.insert(bytes.end(),mount.path.begin(),mount.path.end());}
        return bytes;
    }
    std::uint32_t save_bytes(std::span<const std::uint8_t> bytes) const {
        if(roots.empty())return 3;if(bytes.size()>max_bytes)return 111;
        invalidate_reparse_cache();
        return ::fsetxattr(owner(),namespace_attribute,bytes.data(),bytes.size(),0,0)<0?error():0;
    }
    // 持 mutex 時呼叫：根目錄 ctime 未變則沿用快取狀態，否則在 flock 共享鎖下重新載入。
    FileVolumeState cached_state() const {
        if(roots.empty())return defaults();
        struct stat root{};if(::fstat(owner(),&root)<0)return {{},{},error()};
        if(reparse_cache.valid && reparse_cache.root_changed_sec==root.st_ctimespec.tv_sec && reparse_cache.root_changed_nsec==root.st_ctimespec.tv_nsec)return reparse_cache.state;
        while(::flock(owner(),LOCK_SH)<0){if(errno==EINTR)continue;return {{},{},error()};}
        auto state=load();(void)::flock(owner(),LOCK_UN);
        if(!state.error){reparse_cache.valid=true;reparse_cache.root_changed_sec=root.st_ctimespec.tv_sec;reparse_cache.root_changed_nsec=root.st_ctimespec.tv_nsec;reparse_cache.state=state;reparse_cache.no_marker.clear();}
        return state;
    }
    std::uint32_t save(const FileVolumeState& state) const{return save_bytes(encode(state));}
    std::vector<std::uint8_t> marker(const FileVolumeMount& mount) const {
        const auto& source=roots.at(mount.source)->volume;const auto& target=roots.at(mount.target)->volume;const auto& scope=roots.begin()->second->volume;
        std::vector<std::uint8_t> bytes(mount_magic.begin(),mount_magic.end());put(bytes,static_cast<unsigned char>(mount.source),1);put(bytes,static_cast<unsigned char>(mount.target),1);
        for(auto value:{scope.device,scope.inode,source.device,source.inode,target.device,target.inode,mount.device,mount.inode})put(bytes,value,8);return bytes;
    }
    FileVolumeReparse check(int directory,const FileVolumeState& state) const {
        if(state.error)return {{},state.error};struct stat info{};if(::fstat(directory,&info)<0)return {{},error()};if(!S_ISDIR(info.st_mode))return {{},267};
        const auto found=std::find_if(state.folders.begin(),state.folders.end(),[&](const auto& mount){return mount.device==static_cast<std::uint64_t>(info.st_dev) && mount.inode==info.st_ino;});
        const auto stored=attribute(directory,mount_attribute);
        if(stored.second==4390)return {{},found==state.folders.end()?4390U:13U};
        if(stored.second)return {{},stored.second};
        if(found==state.folders.end() || stored.first!=marker(*found))return {{},13};
        // registry 路徑也必須指向目前固定的 inode，拒絕宿主直接搬動或替換。
        auto opened=walk(found->source,found->path);Fd fd{opened.descriptor};if(opened.error)return {{},opened.error};struct stat current{};
        if(::fstat(fd.value,&current)<0)return {{},error()};if(current.st_dev!=info.st_dev || current.st_ino!=info.st_ino)return {{},13};return {*found,0};
    }
#endif
    FileVolumeOpen walk(char volume,std::string_view path) const {
#if !defined(_WIN32)
        if(!roots.contains(volume))return {-1,3};if(!path.empty() && !relative_path(path))return {-1,87};
        const auto descriptor=::openat(roots.at(volume)->descriptor.value,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(descriptor<0)return {-1,error()};Fd current{descriptor};
        for(std::size_t at=0;at<path.size();){auto end=path.find('/',at);if(end==path.npos)end=path.size();const auto name=std::string(path.substr(at,end-at));at=end+1;
            const auto next=::openat(current.value,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(next<0)return {-1,error()};::close(current.value);current.value=next;}
        const auto fd=current.value;current.value=-1;return {fd,0};
#else
        (void)volume;(void)path;return {-1,50};
#endif
    }
};

FileVolumes::FileVolumes(const std::map<char,std::filesystem::path>& roots):impl_(std::make_shared<Impl>()) {
    for(const auto& [id,path]:roots){if(!letter(id) || path.empty())throw std::invalid_argument("磁碟區需要有效識別與根目錄");auto root=std::make_shared<Impl::Root>();root->volume.id=id;
        std::error_code ec;root->volume.host_path=std::filesystem::canonical(path,ec);if(ec || !std::filesystem::is_directory(root->volume.host_path))throw std::invalid_argument("磁碟區根目錄必須存在");
#if !defined(_WIN32)
        root->descriptor.value=::open(root->volume.host_path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);struct stat info{};
        if(root->descriptor.value<0 || ::fstat(root->descriptor.value,&info)<0)throw std::invalid_argument("磁碟區根目錄無法固定");root->volume.device=static_cast<std::uint64_t>(info.st_dev);root->volume.inode=info.st_ino;
#endif
        impl_->roots.emplace(id,std::move(root));}
}
std::vector<FileVolume> FileVolumes::volumes() const {std::vector<FileVolume> result;for(const auto& [id,root]:impl_->roots){(void)id;auto volume=root->volume;
#if defined(__APPLE__)
    std::array<char,4096> path{};if(::fcntl(root->descriptor.value,F_GETPATH,path.data())==0)volume.host_path=path.data();else volume.host_path.clear();
#endif
    result.push_back(std::move(volume));}return result;}
FileVolumeState FileVolumes::snapshot() const {
#if defined(__APPLE__)
    std::lock_guard<std::mutex> guard(impl_->mutex);return impl_->cached_state();
#else
    return impl_->defaults();
#endif
}
FileVolumeOpen FileVolumes::open_root(char volume,bool metadata_only) const{
#if defined(__APPLE__)
    if(metadata_only){if(!impl_->roots.contains(volume))return {-1,3};const auto fd=::openat(impl_->roots.at(volume)->descriptor.value,".",O_SEARCH|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);return {fd,fd<0?error():0U};}
#else
    (void)metadata_only;
#endif
    return impl_->walk(volume,{});
}
FileVolumeReparse FileVolumes::reparse(int directory) const {
#if defined(__APPLE__)
    struct stat info{};if(::fstat(directory,&info)<0)return {{},error()};if(!S_ISDIR(info.st_mode))return {{},267};
    const std::tuple<std::uint64_t,std::uint64_t,std::int64_t,std::int64_t> key{static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino),info.st_ctimespec.tv_sec,info.st_ctimespec.tv_nsec};
    FileVolumeState state;std::int64_t version_sec{},version_nsec{};
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        state=impl_->cached_state();if(state.error)return {{},state.error};
        if(impl_->reparse_cache.valid && impl_->reparse_cache.no_marker.contains(key))return {{},4390};
        version_sec=impl_->reparse_cache.root_changed_sec;version_nsec=impl_->reparse_cache.root_changed_nsec;
    }
    Lock lock(impl_->mutex,impl_->owner(),false);if(lock.error)return {{},lock.error};
    auto result=impl_->check(directory,state);
    // 只有在同一版命名空間下確認「無標記且不在掛載清單」才記錄；狀態重載時會整批清空。
    if(result.error==4390 && impl_->reparse_cache.valid && impl_->reparse_cache.root_changed_sec==version_sec && impl_->reparse_cache.root_changed_nsec==version_nsec && impl_->reparse_cache.no_marker.size()<16384)
        impl_->reparse_cache.no_marker.insert(key);
    return result;
#else
    (void)directory;return {{},50};
#endif
}
std::uint32_t FileVolumes::set_drive(char drive,char volume,bool writable) try {
    if(!writable)return 5;if(!letter(drive) || !letter(volume))return 87;if(!impl_->roots.contains(volume))return 3;
#if defined(__APPLE__)
    Lock lock(impl_->mutex,impl_->owner(),true);if(lock.error)return lock.error;auto state=impl_->load();if(state.error)return state.error;
    if(state.drives.contains(drive))return 183;state.drives.emplace(drive,volume);return impl_->save(state);
#else
    return 50;
#endif
}catch(const std::bad_alloc&){return 8;}
std::uint32_t FileVolumes::delete_drive(char drive,bool writable) try {
    if(!writable)return 5;if(!letter(drive))return 87;
#if defined(__APPLE__)
    Lock lock(impl_->mutex,impl_->owner(),true);if(lock.error)return lock.error;auto state=impl_->load();if(state.error)return state.error;
    if(!state.drives.erase(drive))return 2;return impl_->save(state);
#else
    return 50;
#endif
}catch(const std::bad_alloc&){return 8;}
std::uint32_t FileVolumes::set_folder(char source,std::string path,char target,bool writable) try {
    if(!writable)return 5;if(!relative_path(path))return 87;if(!impl_->roots.contains(source) || !impl_->roots.contains(target))return 3;
#if defined(__APPLE__)
    Lock lock(impl_->mutex,impl_->owner(),true);if(lock.error)return lock.error;auto state=impl_->load();if(state.error)return state.error;
    const auto opened=impl_->walk(source,path);Fd fd{opened.descriptor};if(opened.error)return opened.error;const auto existing=impl_->check(fd.value,state);if(!existing.error)return 145;if(existing.error!=4390)return existing.error;
    if(std::any_of(state.folders.begin(),state.folders.end(),[&](const auto& mount){return mount.source==source && lower(mount.path)==lower(path);}))return 13;
    struct Stream {DIR* value{};~Stream(){if(value)::closedir(value);}} stream;
    const auto duplicate=::openat(fd.value,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(duplicate<0)return error();stream.value=::fdopendir(duplicate);if(!stream.value){const auto code=error();::close(duplicate);return code;}
    errno=0;while(const auto* entry=::readdir(stream.value))if(std::string_view(entry->d_name)!="." && std::string_view(entry->d_name)!="..")return 145;if(errno)return error();
    struct stat info{};if(::fstat(fd.value,&info)<0)return error();FileVolumeMount mount{source,target,std::move(path),static_cast<std::uint64_t>(info.st_dev),info.st_ino};
    if(state.folders.size()>=max_mounts)return 8;state.folders.push_back(mount);const auto bytes=impl_->marker(mount);
    // 所有可配置記憶體先完成，避免 bad_alloc 留下半完成的原生標記。
    const auto namespace_bytes=impl_->encode(state);if(namespace_bytes.size()>max_bytes)return 111;
    if(::fsetxattr(fd.value,mount_attribute,bytes.data(),bytes.size(),0,XATTR_CREATE)<0)return error();
    const auto saved=impl_->save_bytes(namespace_bytes);if(saved){if(::fremovexattr(fd.value,mount_attribute,0)<0)return 1117;return saved;}
    return 0;
#else
    return 50;
#endif
}catch(const std::bad_alloc&){return 8;}
std::uint32_t FileVolumes::delete_folder(char source,std::string path,bool writable,const std::function<std::uint32_t()>& remove) try {
    if(!writable)return 5;if(!relative_path(path))return 87;
#if defined(__APPLE__)
    Lock lock(impl_->mutex,impl_->owner(),true);if(lock.error)return lock.error;auto state=impl_->load();if(state.error)return state.error;
    const auto opened=impl_->walk(source,path);Fd fd{opened.descriptor};if(opened.error)return opened.error;const auto original=impl_->check(fd.value,state);
    if(original.error==4390)return std::any_of(state.folders.begin(),state.folders.end(),[&](const auto& mount){return mount.source==source && lower(mount.path)==lower(path);})?13U:0U;
    if(original.error)return original.error;
    const auto prior_bytes=impl_->encode(state);std::erase(state.folders,original.mount);const auto next_bytes=impl_->encode(state);const auto saved=impl_->save_bytes(next_bytes);if(saved)return saved;
    if(remove){const auto code=remove();if(code && impl_->save_bytes(prior_bytes))return 1117;return code;}
    if(::fremovexattr(fd.value,mount_attribute,0)<0){const auto code=error();if(impl_->save_bytes(prior_bytes))return 1117;return code;}return 0;
#else
    (void)source;(void)remove;return 50;
#endif
}catch(const std::bad_alloc&){return 8;}
std::uint32_t FileVolumes::relocate_folders(char source,std::string from,char target,std::string to,bool writable,const std::function<std::uint32_t()>& move,const std::function<std::uint32_t()>& undo) try {
    if(!writable)return 5;if(!move || !undo || !relative_path(from) || !relative_path(to))return 87;
#if defined(__APPLE__)
    Lock lock(impl_->mutex,impl_->owner(),true);if(lock.error)return lock.error;auto state=impl_->load();if(state.error)return state.error;bool changed=false;
    auto next=state;for(auto& point:next.folders)if(point.source==source && (point.path==from || point.path.starts_with(from+'/'))){
        if(source!=target)return 17;const auto old=impl_->walk(source,point.path);Fd fd{old.descriptor};if(old.error)return old.error;const auto valid=impl_->check(fd.value,state);if(valid.error)return valid.error;
        point.path=to+point.path.substr(from.size());changed=true;
    }
    if(!changed)return move();std::set<std::pair<char,std::string>> paths;for(const auto& point:next.folders)if(!paths.emplace(point.source,lower(point.path)).second)return 183;
    const auto bytes=impl_->encode(next);if(bytes.size()>max_bytes)return 111;
    const auto moved=move();if(moved)return moved;
    for(const auto& point:next.folders)if(point.source==source && (point.path==to || point.path.starts_with(to+'/'))){
        const auto opened=impl_->walk(source,point.path);Fd fd{opened.descriptor};const auto valid=opened.error?FileVolumeReparse{{},opened.error}:impl_->check(fd.value,next);
        if(valid.error){if(undo())return 1117;return valid.error;}
    }
    const auto saved=impl_->save_bytes(bytes);if(saved && undo())return 1117;return saved;
#else
    (void)source;(void)target;return move();
#endif
}catch(const std::bad_alloc&){return 8;}
}
