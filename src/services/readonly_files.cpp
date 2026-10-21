#include "x64arm/startup_profile.hpp"
#include "x64arm/readonly_files.hpp"
#include "x64arm/pe_file_cache.hpp"
#include "x64arm/shared_file_state.hpp"
#include "x64arm/process_io.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/file_security.hpp"
#include "x64arm/security_access.hpp"
#include "x64arm/unicode.hpp"
#include "directory_overlay.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <map>
#include <mutex>
#if defined(__APPLE__)
#include <sys/attr.h>
#include <copyfile.h>
#endif
#if defined(__APPLE__) || defined(__linux__)
#include <sys/xattr.h>
#endif
#if !defined(_WIN32)
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm {
#if !defined(_WIN32)
namespace {
// Windows 路徑逐層以 ASCII 大小寫不敏感比對目錄項目。每次列舉整個目錄的成本與目錄大小成正比，
// 大型快取目錄會讓每次開檔都很慢；這裡依目錄 (dev, inode) 保存摺疊名稱對照，
// 以目錄 ctime／mtime 驗證：新增、刪除或改名都會更新兩者，變更後立即重建。
struct FoldedDirectory {timespec changed{},modified{};std::unordered_map<std::string,std::string> names;}; // 空字串代表名稱有歧義
std::mutex folded_mutex;
std::map<std::pair<std::uint64_t,std::uint64_t>,FoldedDirectory> folded_directories;
std::size_t folded_names{};
constexpr std::size_t folded_name_limit=1U<<20;
std::string fold_ascii(std::string text){for(auto& c:text)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');return text;}
bool same_time(const timespec& a,const timespec& b){return a.tv_sec==b.tv_sec && a.tv_nsec==b.tv_nsec;}
// 回傳實際名稱；無對應時回傳 requested；有歧義時回傳空字串。與逐次 readdir 的結果相同。
std::string folded_component(int directory,const std::string& requested){
    struct stat info{};const bool identified=::fstat(directory,&info)==0;
#if defined(__APPLE__)
    const auto changed=info.st_ctimespec,modified=info.st_mtimespec;
#else
    const auto changed=info.st_ctim,modified=info.st_mtim;
#endif
    const auto wanted=fold_ascii(requested);const std::pair<std::uint64_t,std::uint64_t> key{static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
    if(identified){
        std::lock_guard lock(folded_mutex);const auto found=folded_directories.find(key);
        if(found!=folded_directories.end() && same_time(found->second.changed,changed) && same_time(found->second.modified,modified)){
            const auto name=found->second.names.find(wanted);return name==found->second.names.end()?requested:name->second;
        }
    }
    struct DirectoryStream {DIR* value;~DirectoryStream(){if(value)::closedir(value);}};
    const int copied=::openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(copied<0)return requested;
    DirectoryStream entries{::fdopendir(copied)};
    if(!entries.value){::close(copied);return requested;}
    FoldedDirectory listing;listing.changed=changed;listing.modified=modified;
    while(const auto* entry=::readdir(entries.value)){
        const std::string name=entry->d_name;const auto folded=fold_ascii(name);
        const auto [at,inserted]=listing.names.try_emplace(folded,name);if(!inserted)at->second.clear();
    }
    std::string result=requested;if(const auto name=listing.names.find(wanted);name!=listing.names.end())result=name->second;
    if(identified){
        std::lock_guard lock(folded_mutex);
        if(folded_names+listing.names.size()>folded_name_limit){folded_directories.clear();folded_names=0;}
        if(const auto old=folded_directories.find(key);old!=folded_directories.end()){folded_names-=old->second.names.size();folded_directories.erase(old);}
        folded_names+=listing.names.size();folded_directories.emplace(key,std::move(listing));
    }
    return result;
}
}
#endif
// FILE_OBJECT 與磁碟檔案分開：DuplicateHandle 共用 HostFile，獨立 open 共用 inode state。
struct HostDeleteTarget {
    int directory{-1};
    std::string leaf;
    std::uint64_t device{}, inode{}, parent_device{}, parent_inode{};
    bool pending{}, is_directory{};std::uint32_t shared_index{};
    ~HostDeleteTarget() {
#if !defined(_WIN32)
        if(directory>=0)::close(directory);
#endif
    }
};
struct HostFileState {
    std::mutex size_mutex;
    bool pending{};std::uint32_t record_error{};std::shared_ptr<SharedFileState> shared;
    std::vector<std::weak_ptr<HostFile>> opens;
    std::vector<std::shared_ptr<HostDeleteTarget>> targets;
    ~HostFileState() {
#if !defined(_WIN32)
        if(shared)return;
        for(const auto& target:targets)if(target->pending) {
            struct stat current{};
            // 不追蹤外部替換的同名檔；只刪原本取得且仍相同的 directory entry。
            if(::fstatat(target->directory,target->leaf.c_str(),&current,AT_SYMLINK_NOFOLLOW)==0 &&
               static_cast<std::uint64_t>(current.st_dev)==target->device && static_cast<std::uint64_t>(current.st_ino)==target->inode)
                (void)::unlinkat(target->directory,target->leaf.c_str(),target->is_directory?AT_REMOVEDIR:0);
        }
#endif
    }
};
struct HostFileRegistry { std::map<std::pair<std::uint64_t,std::uint64_t>,std::weak_ptr<HostFileState>> files; };
std::shared_ptr<HostFileState> ReadOnlyFiles::file_state(const FileMetadata& info,bool create) {
    if(!host_files_) {if(!create)return {};host_files_=std::make_shared<HostFileRegistry>();}
    const auto key=std::pair{info.device,info.id};
    const auto found=host_files_->files.find(key);
    if(found!=host_files_->files.end()) {if(auto state=found->second.lock())return state;host_files_->files.erase(found);}
    std::shared_ptr<SharedFileState> shared;std::uint32_t record_error{};
#if defined(__APPLE__)
    if(!shared_session_.empty()){auto opened=SharedFileState::open(shared_session_,info,create);shared=std::move(opened.first);record_error=opened.second;}
#endif
    if(!create && !shared && !record_error)return {};
    // 回收 weak registry，不因長時間反覆開啟暫存檔而累積條目。
    std::erase_if(host_files_->files,[](const auto& entry){return entry.second.expired();});
    auto state=std::make_shared<HostFileState>();state->shared=std::move(shared);state->record_error=record_error;host_files_->files.emplace(key,state);return state;
}
std::uint32_t ReadOnlyFiles::sharing_error(const FileMetadata& info,std::uint32_t access,std::uint32_t sharing,const HostFile* owner) {
    const auto state=file_state(info);if(!state)return 0;if(state->record_error)return state->record_error;
    if(state->shared){if(const auto error=state->shared->sharing(access,sharing,owner?owner->transfer_identity():0))return error;}else if(state->pending)return 5;
    const auto category=[](std::uint32_t rights){return ((rights&(0x80000000U|1U|0x20U))?1U:0U)|((rights&(0x40000000U|2U|4U))?2U:0U)|((rights&0x10000U)?4U:0U);};
    for(const auto& weak:state->opens)if(const auto file=weak.lock())if(file.get()!=owner && (!owner || !owner->transfer_identity() || file->transfer_identity()!=owner->transfer_identity()))
        if((category(access)&~file->sharing_) || (category(file->access_)&~sharing))return 32;
    return 0;
}
bool HostFile::delete_pending() const {return state_ && (state_->shared?state_->shared->pending():state_->pending);}
std::uint64_t HostFile::transfer_identity() const {return shared_lease_?shared_lease_->identity():0;}
std::shared_ptr<void> HostFile::retain_file() const {return state_;}
void ReadOnlyFiles::set_shared_session(std::string session){
    if(shared_session_==session)return;if(!shared_session_.empty())throw std::invalid_argument("不能替換已使用的檔案共享命名空間");shared_session_=std::move(session);
}
std::uint32_t ReadOnlyFiles::attach_shared_file(HostFile& file,std::uint64_t object,bool existing){
    if(!file.state_)return 6;if(file.state_->record_error)return file.state_->record_error;
#if !defined(__APPLE__)
    (void)object;(void)existing;return 0;
#else
    auto& state=*file.state_;if(!state.shared)return object?6U:0U;
    const auto info=file.information();if(info.error)return info.error;
    for(const auto& target:state.targets)if(!target->shared_index){
        auto [index,error]=state.shared->target(target->directory,target->leaf,info.metadata,target->is_directory);if(error)return error;target->shared_index=index;
    }
    const auto target=file.delete_on_close_ && file.delete_target_?file.delete_target_->shared_index:0U;
    if(!file.shared_lease_){
        auto [lease,error]=state.shared->acquire(object,file.access_,file.sharing_,existing,target);if(error)return error;file.shared_lease_=std::move(lease);
        // 發佈前已取得的原始 inode 鎖併入同一紀錄，之後仍能精確解鎖。
        for(const auto& held:file.locks_)if(const auto code=state.shared->range_lock(file.transfer_identity(),held.offset,held.length,held.exclusive,false,[]{return 0U;},true)){file.shared_lease_.reset();return code;}
    }
    else if(target)if(const auto error=state.shared->close_target(file.transfer_identity(),target))return error;
    if(file.delete_target_ && file.delete_target_->pending)if(const auto error=state.shared->disposition(file.delete_target_->shared_index,true,file.transfer_identity()))return error;
    return 0;
#endif
}
std::uint32_t ReadOnlyFiles::publish_shared_file(HostFile& file){
#if !defined(__APPLE__)
    (void)file;return 0;
#else
    if(!file.state_ || shared_session_.empty())return 50;auto& state=*file.state_;
    if(!state.shared){const auto info=file.information();if(info.error)return info.error;auto [record,error]=SharedFileState::open(shared_session_,info.metadata,true);if(error)return error;state.shared=std::move(record);state.record_error=0;}
    for(const auto& weak:state.opens)if(const auto peer=weak.lock())if(const auto error=attach_shared_file(*peer,0,true))return error;
    if(const auto error=attach_shared_file(file,0,true))return error;
    for(const auto& target:state.targets)if(target->pending){
        const bool live=std::any_of(state.opens.begin(),state.opens.end(),[&](const auto& weak){const auto peer=weak.lock();return peer && peer->delete_target_==target;});
        if(!live)if(const auto error=state.shared->disposition(target->shared_index,true))return error;
    }
    return 0;
#endif
}

ReadOnlyFiles::ReadOnlyFiles(const std::filesystem::path& root, const std::filesystem::path& c_drive, bool writable, const std::filesystem::path& install_root, const std::map<char,std::filesystem::path>& drives) : writable_(writable) {
    const auto validate = [](const auto& path, const char* option) {
        if (path.empty()) return std::filesystem::path{};
        std::error_code error;
        const auto directory = std::filesystem::canonical(path, error);
        if (error || !std::filesystem::is_directory(directory)) throw std::invalid_argument(std::string(option) + " 必須是存在的目錄");
        return directory;
    };
    root_ = validate(root, "--fs-root"); c_drive_ = validate(c_drive, "--c-drive");
    install_root_ = validate(install_root, "--install-root");
    for(const auto& [letter,path]:drives){if(letter<'D' || letter>'Z' || path.empty())throw std::invalid_argument("磁碟映射需要 D-Z 與有效目錄");drives_.emplace(letter,validate(path,"--drive"));}
    auto volumes=drives_;if(!c_drive_.empty())volumes.emplace('C',c_drive_);volumes_=std::make_shared<FileVolumes>(volumes);
    for(const auto& [id,path]:volumes)if(!root_.empty() && path==root_){relative_volume_=id;break;}
}
std::string ReadOnlyFiles::map_executable(const std::filesystem::path& path) {
    const auto host=std::filesystem::canonical(path);
    if(!std::filesystem::is_regular_file(host))throw std::invalid_argument("主映像必須是一般檔案");
    std::string guest="C:/"+host.filename().string();
    std::vector<std::pair<std::filesystem::path,std::string>> mounts{{install_root_,"C:/Program Files/"},{c_drive_,"C:/"}};
    for(const auto& [letter,mount_path]:drives_)mounts.emplace_back(mount_path,std::string(1,letter)+":/");
    // fs-root 是 C 槽的應用程式覆疊；保留子目錄，讓主映像、PEB 與 DLL／資源搜尋一致。
    // 明確指定的磁碟及安裝目錄仍優先使用各自的既有映射。
    mounts.emplace_back(root_,std::string(1,relative_volume_?relative_volume_:'C')+":/");
    for(const auto& [mount,prefix]:mounts) {
        if(mount.empty())continue;
        const auto relative=host.lexically_relative(mount);
        if(!relative.empty() && *relative.begin()!="..") {guest=std::string(prefix)+relative.generic_string();break;}
    }
    auto key=guest;for(auto& c:key)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
    readonly_files_[key]=host;
    // 子程序即使位於另一個掛載，仍繼承原 fs-root 的客體路徑與工作目錄能力。
    if(!root_.empty())for(const auto& [mount,prefix]:mounts) {
        if(mount.empty())continue;
        const auto relative=root_.lexically_relative(mount);
        if(relative.empty() || *relative.begin()=="..")continue;
        auto application_root=prefix+(relative=="."?std::string{}:relative.generic_string()+"/");
        for(auto& c:application_root)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
        application_sources_[application_root]=root_;break;
    }
    application_sources_.try_emplace(key.substr(0,key.find_last_of('/')+1),host.parent_path());
    std::replace(guest.begin(),guest.end(),'/','\\');return guest;
}
bool ReadOnlyFiles::readonly_descriptor(int descriptor) const {
#if !defined(_WIN32)
    struct stat current{};if(::fstat(descriptor,&current)!=0)return false;
    return readonly_identity(static_cast<std::uint64_t>(current.st_dev),current.st_ino);
#else
    (void)descriptor;return false;
#endif
}
bool ReadOnlyFiles::readonly_identity(std::uint64_t device,std::uint64_t inode) const {
#if !defined(_WIN32)
    for(const auto& [key,path]:readonly_files_) {
        (void)key;struct stat original{};
        if(::stat(path.c_str(),&original)==0 && device==static_cast<std::uint64_t>(original.st_dev) && inode==original.st_ino)return true;
    }
#else
    (void)device;(void)inode;
#endif
    return false;
}
bool ReadOnlyFiles::writable_image_source(std::string path,const std::filesystem::path& host) const {
#if !defined(_WIN32)
    if(!writable_)return false;
    // 只核對明示可寫掛載，不沿用主映像相鄰目錄的唯讀能力或硬連結別名。
    auto mounted=*this;mounted.readonly_files_.clear();mounted.application_sources_.clear();
    const auto direct=mounted.resolve_mapped(std::move(path),false,nullptr);
    if(direct.descriptor>=0)::close(direct.descriptor);
    if(direct.error || direct.readonly_file || direct.host_path.empty())return false;
    std::error_code error;
    if(!std::filesystem::equivalent(direct.host_path.parent_path(),host.parent_path(),error) || error)return false;
    return std::filesystem::equivalent(direct.host_path,host,error) && !error;
#else
    (void)path;(void)host;return false;
#endif
}
void ReadOnlyFiles::mount_readonly(std::string guest_directory,const std::filesystem::path& host_directory) {
    const auto normalized=resolve_mapped(std::move(guest_directory),true,nullptr);
#if !defined(_WIN32)
    if(normalized.descriptor>=0)::close(normalized.descriptor);
#endif
    if(normalized.key.size()<4 || normalized.key[1]!=':' || normalized.key.ends_with('/'))throw std::invalid_argument("唯讀掛載需要磁碟的絕對子目錄");
    const auto host=std::filesystem::canonical(host_directory);
    if(!std::filesystem::is_directory(host))throw std::invalid_argument("唯讀掛載來源必須是目錄");
    const auto prefix=normalized.key+'/';
    if(const auto found=readonly_mounts_.find(prefix);found!=readonly_mounts_.end() && found->second==host)return;
    readonly_mounts_[prefix]=host;cache_=std::make_shared<Cache>();
}
ReadOnlyFiles::Resolved ReadOnlyFiles::resolve(std::string path, bool directory_only, std::string* leaf, bool metadata_only,bool follow_leaf,unsigned depth,bool strict_root) const {
    security::NativeFileIdentityScope identity_scope;
    auto result=resolve_mapped(std::move(path),directory_only,leaf,metadata_only,depth,follow_leaf,strict_root);
    if(result.error && result.key.size()>2 && result.key[1]==':'){
        const auto original=static_cast<char>(result.key[0]>='a' && result.key[0]<='z'?result.key[0]-32:result.key[0]);const auto state=volumes_->snapshot();
        const auto known=volumes_->volumes();if(std::any_of(known.begin(),known.end(),[&](const auto& item){return item.id==original;}) && !state.drives.contains(original))return result;
    }
    if(!result.error || (result.error!=2 && result.error!=3 && !(result.error==5 && c_drive_.empty() && result.key.starts_with("c:/"))))return result;
    for(const auto& [prefix,host]:application_sources_){
        if(!result.key.starts_with(prefix) && result.key+"/"!=prefix)continue;
        const auto suffix=result.key.size()>=prefix.size()?result.key.substr(prefix.size()):std::string{};
        ReadOnlyFiles source({},host);source.security_context_=security_context_;auto mapped=source.resolve_mapped("C:/"+suffix,directory_only,nullptr,metadata_only,depth,follow_leaf,strict_root);
        if(mapped.error)continue;
        if(leaf){
#if !defined(_WIN32)
            ::close(mapped.descriptor);
#endif
            return {result.key,-1,5,true};
        }
        mapped.key=result.key;mapped.readonly_file=true;return mapped;
    }
    return result;
}
ReadOnlyFiles::Resolved ReadOnlyFiles::resolve_mapped(std::string path, bool directory_only, std::string* leaf, bool metadata_only,unsigned depth,bool follow_leaf,bool strict_root) const {
    if(depth>32)return {{},-1,1921}; // ERROR_CANT_RESOLVE_FILENAME；限制循環與超深 reparse 鏈。
    // 僅查中繼資料時不要求內容讀權，仍沿用原本磁碟映射與 NOFOLLOW 邊界。
#if defined(__APPLE__)
    const auto file_mode=metadata_only?O_SEARCH:O_RDONLY;
    const auto traversal_mode=O_SEARCH;
#elif defined(__linux__)
    const auto file_mode=metadata_only?O_PATH:O_RDONLY;
    const auto traversal_mode=O_PATH;
#else
    (void)metadata_only;
#if !defined(_WIN32)
    const auto file_mode=O_RDONLY;
    const auto traversal_mode=O_RDONLY;
#endif
#endif
    const bool relative_namespace=path.starts_with("relative:/");
    if(relative_namespace){if(!depth)return {{},-1,5};path.erase(0,10);}
    if(path.empty())return {{},-1,2};
    if(!relative_namespace){
        std::string network_path=path;std::replace(network_path.begin(),network_path.end(),'\\','/');
        if(network_path.starts_with("//?/") && network_path.size()>8 && (network_path.substr(4,4)=="UNC/" || network_path.substr(4,4)=="unc/"))network_path="//"+network_path.substr(8);
        else if(network_path.starts_with("//?/"))network_path.erase(0,4);
        auto location=network_files_->locate(network_scope_,utf8_to_utf16_lossy(network_path));
        if(location.matched){
            if(location.error)return {network_path,-1,location.error};
            auto lease=std::move(location.lease);ReadOnlyFiles source({},location.binding->mount->root(),writable_ && !location.binding->readonly);source.security_context_=security_context_;
            auto result=source.resolve_mapped("C:/"+utf16_to_utf8(location.suffix),directory_only,leaf,metadata_only,depth+1,follow_leaf,strict_root);
            result.key=utf16_to_utf8(network_resources::canonical(utf8_to_utf16_lossy(network_path)));std::replace(result.key.begin(),result.key.end(),'\\','/');result.volume=0;result.network_lease=std::move(lease);result.readonly_file=result.readonly_file || location.binding->readonly;result.mount_path=utf16_to_utf8(location.binding->remote)+"/";return result;
        }
    }
    if(!relative_namespace && !current_directory_.empty() && path.find(':')==std::string::npos && path.front()!='/' && path.front()!='\\')path=current_directory_+"/"+path;
    std::replace(path.begin(), path.end(), '\\', '/');
    const bool verbatim=path.starts_with("//?/");
    const auto guid_volume=windows_volume_letter(std::string_view(path));
    if(guid_volume)path=std::string(1,guid_volume)+":/"+path.substr(49);
    if(path.starts_with("//?/") && path.size()>10){auto prefix=path.substr(4,6);for(auto& c:prefix)if(c>='A' && c<='Z')c=static_cast<char>(c+32);if(prefix=="volume")return {{},-1,3};}
    if (path.starts_with("//?/")) path.erase(0,4);
    bool drive = false;char drive_letter='C';
    if (path.size() >= 3 && ((path[0]>='A' && path[0]<='Z') || (path[0]>='a' && path[0]<='z')) && path[1] == ':' && path[2] == '/') {
        drive_letter=path[0]>='a'?static_cast<char>(path[0]-'a'+'A'):path[0];
        drive = true; path.erase(0, 3);
        // 磁碟根目錄串接檔名時允許重複分隔符；UNC 仍走下方拒絕路徑。
        path.erase(0,path.find_first_not_of('/'));
    } else if (!path.empty() && path[0] == '/' && (path.size() == 1 || path[1] != '/')) {
        if(current_directory_.size()>1 && current_directory_[1]==':'){drive_letter=current_directory_[0];if(drive_letter>='a' && drive_letter<='z')drive_letter=static_cast<char>(drive_letter-'a'+'A');}
        drive = true; path.erase(0, 1);
    }
    if ((!path.empty() && path.front() == '/') || path.find(':') != std::string::npos || path.find('\0') != std::string::npos) return {{}, -1, 5};
    std::vector<std::string> components;
    for (std::size_t begin = 0; begin < path.size();) {
        auto end = path.find('/', begin); if (end == std::string::npos) end = path.size();
        const auto part = path.substr(begin, end - begin); begin = end + 1;
        if (part == "..") {if(!dos_path_normalization_ || verbatim)return {{}, -1, 5};if(!components.empty())components.pop_back();continue;}
        if (!part.empty() && part != ".") components.push_back(part);
    }
    std::string root_key=guid_volume?windows_volume_path(guid_volume):drive?std::string(1,drive_letter)+":/":"relative:/";
    std::replace(root_key.begin(),root_key.end(),'\\','/');std::string key=root_key;
    for (const auto& part : components) { if (key.back() != '/') key += '/'; key += part; }
    if (drive) for (auto& c : key) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    const auto namespace_state=volumes_->snapshot();
    if(!cache_->volumes || *cache_->volumes!=namespace_state){auto next=std::make_shared<Cache>();next->volumes=namespace_state;cache_=std::move(next);}
    if(drive && namespace_state.error)return {key,-1,namespace_state.error};
    const auto known_volumes=volumes_->volumes();char volume=drive?guid_volume:relative_volume_;
    if(drive && !guid_volume)if(const auto found=namespace_state.drives.find(drive_letter);found!=namespace_state.drives.end())volume=found->second;
    const auto original_drive=std::find_if(known_volumes.begin(),known_volumes.end(),[&](const auto& item){return item.id==drive_letter;});
    if(drive && !volume && original_drive!=known_volumes.end())return {key,-1,5};
    auto overlay_key=key;
    if(volume){overlay_key=std::string(1,static_cast<char>(volume+32))+":/";for(const auto& part:components){if(overlay_key.back()!='/')overlay_key+='/';overlay_key+=part;}for(auto& c:overlay_key)if(c>='A' && c<='Z')c=static_cast<char>(c+32);}
    const std::pair<const std::string,std::filesystem::path>* mounted=nullptr;
    for(const auto& entry:readonly_mounts_)if((overlay_key.starts_with(entry.first) || overlay_key+'/'==entry.first) && (!mounted || entry.first.size()>mounted->first.size()))mounted=&entry;
    if(mounted){
        if(leaf)return {key,-1,5,true};
        ReadOnlyFiles source({},mounted->second);
        source.security_context_=security_context_;
        auto result=source.resolve_mapped("C:/"+(overlay_key.size()>=mounted->first.size()?overlay_key.substr(mounted->first.size()):std::string{}),directory_only,nullptr,metadata_only,depth,follow_leaf,strict_root);
        result.key=key;result.readonly_file=true;result.volume=0;return result;
    }
    if(const auto file=readonly_files_.find(overlay_key);file!=readonly_files_.end()) {
        if(directory_only || leaf)return {key,-1,5,true};
#if defined(_WIN32)
        return {key,-1,50,true};
#else
        if(metadata_only){
            const auto parent=file->second.parent_path();
            const auto descriptor=::open(parent.c_str(),file_mode|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
            if(descriptor>=0)if(const auto error=authorize(descriptor,0x20U)){::close(descriptor);return {key,-1,error,true,{},{},error==5};}
            return {key,descriptor,descriptor<0?(errno==ENOENT?2U:5U):0U,true,file->second,file->second.filename().string()};
        }
        const auto descriptor=::open(file->second.c_str(),file_mode|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        return {key,descriptor,descriptor<0?(errno==ENOENT?2U:5U):0U,true,file->second,{},descriptor<0 && errno!=ENOENT};
#endif
    }
    auto root=!drive?root_:std::filesystem::path{};
    const auto target_volume=std::find_if(known_volumes.begin(),known_volumes.end(),[&](const auto& item){return item.id==volume;});
    if(target_volume!=known_volumes.end())root=target_volume->host_path;
    // 先正規化並拒絕 traversal，再以完整 Windows 路徑片段選擇最長映射。
    // Program Files 的子目錄與一般 I/O 使用同一個 openat／NOFOLLOW 契約。
    const bool install_mapping=drive && !install_root_.empty() && (overlay_key=="c:/program files" || overlay_key.starts_with("c:/program files/"));
    if(install_mapping) {
        root=install_root_;components.erase(components.begin());volume=0;
    }
    if (root.empty()) return {key, -1, 5};
#if defined(_WIN32)
    return {key, -1, 50}; // Windows 宿主的檔案 backend 尚未實作，CPU 核心仍可建置。
#else
    const auto error_code = [] { return errno == ENOENT ? 2U : errno == ENOTDIR ? 3U : errno == EACCES || errno == EPERM || errno == ELOOP ? 5U : 1117U; };
    struct Descriptor {
        int value;
        ~Descriptor() { if (value >= 0) ::close(value); }
    };
    const auto pinned=volume?volumes_->open_root(volume,true):FileVolumeOpen{};
    Descriptor directory{volume?pinned.descriptor: ::open(root.c_str(), traversal_mode | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    auto host_path=root;
    auto physical_components=components;
    const auto finish=[&](Resolved result){result.volume=volume;result.mount_path=root_key;for(const auto& part:physical_components){if(!result.volume_path.empty())result.volume_path+='/';result.volume_path+=part;}return result;};
    if (directory.value < 0) return finish({key, -1, volume?pinned.error:error_code(),false,{},{},(volume?pinned.error:error_code())==5});
    // Windows C 槽使用 ASCII 大小寫不敏感查找；保持逐層 openat／NOFOLLOW 邊界。
    const auto component = [&](const std::string& requested) {
        if (!drive) return requested;
        return folded_component(directory.value, requested);
    };
    const auto expand=[&](std::size_t at,const std::string& name)->std::optional<Resolved>{
        struct stat entry{};if(::fstatat(directory.value,name.c_str(),&entry,AT_SYMLINK_NOFOLLOW)<0)return {};
        std::string prefix=root_key;
        if(install_mapping)prefix+="Program Files/";
        for(std::size_t j=0;j<=at;++j){if(prefix.back()!='/')prefix+='/';prefix+=components[j];}
        if(S_ISDIR(entry.st_mode)){
            Descriptor folder{::openat(directory.value,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};if(folder.value<0)return {};
            const auto point=volumes_->reparse(folder.value);if(point.error==4390 || point.error==50)return {};if(point.error)return finish({key,-1,point.error});
            auto target=windows_volume_path(point.mount.target);std::replace(target.begin(),target.end(),'\\','/');const auto target_root=target;
            for(std::size_t j=at+1;j<components.size();++j){if(target.back()!='/')target+='/';target+=components[j];}
            auto result=resolve(std::move(target),directory_only,leaf,metadata_only,follow_leaf,depth+1,strict_root);
            if(result.mount_path.starts_with(target_root))result.mount_path=prefix+'/'+result.mount_path.substr(target_root.size());return result;
        }
        if(!S_ISLNK(entry.st_mode))return {};
        const auto link=file_reparse::read(directory.value,name);if(link.error)return finish({key,-1,link.error});
        if(at+1<components.size() && link.kind!=2)return finish({key,-1,3});
        try{auto target=file_reparse::target_path(prefix,link.target,current_directory_,strict_root);
            for(std::size_t j=at+1;j<components.size();++j){if(target.back()!='/')target+='/';target+=components[j];}
            auto result=resolve(std::move(target),leaf && at+1==components.size()?false:directory_only,leaf,metadata_only,follow_leaf,depth+1,strict_root);return result;
        }catch(const std::invalid_argument&){return finish({key,-1,5});}
    };
    for (std::size_t i = 0; i + 1 < components.size(); ++i) {
        if(const auto error=authorize(directory.value,0x20U))return finish({key,-1,error,false,{},{},error==5});
        const auto name = component(components[i]); if (name.empty()) return finish({key, -1, 5});physical_components[i]=name;
        if(auto link=expand(i,name))return *link;
        const auto next = ::openat(directory.value, name.c_str(), traversal_mode | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) return finish({key, -1, error_code(),false,{},{},error_code()==5});
        ::close(directory.value); directory.value = next;
        host_path/=name;
    }
    if (components.empty()) { if (leaf)*leaf=".";
        if(!leaf && !metadata_only){const auto readable=::openat(directory.value,".",file_mode|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(readable<0)return finish({key,-1,error_code(),false,{},{},error_code()==5});::close(directory.value);directory.value=readable;}
        const auto fd = directory.value; directory.value = -1; return finish({key,fd,0,false,root,"."}); }
    const auto name = component(components.back()); if (name.empty()) return finish({key, -1, 5});physical_components.back()=name;
    if(const auto error=authorize(directory.value,0x20U))return finish({key,-1,error,false,{},{},error==5});
    if(follow_leaf || (!leaf && !metadata_only))if(auto link=expand(components.size()-1,name))return *link;
    if (leaf) { *leaf=name; const auto fd=directory.value;directory.value=-1;return finish({key,fd,0,false,host_path/name}); }
    if(metadata_only){
        // 父目錄存在不代表葉節點存在；先查 NOFOLLOW 中繼資料，讓缺檔沿用
        // resolve 的伴隨資源搜尋。後續仍以已固定父目錄驗證權限及 inode。
        struct stat entry{};
        if(::fstatat(directory.value,name.c_str(),&entry,AT_SYMLINK_NOFOLLOW)<0)
            return finish({key,-1,error_code(),false,{},{},error_code()==5});
        const auto fd=directory.value;directory.value=-1;return finish({key,fd,0,false,host_path/name,name});
    }
    const auto file = ::openat(directory.value, name.c_str(), file_mode | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (directory_only ? O_DIRECTORY : 0));
    if (file < 0) return finish({key, -1, error_code(),false,{},{},error_code()==5});
    return finish({key, file, 0,readonly_descriptor(file),host_path/name});
#endif
}
namespace {
#if !defined(_WIN32)
struct FileDescriptor { int value{-1}; ~FileDescriptor() { if (value >= 0) ::close(value); } };
std::uint32_t native_error(int error = errno) {
    switch(error) {case ENOENT:return 2;case ENOTDIR:return 3;case EACCES:case EPERM:case ELOOP:case EISDIR:return 5;case EBADF:return 6;
    case EMLINK:return 1142;case ENOMEM:return 8;case EEXIST:return 183;case ENOSPC:case EDQUOT:return 112;case ENOTEMPTY:return 145;case EXDEV:return 17;case EINVAL:return 87;case EMFILE:case ENFILE:return 4;default:return 1117;}
}
std::uint32_t valid_data(int descriptor,std::uint64_t eof,std::uint64_t& length,bool& managed){
    length=eof;managed=false;
#if defined(__APPLE__) || defined(__linux__)
    std::array<std::uint8_t,16> data{};
#if defined(__APPLE__)
    const auto size=::fgetxattr(descriptor,"com.x64arm.validdata",data.data(),data.size(),0,0);
    if(size<0 && (errno==ENOATTR || errno==ENOTSUP))return 0;
#else
    const auto size=::fgetxattr(descriptor,"user.x64arm.validdata",data.data(),data.size());
    if(size<0 && (errno==ENODATA || errno==ENOTSUP))return 0;
#endif
    if(size<0)return native_error();
    if(size!=16 || data[0]!='V' || data[1]!='D' || data[2]!='L' || data[3]!=1)return 13;
    length=0;for(unsigned i=0;i<8;++i)length|=std::uint64_t{data[8+i]}<<(i*8);length=std::min(length,eof);managed=true;return 0;
#else
    (void)descriptor;return 0;
#endif
}
std::uint32_t store_valid_data(int descriptor,std::uint64_t length){
#if defined(__APPLE__) || defined(__linux__)
    std::array<std::uint8_t,16> data{'V','D','L',1};for(unsigned i=0;i<8;++i)data[8+i]=static_cast<std::uint8_t>(length>>(i*8));
#if defined(__APPLE__)
    const auto result=::fsetxattr(descriptor,"com.x64arm.validdata",data.data(),data.size(),0,0);
#else
    const auto result=::fsetxattr(descriptor,"user.x64arm.validdata",data.data(),data.size(),0);
#endif
    return result<0?(errno==ENOTSUP?50U:native_error()):0;
#else
    (void)descriptor;(void)length;return 50;
#endif
}
std::uint64_t filetime(const timespec& value) {
    const auto ticks = static_cast<__int128>(value.tv_sec) * 10000000 + value.tv_nsec / 100 + UINT64_C(116444736000000000);
    return ticks < 0 ? 0 : ticks > UINT64_MAX ? UINT64_MAX : static_cast<std::uint64_t>(ticks);
}
FileMetadata metadata(const struct stat& value, int descriptor = -1, int directory = -1, std::string_view leaf = {}, std::uint32_t* query_error = nullptr) {
    FileMetadata result;
    result.size = S_ISREG(value.st_mode) && value.st_size > 0 ? static_cast<std::uint64_t>(value.st_size) : 0;
    result.valid_data_size=result.size;
    if(descriptor>=0 && S_ISREG(value.st_mode)){bool managed=false;const auto error=valid_data(descriptor,result.size,result.valid_data_size,managed);if(error && query_error)*query_error=error;}
    result.attributes = S_ISDIR(value.st_mode) ? 0x10U : 0x20U;
#if defined(__APPLE__) || defined(__linux__)
    // DOS 屬性保存在檔案本身；重啟引擎、rename 及各種查詢共用同一份資料。
    std::array<std::uint8_t,8> stored{};
    if(descriptor>=0 || directory>=0) {
        // 只有中繼資料能力時，不以內容 fd 作為 DOS 屬性查詢的先決條件。
#if defined(__APPLE__)
        // Darwin 沒有 getxattrat；由已固定父目錄取得原生路徑，前後核對同一 inode。
        std::array<char,4096> parent_path{};std::string path;
        struct stat prior{},after{};
        const bool native_path=descriptor<0 && directory>=0 && !::fcntl(directory,F_GETPATH,parent_path.data());
        if(native_path)path=std::string(parent_path.data())+"/"+std::string(leaf);
        const bool pinned=native_path && !::lstat(path.c_str(),&prior) && prior.st_dev==value.st_dev && prior.st_ino==value.st_ino && prior.st_ctimespec.tv_sec==value.st_ctimespec.tv_sec && prior.st_ctimespec.tv_nsec==value.st_ctimespec.tv_nsec;
        if(query_error && descriptor<0 && directory>=0 && !pinned)*query_error=native_path?1237U:native_error();
        auto size=descriptor>=0?::fgetxattr(descriptor,"com.x64arm.dosattributes",stored.data(),stored.size(),0,0):pinned?::getxattr(path.c_str(),"com.x64arm.dosattributes",stored.data(),stored.size(),0,XATTR_NOFOLLOW):-1;
        if(query_error && size<0 && (!*query_error) && (descriptor>=0 || pinned) && errno!=ENOATTR && errno!=ENOTSUP)*query_error=native_error();
        if(pinned && (::lstat(path.c_str(),&after)<0 || after.st_dev!=value.st_dev || after.st_ino!=value.st_ino || prior.st_ctimespec.tv_sec!=after.st_ctimespec.tv_sec || prior.st_ctimespec.tv_nsec!=after.st_ctimespec.tv_nsec)){size=-1;if(query_error)*query_error=1237;}

#else
        const auto path=directory>=0?"/proc/self/fd/"+std::to_string(directory)+"/"+std::string(leaf):std::string{};
        const auto size=descriptor>=0?::fgetxattr(descriptor,"user.x64arm.dosattributes",stored.data(),stored.size()): ::lgetxattr(path.c_str(),"user.x64arm.dosattributes",stored.data(),stored.size());
        if(query_error && size<0 && errno!=ENODATA && errno!=ENOTSUP)*query_error=native_error();
#endif
        if(size==8 && stored[0]=='x' && stored[1]=='6' && stored[2]=='4' && stored[3]==1) {
            std::uint32_t attributes=0;for(unsigned i=0;i<4;++i)attributes|=static_cast<std::uint32_t>(stored[4+i])<<(8*i);
            result.attributes=(attributes&0x3127U)|(S_ISDIR(value.st_mode)?0x10U:0U);
            if(!S_ISDIR(value.st_mode))result.attributes&=~1U;
        }
    }
#endif
    if (!(value.st_mode & S_IWUSR)) result.attributes |= 1;
    if(!result.attributes)result.attributes=0x80;
    if (S_ISLNK(value.st_mode)) result.attributes |= 0x400;
    result.allocation_size=value.st_blocks>0?static_cast<std::uint64_t>(value.st_blocks)*512:0;
    result.id = value.st_ino; result.device = static_cast<std::uint64_t>(value.st_dev); result.links = static_cast<std::uint32_t>(value.st_nlink);
#if defined(__APPLE__)
    result.created = filetime(value.st_birthtimespec); result.accessed = filetime(value.st_atimespec);
    result.modified = filetime(value.st_mtimespec); result.changed = filetime(value.st_ctimespec);
#else
    result.created = filetime(value.st_ctim); result.accessed = filetime(value.st_atim);
    result.modified = filetime(value.st_mtim); result.changed = filetime(value.st_ctim);
#endif
    return result;
}
std::uint32_t set_descriptor_attributes(int descriptor,const struct stat& info,std::uint32_t attributes) {
#if defined(__APPLE__) || defined(__linux__)
    std::array<std::uint8_t,8> stored{'x','6','4',1};
    attributes&=0x3127U;for(unsigned i=0;i<4;++i)stored[4+i]=static_cast<std::uint8_t>(attributes>>(8*i));
    // macOS 對唯讀檔案的延伸屬性更新也會檢查寫入權限；先暫時允許
    // 擁有者更新，再套用最終 DOS READONLY 對應的權限。
    const auto original_mode=static_cast<mode_t>(info.st_mode&07777);
    const auto final_mode=static_cast<mode_t>(S_ISREG(info.st_mode)?((attributes&1)?original_mode&~(S_IWUSR|S_IWGRP|S_IWOTH):original_mode|S_IWUSR):original_mode);
    const bool temporary_write=!(original_mode&S_IWUSR);
    if(temporary_write && ::fchmod(descriptor,static_cast<mode_t>(original_mode|S_IWUSR))<0)return native_error();
#if defined(__APPLE__)
    const auto updated=::fsetxattr(descriptor,"com.x64arm.dosattributes",stored.data(),stored.size(),0,0);
#else
    const auto updated=::fsetxattr(descriptor,"user.x64arm.dosattributes",stored.data(),stored.size(),0);
#endif
    if(updated<0) {
        const auto error=native_error();
        if(temporary_write)(void)::fchmod(descriptor,original_mode);
        return error;
    }
    if((temporary_write || final_mode!=original_mode) && ::fchmod(descriptor,final_mode)<0)return native_error();
    return 0;
#else
    return 50;
#endif
}

#endif
}
void ReadOnlyFiles::set_security_context(const security::FileTokenContext& context){
    set_security_context(context.token,context.primary_user,context.primary_group);
}
void ReadOnlyFiles::set_security_context(const VirtualToken& token,const std::vector<std::uint8_t>& primary_user,const std::vector<std::uint8_t>& primary_group){
    // 先比較借用的即時狀態；只有內容改變才複製不可變快照。
    // 不以 token ID 取代比較，跨程序權杖修改與 impersonation 仍立即生效。
    if(security_context_ && security_context_->token==token && security_context_->primary_user==primary_user && security_context_->primary_group==primary_group)return;
    security_context_=std::make_shared<const security::FileTokenContext>(security::FileTokenContext{token,primary_user,primary_group});
    constexpr char digits[]="0123456789abcdef";network_scope_.clear();for(auto b:token.user.sid){network_scope_+=digits[b>>4];network_scope_+=digits[b&15];}network_scope_+='-';for(unsigned n=0;n<16;++n)network_scope_+=digits[(token.authentication_id>>(n*4))&15U];
}
bool ReadOnlyFiles::network_path(std::string path)const{const auto found=network_files_->locate(network_scope_,utf8_to_utf16_lossy(path));return found.matched && !found.error;}
std::filesystem::path ReadOnlyFiles::network_mount_directory(std::string name){
    if(c_drive_.empty() || name.empty() || name.size()>80 || name.find_first_not_of("0123456789abcdef-")!=name.npos)throw std::invalid_argument("網路掛載名稱或 Windows 磁碟無效");
    security::SecurityDescriptor descriptor;
    if(security_context_){descriptor.control=0x1004;descriptor.data[0]=security_context_->token.user.sid;descriptor.parts[0]=1;descriptor.data[3]=security::encode_acl({security::encode_ace({0,3,0x1f01ff,security_context_->token.user.sid})});descriptor.parts[3]=1;}
    const auto private_sd=security_context_?security::relative_descriptor(descriptor):std::vector<std::uint8_t>{};
    std::string guest="C:/.x64arm-network";auto error=create_directory(guest);if(error && error!=183)throw SecurityDescriptorError{error};
    guest+='/'+network_scope_;error=create_directory(guest,[&]{return private_sd;});if(error && error!=183)throw SecurityDescriptorError{error};
    guest+='/'+name;error=create_directory(guest,[&]{return private_sd;});if(error && error!=183)throw SecurityDescriptorError{error};
    const auto info=information(guest);if(info.error)throw SecurityDescriptorError{info.error};return c_drive_/".x64arm-network"/network_scope_/name;
}
std::uint32_t ReadOnlyFiles::authorize(int descriptor,std::uint32_t desired)const{
    if(!security_context_)return 0;
    try{
#if !defined(_WIN32)
        const auto copy=::fcntl(descriptor,F_DUPFD_CLOEXEC,0);if(copy<0)return native_error();HostFile file(copy);
        (void)security::file_token_access(*security_context_,security::file_descriptor(security::read_native_file_security(file,true),0x17),desired);return 0;
#else
        (void)descriptor;(void)desired;return 50;
#endif
    }catch(const SecurityDescriptorError& error){return error.code;}catch(const std::bad_alloc&){return 8;}
}
std::uint32_t ReadOnlyFiles::authorize_at(int directory,std::string_view leaf,std::uint32_t desired)const{
    if(!security_context_)return 0;
    try{
        const auto sd=security::file_descriptor(security::read_native_file_security_at(directory,leaf,true),0x17);
        try{(void)security::file_token_access(*security_context_,sd,desired);return 0;}
        catch(const SecurityDescriptorError& error){
            if(error.code!=5 || !desired || (desired&~0x88U))return error.code;
            // NTFS 的父目錄 LIST_DIRECTORY 同時允許子項屬性／EA 查詢；不授予內容讀權。
            if((security::file_integrity_access(*security_context_,sd,desired)&desired)!=desired)return 5;
            return authorize(directory,1U);
        }
    }catch(const SecurityDescriptorError& error){return error.code;}catch(const std::bad_alloc&){return 8;}
}
std::uint32_t ReadOnlyFiles::authorize_delete(int directory,std::string_view leaf)const{
    if(!security_context_)return 0;
    try{
        const auto sd=security::file_descriptor(security::read_native_file_security_at(directory,leaf,true),0x17);
        if(!(security::file_integrity_access(*security_context_,sd,0x10000U)&0x10000U))return 5;
        try{(void)security::file_token_access(*security_context_,sd,0x10000U);return 0;}
        catch(const SecurityDescriptorError& error){if(error.code!=5)return error.code;return authorize(directory,0x40U);}
    }catch(const SecurityDescriptorError& error){return error.code;}catch(const std::bad_alloc&){return 8;}
}
std::uint32_t ReadOnlyFiles::inherit_file_label(int descriptor,int parent,bool directory)const{
    if(!security_context_)return 0;
    try{
#if !defined(_WIN32)
        const auto copy=::fcntl(descriptor,F_DUPFD_CLOEXEC,0);if(copy<0)return native_error();HostFile file(copy);
        const auto parent_sd=security::file_descriptor(security::read_native_file_security_at(parent,".",true),0x17);
        const auto inherited=security::inherit_descriptor({},parent_sd,security_context_->token.owner,security_context_->token.primary_group,directory);
        auto label=inherited.data[2];if(label.empty() && security_context_->token.integrity!=0x2000)label=security::integrity_label(security_context_->token.integrity);
        if(!label.empty())security::write_native_file_label(file,label);return 0;
#else
        (void)descriptor;(void)parent;(void)directory;return 50;
#endif
    }catch(const SecurityDescriptorError& error){return error.code;}catch(const std::bad_alloc&){return 8;}
}
std::vector<std::uint8_t> ReadOnlyFiles::creation_security(int parent,bool directory,std::span<const std::uint8_t> requested)const{
    if(!security_context_){if(!requested.empty())throw SecurityDescriptorError{50};return {};}
    const auto& context=*security_context_;
    auto child=requested.empty()?security::SecurityDescriptor{}:security::parse_relative_descriptor(requested);
    const auto inherited=security::file_descriptor(security::read_native_file_security_at(parent,".",true),0x17);
    child=security::inherit_descriptor(std::move(child),inherited,context.token.owner.empty()?context.token.user.sid:context.token.owner,context.token.primary_group,directory);
    if(!(child.control&4U)){child.control|=4;child.data[3]=context.token.default_dacl;}
    if(child.data[2].empty() && context.token.integrity!=0x2000){child.control|=16;child.data[2]=security::integrity_label(context.token.integrity);}
    return security::relative_descriptor(child);
}
std::uint32_t ReadOnlyFiles::initialize_file_security(int descriptor,std::span<const std::uint8_t> bytes)const{
    if(bytes.empty())return 0;
#if !defined(_WIN32)
    const auto copy=::fcntl(descriptor,F_DUPFD_CLOEXEC,0);if(copy<0)return native_error();HostFile file(copy);
    return initialize_file_security(file,bytes);
#else
    (void)descriptor;return 50;
#endif
}
std::uint32_t ReadOnlyFiles::initialize_file_security(const HostFile& file,std::span<const std::uint8_t> bytes)const{
    if(bytes.empty())return 0;
    try{
        const auto sd=security::parse_relative_descriptor(bytes);const auto& context=*security_context_;
        security::write_native_file_security(file,sd,7U|(sd.control&16U?16U:0U),context.primary_user,context.primary_group);return 0;
    }catch(const SecurityDescriptorError& error){return error.code;}catch(const std::bad_alloc&){return 8;}
}
FileSnapshot ReadOnlyFiles::open_library(std::string path,const std::filesystem::path& application,std::string_view application_guest_directory) {
    startup_profile::Scope measurement("library_file",path,true);
#if defined(_WIN32)
    (void)path;(void)application;(void)application_guest_directory;return {{},{},50};
#else
    auto resolved=resolve(path);
    if(resolved.key.empty())return {{},{},resolved.error,resolved.access_denied};
    auto key=resolved.key;
    if(key.starts_with("relative:/"))key="c:/"+key.substr(10);
    auto prefix=std::string(application_guest_directory);std::replace(prefix.begin(),prefix.end(),'\\','/');
    for(auto& c:prefix)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
    if(!prefix.empty() && !prefix.ends_with('/'))prefix+='/';
    // DLL 搜尋可讀主 EXE 所在目錄，但不把宿主絕對路徑交給客體，
    // 亦不將整個來源目錄加入可寫的 C 槽映射。
    if(!application.empty() && !prefix.empty() && key.starts_with(prefix)) {
        ReadOnlyFiles source({},application);
        source.security_context_=security_context_;
        auto adjacent=source.resolve("C:/"+key.substr(prefix.size()));
        // 相鄰 DLL 的實際存取拒絕不能被未掛載 C 槽的錯誤蓋過。
        if(!adjacent.error || adjacent.access_denied){
            FileDescriptor previous{resolved.descriptor};resolved=std::move(adjacent);
        }
    }
    FileDescriptor fd{resolved.descriptor};
    if(resolved.error)return {key,{},resolved.error,resolved.access_denied};
    if(const auto error=authorize(fd.value,1U))return {key,{},error,error==5};
    // 快取只接收上述已授權的描述元；同名檔案替換、相鄰來源、硬連結
    // 與不同 ReadOnlyFiles 實例以真正的檔案身分區分／共用，不能只相信 guest key。
    try {
        auto bytes=pe_file_cache().read(fd.value,256U*1024U*1024U);
        readonly_files_[key]=resolved.host_path;
        return {key,std::move(bytes),0,false,resolved.host_path};
    }catch(const std::length_error&){return {key,{},193};}
    catch(const std::invalid_argument&){return {key,{},193};}
    catch(const std::bad_alloc&){return {key,{},8};}
    catch(const std::system_error& error){const auto code=native_error(error.code().value());return {key,{},code,code==5};}
#endif
}
OpenHostFile ReadOnlyFiles::open_reader(std::string path,const std::filesystem::path& application,std::string_view application_guest_directory,std::uint32_t sharing) try {
#if defined(_WIN32)
    (void)path;(void)application;(void)application_guest_directory;(void)sharing;return {{},{},{},50,false};
#else
    // 唯讀來源能力維持既有根目錄邊界；一般 Windows 檔案 I/O 另依磁碟根目錄正規化。
    auto resolved=resolve(path,false,nullptr,false,false,0,true);FileDescriptor descriptor{resolved.descriptor};
    auto key=resolved.key;if(key.empty())return {{},{},{},resolved.error,false};
    if(key.starts_with("relative:/"))key="c:/"+key.substr(10);
    auto prefix=std::string(application_guest_directory);std::replace(prefix.begin(),prefix.end(),'\\','/');
    for(auto& c:prefix)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
    if(!prefix.empty() && !prefix.ends_with('/'))prefix+='/';
    // 沿用 DLL 相鄰目錄的唯讀能力；純查詢不登錄 DLL、不改 writable 路徑權限。
    if(!application.empty() && !prefix.empty() && key.starts_with(prefix)) {
        ReadOnlyFiles source({},application);source.security_context_=security_context_;auto adjacent=source.resolve("C:/"+key.substr(prefix.size()));
        if(!adjacent.error) {if(descriptor.value>=0)::close(descriptor.value);descriptor.value=adjacent.descriptor;resolved.error=0;}
    }
    if(resolved.error)return {key,{},{},resolved.error,false};
    if(const auto error=authorize(descriptor.value,0x80000000U))return {key,{},{},error,false};
    struct stat info{};if(::fstat(descriptor.value,&info)<0)return {key,{},{},native_error(),false};
    if(!S_ISREG(info.st_mode) || info.st_size<0)return {key,{},{},5,false};
    const auto details=metadata(info,descriptor.value);if(const auto error=sharing_error(details,0x80000000U,sharing))return {key,{},{},error,true};
    auto state=file_state(details,true);auto file=std::make_shared<HostFile>(descriptor.value);descriptor.value=-1;
    file->state_=state;file->access_=0x80000000U;file->sharing_=sharing;file->readonly_=true;file->volume_=resolved.readonly_file?0:resolved.volume;file->network_lease_=resolved.network_lease;if(const auto error=attach_shared_file(*file))return {key,{},{},error,false};std::erase_if(state->opens,[](const auto& weak){return weak.expired();});state->opens.push_back(file);
    return {key,std::move(file),details,0,true};
#endif
} catch(const std::bad_alloc&) {return {{},{},{},8,false};}
std::uint32_t ReadOnlyFiles::set_current_directory(std::string path) {
#if !defined(_WIN32)
    if((path=="C:\\" || path=="C:/") && c_drive_.empty() && (!root_.empty() || !readonly_files_.empty())){current_directory_.clear();return 0;}
    const auto resolved=resolve(path,true);if(resolved.error)return resolved.error;
    if(const auto error=authorize(resolved.descriptor,0x20U)){::close(resolved.descriptor);return error;}
    ::close(resolved.descriptor);
    if(path.find(':')==std::string::npos && !path.starts_with("/") && !path.starts_with("\\"))path=(current_directory_.empty()?"C:/":current_directory_+"/")+path;
    if(!path.empty() && (path.front()=='/' || path.front()=='\\') && resolved.key.size()>1 && resolved.key[1]==':')path=resolved.key.substr(0,2)+path;
    current_directory_=std::move(path);return 0;
#else
    (void)path;return 50;
#endif
}
std::pair<std::filesystem::path,std::uint32_t> ReadOnlyFiles::executable_path(std::string path,const std::filesystem::path& application,std::string_view guest_directory) {
    const auto opened = open_library(std::move(path),application,guest_directory);
    if (opened.error) return {{},opened.error};
    const auto found = readonly_files_.find(opened.key);
    return found == readonly_files_.end() ? std::pair<std::filesystem::path,std::uint32_t>{{},2} : std::pair{found->second,0U};
}
bool HostFile::transferable()const{
#if !defined(_WIN32)
    struct stat info{};
    // 尚未共用 delete disposition 與 range-lock 的生命週期，不能降級成普通 open。
#if defined(__APPLE__)
    return locks_.empty() && !::fstat(descriptor_,&info) && S_ISREG(info.st_mode);
#else
    return !delete_on_close_ && !delete_pending() && !(access_&(0x10000U|0x10000000U)) && locks_.empty() && !::fstat(descriptor_,&info) && S_ISREG(info.st_mode);
#endif
#else
    return false;
#endif
}
OpenHostFile ReadOnlyFiles::inherit_host(int descriptor,std::uint32_t access,std::uint32_t sharing,bool readonly,std::uint64_t object){
#if defined(_WIN32)
    (void)descriptor;(void)access;(void)sharing;(void)readonly;(void)object;return {{},{},{},50,false};
#else
    if(sharing&~7U)return {{},{},{},87,false};
    struct stat info{};const auto flags=::fcntl(descriptor,F_GETFL);
    if(flags<0 || ::fstat(descriptor,&info) || !S_ISREG(info.st_mode))return {{},{},{},6,false};
    const bool reads=(access&(0x80000000U|1U))!=0,writes=(access&(0x40000000U|2U|4U))!=0;
    if((writes && (readonly || (flags&O_ACCMODE)==O_RDONLY)) || (reads && (flags&O_ACCMODE)==O_WRONLY))return {{},{},{},5,false};
    const auto details=metadata(info,descriptor);const auto copied=::fcntl(descriptor,F_DUPFD_CLOEXEC,3);if(copied<0)return {{},{},{},native_error(),false};
    FileDescriptor owned{copied};auto state=file_state(details,true);auto file=std::make_shared<HostFile>(owned.value);owned.value=-1;
    file->state_=state;file->access_=access;file->sharing_=sharing;file->readonly_=readonly;
    if(const auto error=attach_shared_file(*file,object,true))return {{},{},{},error,false};
    std::erase_if(state->opens,[](const auto& weak){return weak.expired();});state->opens.push_back(file);
    return {{},std::move(file),details,0,true};
#endif
}
int HostFile::duplicate_descriptor() const {
#if !defined(_WIN32)
    return ::fcntl(descriptor_,F_DUPFD_CLOEXEC,0);
#else
    return -1;
#endif
}
OpenHostFile ReadOnlyFiles::open_directory_observer(std::string path) {
    const auto resolved=resolve(std::move(path),true);
    if(resolved.error)return {resolved.key,{},{},resolved.error,false};
#if !defined(_WIN32)
    // 監看只需讀取目錄；沿用 resolve 的磁碟映射與逐段 O_NOFOLLOW 邊界。
    FileDescriptor descriptor{resolved.descriptor};
    if(const auto error=authorize(descriptor.value,1U))return {resolved.key,{},{},error,false};
    auto file=std::make_shared<HostFile>(descriptor.value);descriptor.value=-1;file->volume_=resolved.volume;file->network_lease_=resolved.network_lease;
    const auto info=file->information();
    return {resolved.key,file,info.metadata,info.error,true};
#else
    return {resolved.key,{},{},50,false};
#endif
}
FileInformation ReadOnlyFiles::information(std::string path) {
    security::NativeFileIdentityScope identity_scope;
#if !defined(_WIN32)
    // 一般檔案不可能是重解析點；同一次解析確認葉節點是一般檔案時，略過另一輪 reparse 解析。
    // 連結、目錄、唯讀映射根及解析失敗都維持原本先查 reparse 的流程。
    {
        auto resolved=resolve(path,false,nullptr,true);
        struct stat probe{};
        if(!resolved.error && !resolved.security_leaf.empty() && ::fstatat(resolved.descriptor,resolved.security_leaf.c_str(),&probe,AT_SYMLINK_NOFOLLOW)==0 && S_ISREG(probe.st_mode))
            return information_resolved(std::move(resolved));
        if(resolved.descriptor>=0)::close(resolved.descriptor);
    }
#endif
    const auto link=reparse(path);if(!link.error){std::string leaf;const auto parent=resolve(path,true,&leaf);
#if !defined(_WIN32)
        FileDescriptor fd{parent.descriptor};struct stat entry{};if(parent.error)return {parent.key,{},parent.error};
        if(const auto error=authorize(fd.value,0x80U))return {parent.key,{},error};
        if(::fstatat(fd.value,leaf.c_str(),&entry,AT_SYMLINK_NOFOLLOW)<0)return {parent.key,{},native_error()};
        if(static_cast<std::uint64_t>(entry.st_dev)!=link.device || entry.st_ino!=link.id)return {parent.key,{},32};
        FileDescriptor own{link.kind==3?::openat(fd.value,leaf.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW):-1};
        auto info=metadata(entry,own.value);info.attributes=link.kind==3?info.attributes|0x400U:link.attributes;info.reparse_tag=link.kind==3?0xa0000003U:0xa000000cU;return {parent.key,info,0};
#endif
    }
    return information_resolved(resolve(std::move(path),false,nullptr,true));
}
FileInformation ReadOnlyFiles::information_resolved(Resolved resolved) {
    if((resolved.error==2 || resolved.error==3 || resolved.error==5) && std::any_of(readonly_mounts_.begin(),readonly_mounts_.end(),[&](const auto& entry){return entry.first.starts_with(resolved.key+(resolved.key.ends_with('/')?"":"/"));})) {FileMetadata info;info.attributes=0x10;return {resolved.key,info,0};}
    if(resolved.error && resolved.key=="c:/" && c_drive_.empty() && (!install_root_.empty() || !readonly_files_.empty())) {FileMetadata info;info.attributes=0x10;return {resolved.key,info,0};}
    if (resolved.error) return {resolved.key, {}, resolved.error};
#if defined(_WIN32)
    return {resolved.key, {}, 50};
#else
    FileDescriptor parent{resolved.descriptor};
    const auto name=resolved.security_leaf.empty()?".":resolved.security_leaf.c_str();
    if(const auto error=authorize_at(parent.value,name,0x80U))return {resolved.key,{},error};
    if (const auto found = cache_->metadata.find(resolved.key); !writable_ && found != cache_->metadata.end()) return {resolved.key, found->second, 0};
    // 屬性查詢不要求內容／目錄列舉；所有路徑查詢保留 NOFOLLOW 與 inode 驗證。
    struct stat value{};if(::fstatat(parent.value,name,&value,AT_SYMLINK_NOFOLLOW)<0)return {resolved.key,{},native_error()};
#if defined(__APPLE__)
    FileDescriptor fd{::openat(parent.value,name,O_EVTONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
#else
    FileDescriptor fd{::openat(parent.value,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
#endif
    if(fd.value<0 && errno!=EACCES && errno!=EPERM)return {resolved.key,{},native_error()};
    if(fd.value>=0){struct stat identity{};if(::fstat(fd.value,&identity)<0)return {resolved.key,{},native_error()};if(value.st_dev!=identity.st_dev || value.st_ino!=identity.st_ino)return {resolved.key,{},1237};}
    std::uint32_t metadata_error{};const auto info=metadata(value,fd.value,parent.value,name,&metadata_error);if(metadata_error)return {resolved.key,{},metadata_error};
    struct stat after{};if(::fstatat(parent.value,name,&after,AT_SYMLINK_NOFOLLOW)<0)return {resolved.key,{},native_error()};
#if defined(__APPLE__)
    const auto before_change=value.st_ctimespec,after_change=after.st_ctimespec;
#else
    const auto before_change=value.st_ctim,after_change=after.st_ctim;
#endif
    if(value.st_dev!=after.st_dev || value.st_ino!=after.st_ino || value.st_mode!=after.st_mode || before_change.tv_sec!=after_change.tv_sec || before_change.tv_nsec!=after_change.tv_nsec)return {resolved.key,{},1237};
    cache_->metadata.emplace(resolved.key, info);
    return {resolved.key, info, 0};
#endif
}
DirectorySnapshot ReadOnlyFiles::list(std::string path) {return list(std::move(path),{});}
DirectorySnapshot ReadOnlyFiles::list(std::string path,const std::function<bool(std::string_view)>& include) {
    security::NativeFileIdentityScope identity_scope;
    const auto wanted=[&](std::string_view name){return !include || include(name);};
    auto resolved = resolve(std::move(path), true);
    const bool virtual_program_files=resolved.key=="c:/" && !install_root_.empty();
    const auto overlays=[&](DirectorySnapshot& entries) {
        auto add=[&](std::string name,const auto& data) {
            // 補入項目仍取代同名（不分大小寫）的既有項目；只有符合條件者才查詢中繼資料。
            std::erase_if(entries.entries,[&](const auto& entry){return directory_overlay::same_name(entry.name,name);});if(wanted(name))entries.entries.push_back({std::move(name),data()});
        };
        // 主 EXE 的伴隨資源唯讀補入；明確映射中的同名檔案優先。
        for(const auto& [mount,host]:application_sources_){
            const auto directory=resolved.key+(resolved.key.ends_with('/')?"":"/");if(!directory.starts_with(mount))continue;
            ReadOnlyFiles source({},host);source.security_context_=security_context_;const auto extra=source.list("C:/"+directory.substr(mount.size()),include);if(extra.error)continue;
            directory_overlay::append_missing(entries.entries,extra.entries);
        }
        if(virtual_program_files)add("Program Files",[&]{return information("C:/Program Files").metadata;});
        const auto prefix=resolved.key+(resolved.key.ends_with('/')?"":"/");
        for(const auto& [key,file]:readonly_files_)if(key.starts_with(prefix) && key.find('/',prefix.size())==std::string::npos)add(file.filename().string(),[&]{return information(key).metadata;});
        for(const auto& [mount,host]:readonly_mounts_){
            (void)host;if(!mount.starts_with(prefix) || mount==prefix)continue;
            const auto end=mount.find('/',prefix.size());if(end==std::string::npos)continue;
            const auto child=mount.substr(prefix.size(),end-prefix.size());add(child,[&]{return information(prefix+child).metadata;});
        }
    };
    if((resolved.error==2 || resolved.error==3 || resolved.error==5) && std::any_of(readonly_mounts_.begin(),readonly_mounts_.end(),[&](const auto& entry){return entry.first.starts_with(resolved.key+(resolved.key.ends_with('/')?"":"/"));})) {DirectorySnapshot result;overlays(result);return result;}
    if(resolved.error && resolved.key=="c:/" && c_drive_.empty() && (virtual_program_files || !readonly_files_.empty())) {DirectorySnapshot result;overlays(result);return result;}
    if (resolved.error) return {{}, resolved.error};
#if defined(_WIN32)
    return {{}, 50};
#else
    FileDescriptor fd{resolved.descriptor};
    if(const auto error=authorize(fd.value,1U))return {{},error};
    if (const auto found = cache_->directories.find(resolved.key); !writable_ && found != cache_->directories.end()) {
        if(!include)return found->second;
        DirectorySnapshot filtered;for(const auto& entry:found->second.entries)if(include(entry.name))filtered.entries.push_back(entry);return filtered;
    }
    struct DirectoryStream { DIR* value; ~DirectoryStream() { if (value) ::closedir(value); } } entries{::fdopendir(fd.value)};
    if (!entries.value) return {{}, native_error()};
    fd.value = -1; DirectorySnapshot result;
    for (;;) {
        errno = 0; const auto* item = ::readdir(entries.value);
        if (!item) { if (errno) return {{}, native_error()}; break; }
        const std::string name = item->d_name;
        if (name == "." || name == ".." || !wanted(name)) continue;
        struct stat info{};
        if (::fstatat(::dirfd(entries.value), name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) return {{}, native_error()};
        if (result.entries.size() >= 100000) return {{}, 8};
        FileDescriptor entry{::openat(::dirfd(entries.value),name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK)};
        auto details=metadata(info,entry.value);
        if(S_ISLNK(info.st_mode)){const auto link=file_reparse::read(::dirfd(entries.value),name);if(!link.error){details.attributes=link.attributes;details.reparse_tag=0xa000000cU;}}
        else if(S_ISDIR(info.st_mode) && entry.value>=0){const auto point=volumes_->reparse(entry.value);if(!point.error){details.attributes|=0x400U;details.reparse_tag=0xa0000003U;}else if(point.error!=4390 && point.error!=50)return {{},point.error};}
        result.entries.push_back({name,details});
    }
    overlays(result);
    std::sort(result.entries.begin(), result.entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    if(!include)cache_->directories.emplace(resolved.key, result);
    return result;
#endif
}
FileSnapshot ReadOnlyFiles::open(std::string path) {
    security::NativeFileIdentityScope identity_scope;
    auto resolved = resolve(std::move(path));
    if (resolved.error) return {resolved.key, {}, resolved.error};
    const auto& key = resolved.key;
#if defined(_WIN32)
    return {key, {}, 50};
#else
    FileDescriptor file{resolved.descriptor};
    if(const auto error=authorize(file.value,1U))return {key,{},error};
    if (const auto found = cache_->files.find(key); !writable_ && found != cache_->files.end()) return {key, found->second, 0};
    if (cache_->files.size() >= 4096) return {key, {}, 8};
    struct stat information{};
    if (::fstat(file.value, &information) != 0) return {key, {}, native_error()};
    if (!S_ISREG(information.st_mode)) return {key, {}, 5};
    if(writable_)if(const auto error=sharing_error(metadata(information,file.value),0x80000000U,7))return {key,{},error};
    constexpr std::size_t file_limit = 16U * 1024U * 1024U, total_limit = 128U * 1024U * 1024U;
    if (information.st_size < 0 || static_cast<std::uint64_t>(information.st_size) > file_limit ||
        static_cast<std::uint64_t>(information.st_size) > total_limit - cache_->bytes) return {key, {}, 8};
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(information.st_size));
    std::size_t offset = 0;
    while (offset < bytes->size()) {
        const auto count = ::read(file.value, bytes->data() + offset, bytes->size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return {key, {}, 1117};
        offset += static_cast<std::size_t>(count);
    }
    std::uint8_t extra{}; ssize_t trailing;
    do { trailing = ::read(file.value, &extra, 1); } while (trailing < 0 && errno == EINTR);
    if (trailing != 0) return {key, {}, 1117};
    if (!writable_) {cache_->files.emplace(key, bytes); cache_->bytes += bytes->size(); cache_->metadata.emplace(key, metadata(information,file.value));}
    return {key, std::move(bytes), 0};
#endif
}
HostSecurityTarget::~HostSecurityTarget(){
#if !defined(_WIN32)
    if(directory>=0)::close(directory);
#endif
}
HostFile::~HostFile() {
    if(delete_on_close_ && delete_target_ && !shared_lease_) {delete_target_->pending=true;state_->pending=true;}
#if !defined(_WIN32)
    if (descriptor_>=0) ::close(descriptor_);
#endif
}
FileInformation HostFile::information() const {
#if defined(_WIN32)
    return {{},{},50};
#else
    if(security_target_){
        struct stat current{};if(::fstatat(security_target_->directory,security_target_->leaf.c_str(),&current,AT_SYMLINK_NOFOLLOW)<0)return {{},{},native_error()};
        if(static_cast<std::uint64_t>(current.st_dev)!=security_target_->identity.device || current.st_ino!=security_target_->identity.id)return {{},{},1237};
        return {{},metadata(current),0};
    }
    return descriptor_information(descriptor_);
#endif
}
FileInformation HostFile::descriptor_information(int descriptor) {
#if defined(_WIN32)
    (void)descriptor;return {{},{},50};
#else
    struct stat info{};if (::fstat(descriptor,&info)<0) return {{},{},native_error()};return {{},metadata(info,descriptor),0};
#endif
}
std::uint32_t HostFile::validate_io(std::uint64_t offset,std::uint64_t length,bool writing) const {
    if(offset>INT64_MAX || length>static_cast<std::uint64_t>(INT64_MAX)-offset)return 87;
    return lock_conflict(offset,length,writing);
}
FileIoResult HostFile::read(std::span<std::uint8_t> bytes,std::uint64_t offset) const {
#if defined(_WIN32)
    (void)bytes;(void)offset;return {0,50};
#else
    if(const auto error=validate_io(offset,bytes.size(),false))return {0,error};
    ssize_t size;do {size=::pread(descriptor_,bytes.data(),bytes.size(),static_cast<off_t>(offset));} while(size<0 && errno==EINTR);
    process_io::record(process_io::Kind::Read,size<0?0:static_cast<std::uint64_t>(size));
    return size<0?FileIoResult{0,native_error()}:FileIoResult{static_cast<std::uint64_t>(size),0};
#endif
}
FileIoResult HostFile::write(std::span<const std::uint8_t> bytes,std::uint64_t offset) const {
#if defined(_WIN32)
    (void)bytes;(void)offset;return {0,50};
#else
    if(const auto error=validate_io(offset,bytes.size(),true))return {0,error};
    FileIoResult output;
    output.error=mutate_size([&](){
        struct stat info{};if(::fstat(descriptor_,&info)<0)return native_error();std::uint64_t previous=0;bool managed=false;
        if(const auto error=valid_data(descriptor_,static_cast<std::uint64_t>(std::max<off_t>(0,info.st_size)),previous,managed))return error;
        ssize_t size;do {size=::pwrite(descriptor_,bytes.data(),bytes.size(),static_cast<off_t>(offset));} while(size<0 && errno==EINTR);
        process_io::record(process_io::Kind::Write,size<0?0:static_cast<std::uint64_t>(size));if(size<0)return native_error();output.bytes=static_cast<std::uint64_t>(size);
        return managed && size>0 && offset+output.bytes>previous?store_valid_data(descriptor_,offset+output.bytes):0U;
    });return output;
#endif
}
std::uint32_t HostFile::truncate(std::uint64_t size) const {
#if defined(_WIN32)
    (void)size;return 50;
#else
    if(readonly_)return 5;if(size>INT64_MAX)return 87;
    return mutate_size([&](){
        struct stat info{};if(::fstat(descriptor_,&info)<0)return native_error();if(!S_ISREG(info.st_mode))return 5U;
        const auto original=static_cast<std::uint64_t>(std::max<off_t>(0,info.st_size));std::uint64_t previous=0;bool managed=false;
        if(const auto error=valid_data(descriptor_,original,previous,managed))return error;
        // 擴大 EOF 只建立未寫入範圍；既有 VDL 保留，宿主 sparse backing 仍讀為零。
        if(size>original && !managed){if(const auto error=store_valid_data(descriptor_,previous))return error;managed=true;}
        const auto result=::ftruncate(descriptor_,static_cast<off_t>(size));process_io::record(process_io::Kind::Other);if(result<0)return native_error();
        return managed && size<previous?store_valid_data(descriptor_,size):0U;
    });
#endif
}
std::uint32_t HostFile::mutate_size(const std::function<std::uint32_t()>& operation) const{
    if(state_ && state_->shared)return state_->shared->mutate_size(operation);
    if(state_){std::lock_guard lock(state_->size_mutex);return operation();}
    return operation();
}
std::uint32_t HostFile::set_valid_data(std::uint64_t size) const{
    if(readonly_ || !security_writable())return 5;
    if(!size || size>INT64_MAX)return 87;
    return mutate_size([&](){
        const auto info=information();if(info.error)return info.error;
        if(info.metadata.attributes&(0x10U|0x200U|0x800U))return 87U;
        std::uint64_t previous=0;bool managed=false;if(const auto error=valid_data(descriptor_,info.metadata.size,previous,managed))return error;
        if(size<=previous || size>info.metadata.size)return 87U;
        // 取得真正配置的 backing，再提交 VDL；不更動 EOF、不讀取宿主未配置磁區。
        if(size>info.metadata.allocation_size)if(const auto error=allocate(info.metadata.size))return error;
        return store_valid_data(descriptor_,size);
    });
}
std::uint32_t HostFile::flush() const {
#if defined(_WIN32)
    return 50;
#else
    const auto result=::fsync(descriptor_);process_io::record(process_io::Kind::Other);return result<0?native_error():0;
#endif
}
std::uint32_t HostFile::lock_conflict(std::uint64_t offset,std::uint64_t length,bool writing) const {
#if defined(F_OFD_GETLK)
    if(!length)return 0;
    if(state_ && state_->shared){if(const auto code=state_->shared->range_conflict(transfer_identity(),offset,length,writing))return code;}
    else if(state_)for(const auto& weak:state_->opens)if(const auto peer=weak.lock())for(const auto& held:peer->locks_)
        if(offset<held.offset+held.length && held.offset<offset+length && (peer.get()==this?writing && !held.exclusive:writing || held.exclusive))return 33;
    // 共用鎖亦限制持鎖 handle 自己寫入；獨佔鎖允許持鎖者讀寫。
    if(!state_ && writing && std::any_of(locks_.begin(),locks_.end(),[&](auto lock){return !lock.exclusive && offset<lock.offset+lock.length && lock.offset<offset+length;}))return 33;
    struct flock lock{};lock.l_type=writing?F_WRLCK:F_RDLCK;lock.l_whence=SEEK_SET;lock.l_start=static_cast<off_t>(offset);lock.l_len=static_cast<off_t>(length);
    if(::fcntl(descriptor_,F_OFD_GETLK,&lock)<0)return native_error();return lock.l_type==F_UNLCK?0:33;
#else
    (void)offset;(void)length;(void)writing;return 0;
#endif
}
std::uint32_t HostFile::lock_range(std::uint64_t offset,std::uint64_t length,bool exclusive,bool unlock) {
#if defined(F_OFD_SETLK)
    if(!length || offset>INT64_MAX || length>UINT64_MAX-offset)return 87;
    const auto conflict=[&](const auto& lock){return offset<lock.offset+lock.length && lock.offset<offset+length;};
    const auto native=[&]()->std::uint32_t {
    const auto found=unlock?std::find_if(locks_.begin(),locks_.end(),[&](auto lock){return lock.offset==offset && lock.length==length;}):locks_.end();
    if(unlock && found==locks_.end())return 158;
    const bool native_exclusive=unlock?found->exclusive:exclusive;
    const auto flags=::fcntl(descriptor_,F_GETFL);if(flags<0)return native_error();
    const auto access=flags&O_ACCMODE;
    // Windows 允許唯讀 handle 持獨佔鎖；Sandbox 的共用紀錄保留完整契約。
    // 原始 inode 鎖仍保護可對應的宿主讀寫，不替客體擴張檔案存取權。
    const bool skip=!native_exclusive && access==O_WRONLY;
    const auto type=native_exclusive && access!=O_RDONLY?F_WRLCK:F_RDLCK;
    if((skip || (native_exclusive && access==O_RDONLY)) && !state_)return 6;
    if(!unlock){
        if(std::any_of(locks_.begin(),locks_.end(),[&](auto lock){return conflict(lock) && (exclusive || lock.exclusive);} ))return 33;
        if(state_ && !state_->shared)for(const auto& weak:state_->opens)if(const auto peer=weak.lock())if(peer.get()!=this)
            for(const auto& held:peer->locks_)if(conflict(held) && (exclusive || held.exclusive))return 33;
        if(locks_.size()>=1024)return 8;
        struct flock lock{};lock.l_type=static_cast<decltype(lock.l_type)>(type);lock.l_whence=SEEK_SET;lock.l_start=static_cast<off_t>(offset);lock.l_len=length>static_cast<std::uint64_t>(INT64_MAX)-offset?0:static_cast<off_t>(length);
        if(!skip && ::fcntl(descriptor_,F_OFD_SETLK,&lock)<0)return errno==EAGAIN || errno==EACCES?33:native_error();
        locks_.push_back({offset,length,exclusive});return 0;
    }
    // 僅解除未被其他共用鎖覆蓋的區段，避免先全解鎖再重鎖的競態。
    std::vector<std::pair<std::uint64_t,std::uint64_t>> covered;
    for(auto it=locks_.begin();it!=locks_.end();++it)if(it!=found && conflict(*it))covered.emplace_back(std::max(offset,it->offset),std::min(offset+length,it->offset+it->length));
    std::sort(covered.begin(),covered.end());covered.emplace_back(offset+length,offset+length);auto begin=offset;
    for(auto [low,high]:covered){if(low>begin && !skip){struct flock lock{};lock.l_type=F_UNLCK;lock.l_whence=SEEK_SET;lock.l_start=static_cast<off_t>(begin);lock.l_len=low>static_cast<std::uint64_t>(INT64_MAX)?0:static_cast<off_t>(low-begin);if(::fcntl(descriptor_,F_OFD_SETLK,&lock)<0)return native_error();}begin=std::max(begin,high);}
    locks_.erase(found);return 0;
    };
    if(state_ && state_->shared)return state_->shared->range_lock(transfer_identity(),offset,length,exclusive,unlock,native);
    return native();
#else
    (void)offset;(void)length;(void)exclusive;(void)unlock;return 50;
#endif
}
std::uint32_t HostFile::set_times(std::uint64_t created,std::uint64_t accessed,std::uint64_t modified) const {
#if defined(_WIN32)
    (void)created;(void)accessed;(void)modified;return 50;
#else
    const auto specified=[](std::uint64_t value){return value && value!=UINT64_MAX;};
    const auto convert=[&](std::uint64_t value){return specified(value)?timespec{static_cast<time_t>(value/10000000)-INT64_C(11644473600),static_cast<long>((value%10000000)*100)}:timespec{0,UTIME_OMIT};};
    const timespec times[]{convert(accessed),convert(modified)};
    if(::futimens(descriptor_,times)<0)return native_error();
    if(specified(created)) {
#if defined(__APPLE__)
        attrlist attributes{};attributes.bitmapcount=ATTR_BIT_MAP_COUNT;attributes.commonattr=ATTR_CMN_CRTIME;
        auto time=convert(created);if(::fsetattrlist(descriptor_,&attributes,&time,sizeof(time),0)<0)return native_error();
#else
        return 50;
#endif
    }
    return 0;
#endif
}
OpenHostFile ReadOnlyFiles::open_host(std::string path,std::uint32_t access,std::uint32_t disposition,std::uint32_t flags_value,std::uint32_t sharing,const std::function<std::vector<std::uint8_t>()>& creation,const std::function<OpenHostFile()>& file_template) try {
    security::NativeFileIdentityScope identity_scope;
    if(!writable_)return {{},{},{},50,false};
    if(flags_value&0x00200000U){const auto point=reparse(path);if(!point.error)return {{},{},{},50,true};}
    const auto followed=follow_reparse(path);if(followed.second && followed.second!=50)return {{},{},{},followed.second,false};
    if(!followed.second)path=followed.first;
    const bool deleting=(flags_value&0x04000000U)!=0;
    if(deleting)access|=0x10000U;
#if !defined(_WIN32)
    if(!readonly_files_.empty() || !readonly_mounts_.empty() || network_path(path)) {
        auto mapped=resolve(path);FileDescriptor descriptor{mapped.descriptor};
        if(mapped.readonly_file) {
            if(mapped.error)return {mapped.key,{},{},mapped.error,false};
            constexpr std::uint32_t read_access=0x80000000U|0x20000000U|1U|8U|0x20U|0x80U|0x20000U|0x100000U;
            if((access&~read_access) || disposition!=3)return {mapped.key,{},{},5,true};
            if(const auto error=authorize(descriptor.value,access))return {mapped.key,{},{},error,true};
            struct stat info{};if(::fstat(descriptor.value,&info)<0)return {mapped.key,{},{},native_error(),true};
            const auto details=metadata(info,descriptor.value);if(const auto error=sharing_error(details,access,sharing))return {mapped.key,{},{},error,true};
            auto state=file_state(details,true);
            auto file=std::make_shared<HostFile>(descriptor.value);descriptor.value=-1;
            file->state_=state;file->access_=access;file->sharing_=sharing;file->readonly_=true;file->network_lease_=mapped.network_lease;if(const auto error=attach_shared_file(*file))return {mapped.key,{},{},error,true};std::erase_if(state->opens,[](const auto& weak){return weak.expired();});state->opens.push_back(file);
            return {mapped.key,file,file->information().metadata,0,true};
        }
    }
#endif
    std::string leaf;auto parent=resolve(path,true,&leaf,false,!(flags_value&0x00200000U));
    if(parent.error==5 && parent.key.size()==3 && parent.key[1]==':' && parent.key[2]=='/' && disposition==3 && (flags_value&0x02000000U)){
        auto root=resolve(std::move(path),false,nullptr,false,!(flags_value&0x00200000U));if(!root.error){parent=std::move(root);leaf=".";}
    }
    if(parent.error)return {parent.key,{},{},parent.error,false};
#if defined(_WIN32)
    (void)access;(void)disposition;(void)flags_value;(void)sharing;(void)creation;(void)file_template;return {parent.key,{},{},50,false};
#else
    FileDescriptor directory{parent.descriptor};struct stat previous{};
    bool existed=::fstatat(directory.value,leaf.c_str(),&previous,AT_SYMLINK_NOFOLLOW)==0;
    if(!existed && errno!=ENOENT)return {parent.key,{},{},native_error(),false};
    if(existed && disposition==1)return {parent.key,{},{},80,true};
    if(existed && !S_ISREG(previous.st_mode) && !S_ISDIR(previous.st_mode))return {parent.key,{},{},5,true};
    if(existed)if(const auto error=sharing_error(metadata(previous),access,sharing))return {parent.key,{},{},error,true};
    if(deleting && ((flags_value&1U) || (existed && !(previous.st_mode&S_IWUSR)) || ::faccessat(directory.value,".",W_OK|X_OK,0)<0))return {parent.key,{},{},5,existed};
    // 目錄的 delete disposition／非空檢查另有契約；目前只接受一般檔案。
    if(deleting && existed && S_ISDIR(previous.st_mode))return {parent.key,{},{},50,true};
    const bool readable=(access&(0x80000000U|1U))!=0,writable=(access&(0x40000000U|2U|4U))!=0;
    if(existed && writable && (metadata(previous).attributes&1U))return {parent.key,{},{},5,true};
    if(disposition<1 || disposition>5 || ((disposition==2 || disposition==5) && !writable))return {parent.key,{},{},87,existed};
    int flags=O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK|(writable?(readable?O_RDWR:O_WRONLY):O_RDONLY);if(flags_value&0x80000000U)flags|=O_SYNC;
#if defined(__APPLE__)
    if(!readable && !writable)flags=(flags&~O_ACCMODE)|O_EVTONLY;
#endif
    if(existed && S_ISDIR(previous.st_mode)) {if(!(flags_value&0x02000000U) || writable || disposition!=3)return {parent.key,{},{},5,true};flags|=O_DIRECTORY;}
    FileDescriptor fd;std::vector<std::uint8_t> security;
    const bool can_create=disposition==1 || disposition==2 || disposition==4;
    if(existed && disposition!=1)fd.value=::openat(directory.value,leaf.c_str(),flags);
    if(fd.value<0 && can_create && (!existed || disposition==1 || errno==ENOENT)){
        existed=false;
        if(const auto error=authorize(directory.value,2U))return {parent.key,{},{},error,false};
        security=creation_security(directory.value,false,creation?creation():std::vector<std::uint8_t>{});
        fd.value=::openat(directory.value,leaf.c_str(),flags|O_CREAT|O_EXCL,0666);existed=fd.value<0 && errno==EEXIST;
        if(existed && disposition!=1)fd.value=::openat(directory.value,leaf.c_str(),flags);
        else if(existed)return {parent.key,{},{},80,true};
    }else if(fd.value<0 && !existed && !can_create)fd.value=::openat(directory.value,leaf.c_str(),flags);
    if(fd.value<0){
        const auto host_errno=errno;const auto error=native_error();
        if(std::getenv("X64ARM_TRACE_SECURITY_ERRORS") && error==5){
            const auto guest_error=existed?authorize_at(directory.value,leaf,access):authorize(directory.value,2U);
            std::fprintf(stderr,"[security_open_error] path=%s access=%x disposition=%u flags=%x existed=%u native_error=%d guest_error=%u\n",parent.key.c_str(),access,disposition,flags_value,existed?1U:0U,host_errno,guest_error);
        }
        errno=host_errno;return {parent.key,{},{},error,existed};
    }
    struct stat info{};if(::fstat(fd.value,&info)<0)return {parent.key,{},{},native_error(),existed};
    struct IncompleteOpen {
        int directory;const std::string& leaf;const struct stat& identity;bool cleanup;
        ~IncompleteOpen() {struct stat current{};if(cleanup && ::fstatat(directory,leaf.c_str(),&current,AT_SYMLINK_NOFOLLOW)==0 && current.st_dev==identity.st_dev && current.st_ino==identity.st_ino)(void)::unlinkat(directory,leaf.c_str(),0);}
    } incomplete{directory.value,leaf,info,!existed};
    if(!existed) {
        if(file_template){
            const auto original=file_template();if(original.error)return {parent.key,{},{},original.error,false};
            if(!original.file)return {parent.key,{},{},6,false};
            if(const auto error=authorize(original.file->descriptor_,0x88U))return {parent.key,{},{},error,false};
            if(const auto error=original.file->copy_template_metadata(fd.value,false,(flags_value&0xffffU)|0x20U))return {parent.key,{},{},error,false};
        }else if(const auto error=set_descriptor_attributes(fd.value,info,(flags_value&0xffffU)|0x20U))return {parent.key,{},{},error,false};
        if(const auto error=initialize_file_security(fd.value,security))return {parent.key,{},{},error,false};
        if(::fstat(fd.value,&info)<0)return {parent.key,{},{},native_error(),false};
    }
    if(existed){
        if(const auto error=authorize(fd.value,access&~0x10000U))return {parent.key,{},{},error,true};
        if(access&0x10000U)if(const auto error=authorize_delete(directory.value,leaf))return {parent.key,{},{},error,true};
    }else if(security_context_){
        constexpr security::AccessMapping mapping{0x120089U,0x120116U,0x1200a0U,0x1f01ffU};
        const auto requested=security::mapped_access(access,mapping);
        const auto sd=security::parse_relative_descriptor(security);
        if((security::file_integrity_access(*security_context_,sd,requested)&requested)!=requested)return {parent.key,{},{},5,false};
        // 新建立物件依父層 CREATE 權限授予首次 handle；新 DACL 僅限制後續開啟。
    }
    if(!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))return {parent.key,{},{},5,existed};
    if(S_ISDIR(info.st_mode) && (!(flags_value&0x02000000U) || writable || disposition!=3))return {parent.key,{},{},5,existed};
    if(deleting && S_ISDIR(info.st_mode))return {parent.key,{},{},50,existed};
    const auto details=metadata(info,fd.value);
    if(existed && writable && (details.attributes&1U))return {parent.key,{},{},5,true};
    if(const auto error=sharing_error(details,access,sharing))return {parent.key,{},{},error,existed};
    if(deleting && (details.attributes&1U))return {parent.key,{},{},5,existed};
    auto state=file_state(details,true);
    std::erase_if(state->opens,[](const auto& weak){return weak.expired();});
    std::erase_if(state->targets,[](const auto& target){return !target->pending && target.use_count()==1;});
    auto file=std::make_shared<HostFile>(fd.value);fd.value=-1;file->state_=state;file->access_=access;file->sharing_=sharing;file->volume_=parent.volume;file->network_lease_=parent.network_lease;
    std::shared_ptr<HostDeleteTarget> deletion;
    if(deleting) {
        deletion=std::make_shared<HostDeleteTarget>();deletion->leaf=leaf;
        deletion->directory=::fcntl(directory.value,F_DUPFD_CLOEXEC,0);
        struct stat parent_info{};
        if(deletion->directory<0 || ::fstat(directory.value,&parent_info)<0)return {parent.key,{},{},native_error(),existed};
        deletion->device=details.device;deletion->inode=details.id;
        deletion->parent_device=static_cast<std::uint64_t>(parent_info.st_dev);deletion->parent_inode=static_cast<std::uint64_t>(parent_info.st_ino);
        state->targets.push_back(deletion);
    }
    state->opens.push_back(file);
    if(disposition==2 || disposition==5) {if(const auto error=file->truncate(0))return {parent.key,{},{},error,existed};}
    file->delete_target_=std::move(deletion);file->delete_on_close_=deleting;
    if(const auto error=attach_shared_file(*file))return {parent.key,{},{},error,existed};
    incomplete.cleanup=false;
    return {parent.key,file,file->information().metadata,0,existed};
#endif
} catch(const SecurityDescriptorError& error){return {{},{},{},error.code,false};}catch(const std::bad_alloc&) {return {{},{},{},8,false};}
std::uint32_t ReadOnlyFiles::create_directory(std::string path,const std::function<std::vector<std::uint8_t>()>& creation,std::string template_path) try {
    security::NativeFileIdentityScope identity_scope;
    // 唯讀掛載的上層目錄只是合成的列舉項目；在其中建立目錄時先建立實體目錄，不能回報已存在。
    const auto mount_ancestor=[&]{
        auto resolved=resolve(path,false,nullptr,true);
#if !defined(_WIN32)
        if(resolved.descriptor>=0)::close(resolved.descriptor);
#endif
        return (resolved.error==2 || resolved.error==3 || resolved.error==5) && std::any_of(readonly_mounts_.begin(),readonly_mounts_.end(),[&](const auto& entry){return entry.first.starts_with(resolved.key+(resolved.key.ends_with('/')?"":"/"));});
    };
    if(writable_ && !information(path).error && !mount_ancestor())return 183;
    if(!writable_)return 5;std::string leaf;auto parent=resolve(path,true,&leaf);if(parent.error)return parent.error;
#if defined(_WIN32)
    (void)creation;(void)template_path;return 50;
#else
    FileDescriptor fd{parent.descriptor};if(const auto error=authorize(fd.value,4U))return error;
    OpenHostFile original;
    if(!template_path.empty()) {
        const auto followed=follow_reparse(template_path);if(followed.second && followed.second!=50)return followed.second;
        original=pin(followed.second?std::move(template_path):followed.first);if(original.error)return original.error;
        if(!(original.metadata.attributes&0x10U))return 267;
        if(const auto error=authorize(original.file->descriptor_,0x88U))return error;
    }
    const auto security=creation_security(fd.value,true,creation?creation():std::vector<std::uint8_t>{});
    if(::mkdirat(fd.value,leaf.c_str(),0777)<0)return native_error();
    // 套用繼承 ACL 後可能沒有內容／搜尋權；共用 pin 保留 inode，按 WRITE_DAC 初始化。
    struct stat expected{};if(::fstatat(fd.value,leaf.c_str(),&expected,AT_SYMLINK_NOFOLLOW)<0)return native_error();
    FileDescriptor target{original.file?::openat(fd.value,leaf.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW):-1};
    const auto template_error=!original.file?0U:target.value<0?native_error():original.file->copy_template_metadata(target.value,true,0);
    const auto created=template_error?OpenHostFile{}:pin_security(path);
    const auto error=template_error?template_error:created.error?created.error:initialize_file_security(*created.file,security);
    if(error){
        struct stat current{};if(!::fstatat(fd.value,leaf.c_str(),&current,AT_SYMLINK_NOFOLLOW) && expected.st_dev==current.st_dev && expected.st_ino==current.st_ino)(void)::unlinkat(fd.value,leaf.c_str(),AT_REMOVEDIR);return error;
    }return 0;
#endif
}catch(const SecurityDescriptorError& error){return error.code;}catch(const std::bad_alloc&){return 8;}
std::uint32_t ReadOnlyFiles::set_attributes(std::string path,std::uint32_t attributes) {
    security::NativeFileIdentityScope identity_scope;
    if(!writable_)return 5;
    const auto point=reparse(path);if(!point.error){std::string leaf;const auto parent=resolve(path,true,&leaf);
#if !defined(_WIN32)
        FileDescriptor fd{parent.descriptor};if(parent.error)return parent.error;if(parent.readonly_file)return 5;
        if(point.kind!=3)return file_reparse::set_attributes(fd.value,leaf,attributes);
        if(attributes&~0x31b7U)return 50;FileDescriptor own{::openat(fd.value,leaf.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};if(own.value<0)return native_error();struct stat info{};
        if(::fstat(own.value,&info)<0)return native_error();if(static_cast<std::uint64_t>(info.st_dev)!=point.device || info.st_ino!=point.id)return 32;return set_descriptor_attributes(own.value,info,attributes);
#endif
    }
    // DIRECTORY 是由實際檔案類型決定；NORMAL 與其他 bit 併用時忽略。
    if(attributes&~0x31b7U)return 50;
    auto resolved=resolve(std::move(path));if(resolved.error)return resolved.error;
#if defined(_WIN32)
    return 50;
#else
    FileDescriptor file{resolved.descriptor};struct stat info{};
    if(const auto error=authorize(file.value,0x100U))return error;
    if(resolved.readonly_file)return 5;
    if(::fstat(file.value,&info)<0)return native_error();
    if(!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))return 5;
    return set_descriptor_attributes(file.value,info,attributes);
#endif
}
std::uint32_t ReadOnlyFiles::remove(std::string path,bool directory) try {
    security::NativeFileIdentityScope identity_scope;
    if(!writable_)return 5;std::string leaf;auto parent=resolve(path,true,&leaf);
#if defined(_WIN32)
    if(parent.error)return parent.error;
    (void)directory;return 50;
#else
    FileDescriptor fd{parent.descriptor};struct stat identity{};
    const auto protected_source=[&]{
        if(readonly_files_.empty() && readonly_mounts_.empty())return false;
        auto file=resolve(path);FileDescriptor source{file.descriptor};return file.readonly_file;
    };
    if(parent.error)return protected_source()?5U:parent.error;if(leaf==".")return 5;
    if(::fstatat(fd.value,leaf.c_str(),&identity,AT_SYMLINK_NOFOLLOW)<0){const auto error=native_error();return protected_source()?5U:error;}
    if(S_ISLNK(identity.st_mode)){
        if(const auto error=authorize(fd.value,0x40U))return error;
        const auto link=file_reparse::read(fd.value,leaf);if(link.error)return link.error;
        if(static_cast<std::uint64_t>(identity.st_dev)!=link.device || identity.st_ino!=link.id)return 32;
        if(!directory && (link.attributes&1U))return 5;
        if(directory!=(link.kind==2))return directory?267U:5U;
        return ::unlinkat(fd.value,leaf.c_str(),0)<0?native_error():0;
    }
    if(S_ISDIR(identity.st_mode)){
        FileDescriptor own{::openat(fd.value,leaf.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};
        if(own.value>=0){const auto point=volumes_->reparse(own.value);if(!point.error){
            if(!directory)return 5;if(parent.readonly_file)return 5;if(const auto code=sharing_error(metadata(identity),0x10000U,7))return code;
            return volumes_->delete_folder(point.mount.source,point.mount.path,true,[&]{struct stat current{};if(::fstatat(fd.value,leaf.c_str(),&current,AT_SYMLINK_NOFOLLOW)<0)return native_error();if(current.st_dev!=identity.st_dev || current.st_ino!=identity.st_ino)return 32U;return ::unlinkat(fd.value,leaf.c_str(),AT_REMOVEDIR)<0?native_error():0U;});
        }else if(point.error!=4390 && point.error!=50)return point.error;}
    }
    if(protected_source())return 5;
    if(!directory) {const auto details=information(path);if(!details.error && (details.metadata.attributes&1))return 5;}
    if(!S_ISREG(identity.st_mode) && !S_ISDIR(identity.st_mode))return 5;
    if(const auto error=authorize_delete(fd.value,leaf))return error;
    const auto info=metadata(identity);
    if(const auto error=sharing_error(info,0x10000U,7))return error;
    if(const auto state=file_state(info)) {
        if(directory)return 50;
        if(::faccessat(fd.value,".",W_OK|X_OK,0)<0)return native_error();
        auto deletion=std::make_shared<HostDeleteTarget>();deletion->leaf=leaf;
        deletion->directory=fd.value;fd.value=-1;deletion->device=info.device;deletion->inode=info.id;
        struct stat parent_info{};if(::fstat(deletion->directory,&parent_info)<0)return native_error();
        deletion->parent_device=static_cast<std::uint64_t>(parent_info.st_dev);deletion->parent_inode=static_cast<std::uint64_t>(parent_info.st_ino);
        if(state->shared){auto [index,error]=state->shared->target(deletion->directory,deletion->leaf,info,false);if(error)return error;deletion->shared_index=index;if(const auto code=state->shared->disposition(index,true))return code;}
        state->targets.push_back(deletion);deletion->pending=true;state->pending=true;return 0;
    }
    return ::unlinkat(fd.value,leaf.c_str(),directory?AT_REMOVEDIR:0)<0?native_error():0;
#endif
} catch(const std::bad_alloc&) {return 8;}
std::uint32_t ReadOnlyFiles::symbolic_link(std::string link,std::string target,bool directory) try {
    if(!writable_)return 5;
    if(link.empty() || target.empty() || target.find('\0')!=std::string::npos)return 87;
#if defined(__APPLE__)
    return symbolic_link_reparse(std::move(target),std::move(link),directory);
#else
#if defined(_WIN32)
    (void)directory;return 50;
#else
    std::string leaf;auto parent=resolve(link,true,&leaf);FileDescriptor fd{parent.descriptor};
    if(parent.error)return parent.error;if(parent.readonly_file)return 5;
    if(const auto error=authorize(fd.value,2U))return error;
    std::replace(target.begin(),target.end(),'\\','/');
    if(target.size()>1 && target[1]==':' && (target.size()<3 || target[2]!='/')){
        const char drive=target[0]>='a' && target[0]<='z'?static_cast<char>(target[0]-'a'+'A'):target[0];
        const auto current_drive=current_directory_.empty()?'\0':current_directory_[0]>='a' && current_directory_[0]<='z'?static_cast<char>(current_directory_[0]-'a'+'A'):current_directory_[0];
        target=(current_directory_.size()>1 && current_drive==drive?current_directory_:std::string(1,drive)+":")+"/"+target.substr(2);
    }
    const auto stored=std::string(directory?"x64arm-link-v1:1:":"x64arm-link-v1:0:")+target;
    return ::symlinkat(stored.c_str(),fd.value,leaf.c_str())<0?native_error():0;
#endif
#endif
}catch(const std::bad_alloc&){return 8;}
std::uint32_t ReadOnlyFiles::hard_link(std::string existing,std::string target) try {
    if(!writable_)return 5;
#if defined(_WIN32)
    (void)existing;(void)target;return 50;
#else
    const auto point=reparse(existing);struct stat original{};std::string source_leaf;Resolved from;
    FileDescriptor file{-1},source_directory{-1};
    if(!point.error){
        // CreateHardLinkW 的 API 契約以符號連結本身建立硬連結；保留 reparse inode／標記。
        if(point.kind!=1 || (point.attributes&1U))return 5;
        from=resolve(existing,true,&source_leaf);source_directory.value=from.descriptor;if(from.error)return from.error;
        if(::fstatat(source_directory.value,source_leaf.c_str(),&original,AT_SYMLINK_NOFOLLOW)<0)return native_error();
        if(static_cast<std::uint64_t>(original.st_dev)!=point.device || original.st_ino!=point.id)return 32;
    }else{
        auto source=resolve(existing);file.value=source.descriptor;if(source.error)return source.error;
        if(source.readonly_file || readonly_descriptor(file.value))return 5;
        if(::fstat(file.value,&original)<0)return native_error();if(!S_ISREG(original.st_mode))return 5;
        from=resolve(source.key.starts_with("relative:/")?source.key.substr(10):source.key,true,&source_leaf);source_directory.value=from.descriptor;if(from.error)return from.error;
    }
    if(original.st_nlink>=1024)return 1142;
    if(const auto error=sharing_error(metadata(original),0x10000U,7))return error;
    std::string target_leaf;
    auto to=resolve(target,true,&target_leaf);FileDescriptor target_directory{to.descriptor};if(to.error)return to.error;
    if(const auto error=point.error?authorize(file.value,0x80U):authorize_at(source_directory.value,source_leaf,0x80U))return error;
    if(const auto error=authorize(target_directory.value,2U))return error;
    if(from.readonly_file || to.readonly_file)return 5;
    // 映射磁碟仍各有獨立的客體 volume 身分；不能跨 drive 製造硬連結。
    if(from.volume && to.volume?from.volume!=to.volume:from.key.substr(0,2)!=to.key.substr(0,2))return 17;
    struct stat current{};if(::fstatat(source_directory.value,source_leaf.c_str(),&current,AT_SYMLINK_NOFOLLOW)<0)return native_error();
    if(current.st_dev!=original.st_dev || current.st_ino!=original.st_ino)return 32;
    if(::linkat(source_directory.value,source_leaf.c_str(),target_directory.value,target_leaf.c_str(),0)<0)return native_error();
    // 不跟隨競態替換的 symlink；只清除仍屬於本次建立的目錄項目。
    if(::fstatat(target_directory.value,target_leaf.c_str(),&current,AT_SYMLINK_NOFOLLOW)<0)return native_error();
    if(current.st_dev!=original.st_dev || current.st_ino!=original.st_ino){
        struct stat source_now{};if(::fstatat(source_directory.value,source_leaf.c_str(),&source_now,AT_SYMLINK_NOFOLLOW)==0 && source_now.st_dev==current.st_dev && source_now.st_ino==current.st_ino)(void)::unlinkat(target_directory.value,target_leaf.c_str(),0);
        return 32;
    }
    return 0;
#endif
} catch(const std::bad_alloc&){return 8;}
std::uint32_t ReadOnlyFiles::copy(std::string from,std::string to,bool fail_if_exists) try {
    security::NativeFileIdentityScope identity_scope;
    if(!writable_)return 5;
#if defined(_WIN32)
    (void)from;(void)to;(void)fail_if_exists;return 50;
#else
    const auto followed=follow_reparse(to);if(followed.second && followed.second!=50)return followed.second;if(!followed.second)to=followed.first;
    {auto target=resolve(to);FileDescriptor existing{target.descriptor};if(target.readonly_file)return 5;}
    auto source=resolve(std::move(from));FileDescriptor input{source.descriptor};
    if(source.error)return source.error;
    if(const auto error=authorize(input.value,0x80000000U))return error;
    struct stat original{};if(::fstat(input.value,&original)<0)return native_error();
    if(!S_ISREG(original.st_mode) || original.st_size<0)return 5;
    const auto source_metadata=metadata(original,input.value);
    if(const auto error=sharing_error(source_metadata,0x80000000U,7))return error;
    std::string leaf;auto target=resolve(std::move(to),true,&leaf);FileDescriptor directory{target.descriptor};
    if(target.error)return target.error;
    struct stat target_before{};const bool target_exists=::fstatat(directory.value,leaf.c_str(),&target_before,AT_SYMLINK_NOFOLLOW)==0;
    if(target_exists){if(const auto error=authorize_at(directory.value,leaf,0x40000000U))return error;}
    else if(const auto error=authorize(directory.value,2U))return error;
    // 先以 EXCL 判斷是否新建；既有檔案直到身份、保護與屬性檢查完才截短。
    FileDescriptor output{::openat(directory.value,leaf.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0666)};
    const bool created=output.value>=0;
    if(output.value<0) {
        if(errno!=EEXIST)return native_error();
        if(fail_if_exists)return 80;
        output.value=::openat(directory.value,leaf.c_str(),O_WRONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
        if(output.value<0)return native_error();
    }
    struct stat destination{};if(::fstat(output.value,&destination)<0)return native_error();
    struct PartialCopy {
        int directory;const std::string& leaf;const struct stat& identity;bool remove;
        ~PartialCopy() {
            // 僅清除本次新建且仍是同一 inode 的未完成輸出，不誤刪外部替換的名稱。
            struct stat current{};
            if(remove && ::fstatat(directory,leaf.c_str(),&current,AT_SYMLINK_NOFOLLOW)==0 &&
               current.st_dev==identity.st_dev && current.st_ino==identity.st_ino)(void)::unlinkat(directory,leaf.c_str(),0);
        }
    } partial{directory.value,leaf,destination,created};
    if(created){
        // 共用新檔案的完整 DACL／owner／完整性初始化；原生繼承無法還原虛擬 SID。
        const auto security=creation_security(directory.value,false,{});
        if(const auto error=initialize_file_security(output.value,security))return error;
        if(security_context_){
            const auto sd=security::parse_relative_descriptor(security);
            constexpr std::uint32_t desired=0x120116U;
            if((security::file_integrity_access(*security_context_,sd,desired)&desired)!=desired)return 5;
        }
        // 父目錄授予 CREATE 所取得的首次 handle 可完成複製；新 DACL 限制後續開啟。
    }else if(const auto error=authorize(output.value,0x40000000U))return error;
    if(!S_ISREG(destination.st_mode) || readonly_descriptor(output.value))return 5;
    if(const auto error=sharing_error(metadata(destination,output.value),0x40000000U,3))return error;
    if(original.st_dev==destination.st_dev && original.st_ino==destination.st_ino)return 32;
    if(!created && (metadata(destination,output.value).attributes&3U))return 5;
    if(::ftruncate(output.value,0)<0)return native_error();
    // 固定大小緩衝區；複製開始時的檔案長度，避免同步來源持續成長而無限複製。
    std::array<std::uint8_t,65536> buffer{};off_t position=0;
    while(position<original.st_size) {
        const auto amount=static_cast<std::size_t>(std::min<off_t>(original.st_size-position,static_cast<off_t>(buffer.size())));
        ssize_t count;do {count=::pread(input.value,buffer.data(),amount,position);}while(count<0 && errno==EINTR);
        process_io::record(process_io::Kind::Read,count<0?0:static_cast<std::uint64_t>(count));
        if(count<=0)return count<0?native_error():1117;
        ssize_t written=0;
        while(written<count) {
            ssize_t bytes;do {bytes=::pwrite(output.value,buffer.data()+written,static_cast<std::size_t>(count-written),position+written);}while(bytes<0 && errno==EINTR);
            process_io::record(process_io::Kind::Write,bytes<0?0:static_cast<std::uint64_t>(bytes));
            if(bytes<=0)return bytes<0?native_error():112;
            written+=bytes;
        }
        position+=count;
    }
#if defined(__APPLE__)
    const timespec times[]{original.st_atimespec,original.st_mtimespec};
    attrlist attributes{};attributes.bitmapcount=ATTR_BIT_MAP_COUNT;attributes.commonattr=ATTR_CMN_CRTIME;
    if(::fsetattrlist(output.value,&attributes,&original.st_birthtimespec,sizeof(timespec),0)<0)return native_error();
#else
    const timespec times[]{original.st_atim,original.st_mtim};
#endif
    if(::futimens(output.value,times)<0)return native_error();
    if(const auto error=set_descriptor_attributes(output.value,destination,source_metadata.attributes))return error;
    // 覆寫不是交易：晚期 I/O 錯誤可能留下既有目的檔案的部分內容，但不得回報成功。
    // close 的延遲寫入錯誤也必須保留；不強制 fsync，以維持一般 CopyFile 的快取語義。
    const auto closed=::close(output.value);output.value=-1;if(closed<0)return native_error();
    partial.remove=false;return 0;
#endif
}catch(const SecurityDescriptorError& error){return error.code;}catch(const std::bad_alloc&){return 8;}
std::uint32_t ReadOnlyFiles::replace(std::string original,std::string replacement,std::string backup,std::uint32_t flags) try {
    security::NativeFileIdentityScope identity_scope;
    if(!writable_)return 5;if(flags&~6U)return 87;
#if defined(_WIN32)
    (void)original;(void)replacement;(void)backup;return 50;
#else
    const auto old=resolve(original);FileDescriptor old_fd{old.descriptor};if(old.error)return old.error;
    const auto fresh=resolve(replacement);FileDescriptor fresh_fd{fresh.descriptor};if(fresh.error)return fresh.error;
    if(const auto error=authorize(old_fd.value,0x80000000U))return error;
    if(const auto error=authorize(fresh_fd.value,0xc0000000U))return error;
    if(old.readonly_file || fresh.readonly_file)return 5;
    struct stat old_stat{},new_stat{};if(::fstat(old_fd.value,&old_stat) || ::fstat(fresh_fd.value,&new_stat))return native_error();
    if(!S_ISREG(old_stat.st_mode)||!S_ISREG(new_stat.st_mode))return 5;
    if(old_stat.st_dev!=new_stat.st_dev)return 17;if(old_stat.st_ino==new_stat.st_ino)return 87;
    const auto old_info=metadata(old_stat,old_fd.value),new_info=metadata(new_stat,fresh_fd.value);
    if((old_info.attributes|new_info.attributes)&1U)return 5;
    if(const auto e=sharing_error(old_info,0x10000U|0x80000000U,7))return e;
    if(const auto e=sharing_error(new_info,0x10000U|0x80000000U|0x40000000U,7))return e;
    std::string old_leaf,backup_leaf;
    const auto parent=resolve(original,true,&old_leaf);FileDescriptor old_parent{parent.descriptor};if(parent.error)return parent.error;
    if(const auto error=authorize_delete(old_parent.value,old_leaf))return error;
    auto backup_parent=backup.empty()?Resolved{}:resolve(backup,true,&backup_leaf);FileDescriptor backup_fd{backup_parent.descriptor};
    if(!backup.empty()){
        if(backup_parent.error)return backup_parent.error;struct stat directory{},existing{};
        if(const auto error=authorize(backup_fd.value,2U))return error;
        if(::fstat(backup_fd.value,&directory))return native_error();if(directory.st_dev!=old_stat.st_dev)return 17;
        if(::fstatat(backup_fd.value,backup_leaf.c_str(),&existing,AT_SYMLINK_NOFOLLOW)==0){
            if(const auto error=authorize_delete(backup_fd.value,backup_leaf))return error;
            if(!S_ISREG(existing.st_mode)||!(existing.st_mode&S_IWUSR))return 5;
            if(existing.st_ino==old_stat.st_ino || existing.st_ino==new_stat.st_ino)return 87;
            const auto target=resolve(backup);FileDescriptor checked{target.descriptor};if(target.error)return target.error;if(target.readonly_file)return 5;
            if(const auto e=sharing_error(metadata(existing),0x10000U,7))return e;
        }else if(errno!=ENOENT)return native_error();
    }
    // 僅合併 Windows 檔案模型能表達的屬性與建立時間；資料與修改時間來自 replacement。
#if defined(__APPLE__)
    // ReplaceFile 保留原檔案的完整 DACL。fcopyfile 只複製來源的 explicit ACE，
    // 再接上目的檔的 inherited ACE，會改變拒絕順序並破壞客體 SID 投影。
    const auto merge_dacl=[&]()->std::uint32_t {
        if(const auto error=authorize(fresh_fd.value,0x40000U))return error;
        try {
            const auto source_copy=::fcntl(old_fd.value,F_DUPFD_CLOEXEC,0);if(source_copy<0)return native_error();
            HostFile source_security(source_copy);
            const auto target_copy=::fcntl(fresh_fd.value,F_DUPFD_CLOEXEC,0);if(target_copy<0)return native_error();
            HostFile target_security(target_copy);
            const auto descriptor=security::file_descriptor(security::read_native_file_security(source_security,true),4);
            security::write_native_file_security(target_security,descriptor,4,
                security_context_?security_context_->primary_user:security::Bytes{},
                security_context_?security_context_->primary_group:security::Bytes{});
            return 0;
        }catch(const SecurityDescriptorError& error){return error.code;}
    };
    if(const auto error=merge_dacl();error && !(flags&6U))return error;
#else
    if(!(flags&6U))return 50; // 此平台尚未提供 ACL／建立時間合併。
#endif
    if(const auto e=set_descriptor_attributes(fresh_fd.value,new_stat,old_info.attributes);e && !(flags&2U))return e;
#if defined(__APPLE__)
    const auto duplicate=::fcntl(fresh_fd.value,F_DUPFD_CLOEXEC,0);if(duplicate<0)return native_error();
    {HostFile file(duplicate);if(const auto e=file.set_times(old_info.created,0,0);e && !(flags&2U))return e;}
#endif
    if(!backup.empty()){
        // 先保留原 inode，再原子替換備份名稱；主目標始終存在。
        static std::atomic<std::uint64_t> sequence{};std::string temporary;
        bool linked=false;
        for(unsigned attempt=0;attempt<64;++attempt){
            temporary=".x64arm-replace-"+std::to_string(::getpid())+"-"+std::to_string(++sequence);
            if(::linkat(old_parent.value,old_leaf.c_str(),backup_fd.value,temporary.c_str(),0)==0){linked=true;break;}if(errno!=EEXIST)return native_error();
        }
        if(!linked)return 183;
        if(::renameat(backup_fd.value,temporary.c_str(),backup_fd.value,backup_leaf.c_str())){const auto error=native_error();::unlinkat(backup_fd.value,temporary.c_str(),0);return error;}
    }
    const auto moved=rename(replacement,original,true);
    return moved?1176U:0U;
#endif
} catch(const std::bad_alloc&){return 8;}

std::uint32_t ReadOnlyFiles::rename(std::string from,std::string to,bool replace,bool copy_allowed,bool write_through,const HostFile* owner) try {
    security::NativeFileIdentityScope identity_scope;
    std::optional<ReadOnlyFiles> image_paths;std::string image_key;
#if !defined(_WIN32)
    if(!readonly_files_.empty() || !readonly_mounts_.empty()) {
        auto original=reparse(from).error?resolve(from):Resolved{};FileDescriptor first{original.descriptor};
        if(original.readonly_file) {
            const auto image=readonly_files_.find(original.key);
            if(image==readonly_files_.end() || !writable_image_source(from,image->second))return 5;
            image_key=original.key;image_paths=*this;image_paths->readonly_files_.erase(image_key);
        }
        auto destination=reparse(to).error?resolve(to):Resolved{};FileDescriptor second{destination.descriptor};if(destination.readonly_file)return 5;
    }
#endif
    auto& paths=image_paths?*image_paths:*this;
    if(!writable_)return 5;std::string from_name,to_name;auto source=paths.resolve(std::move(from),true,&from_name);if(source.error)return source.error;
#if defined(_WIN32)
    (void)to;(void)replace;(void)copy_allowed;(void)write_through;(void)owner;return 50;
#else
    FileDescriptor first{source.descriptor};auto target=paths.resolve(std::move(to),true,&to_name);if(target.error)return target.error;FileDescriptor second{target.descriptor};if(from_name=="." || to_name==".")return 5;
    struct stat original_entry{},first_info{},second_info{},existing{};
    if(::fstatat(first.value,from_name.c_str(),&original_entry,AT_SYMLINK_NOFOLLOW)<0 || ::fstat(first.value,&first_info)<0 || ::fstat(second.value,&second_info)<0)return native_error();
    const auto original_metadata=metadata(original_entry);
    if(!owner)if(const auto error=authorize_delete(first.value,from_name))return error;
    if(const auto error=authorize(second.value,S_ISDIR(original_entry.st_mode)?4U:2U))return error;
    if(owner){const auto expected=owner->information();if(expected.error)return expected.error;if(expected.metadata.device!=original_metadata.device || expected.metadata.id!=original_metadata.id)return 2;}
    if(const auto error=sharing_error(original_metadata,0x10000U,7,owner))return error;
    if(::fstatat(second.value,to_name.c_str(),&existing,AT_SYMLINK_NOFOLLOW)==0) {
        if(const auto error=authorize_delete(second.value,to_name))return error;
        if(!S_ISREG(existing.st_mode) || !(existing.st_mode&S_IWUSR))return 5;
        if(const auto error=sharing_error(metadata(existing),0x10000U,7))return error;
    }
    struct Relocation {std::shared_ptr<HostDeleteTarget> original,replacement;};std::vector<Relocation> relocations;
    if(const auto state=file_state(original_metadata))for(const auto& entry:state->targets) {
        if(entry->leaf!=from_name || entry->parent_device!=static_cast<std::uint64_t>(first_info.st_dev) || entry->parent_inode!=static_cast<std::uint64_t>(first_info.st_ino))continue;
        auto replacement=std::make_shared<HostDeleteTarget>();replacement->leaf=to_name;replacement->directory=::fcntl(second.value,F_DUPFD_CLOEXEC,0);
        if(replacement->directory<0)return native_error();replacement->parent_device=static_cast<std::uint64_t>(second_info.st_dev);replacement->parent_inode=static_cast<std::uint64_t>(second_info.st_ino);
        relocations.push_back({entry,std::move(replacement)});
    }
    int moved=-1;std::uint32_t move_error=0;const bool different_volume=source.volume && target.volume && source.volume!=target.volume;
    const auto native_move=[&]()->std::uint32_t {
    if(!replace) {
#if defined(__APPLE__)
        moved=::renameatx_np(first.value,from_name.c_str(),second.value,to_name.c_str(),RENAME_EXCL);
#else
        moved=::renameat2(first.value,from_name.c_str(),second.value,to_name.c_str(),RENAME_NOREPLACE);
#endif
    } else moved=::renameat(first.value,from_name.c_str(),second.value,to_name.c_str());
    return moved==0?0U:native_error();};
    if(different_volume){move_error=17;if(!copy_allowed)return move_error;}
    else if(source.volume && target.volume)move_error=volumes_->relocate_folders(source.volume,source.volume_path,target.volume,target.volume_path,true,native_move,[&]{
#if defined(__APPLE__)
        return ::renameatx_np(second.value,to_name.c_str(),first.value,from_name.c_str(),RENAME_EXCL)<0?native_error():0U;
#elif defined(__linux__)
        return ::renameat2(second.value,to_name.c_str(),first.value,from_name.c_str(),RENAME_NOREPLACE)<0?native_error():0U;
#else
        return ::renameat(second.value,to_name.c_str(),first.value,from_name.c_str())<0?native_error():0U;
#endif
    });else move_error=native_move();
    if(move_error && move_error!=17)return move_error;
    if(moved==0) {
        if(!image_key.empty()) {
            // 映像的唯讀保護跟隨已改名的檔案，原名稱可接收更新檔案。
            auto image=readonly_files_.extract(image_key);image.key()=target.key;image.mapped()=target.host_path;
            readonly_files_.insert(std::move(image));
        }
        for(auto& relocation:relocations) {
            auto& previous=*relocation.original;auto& next=*relocation.replacement;
            std::swap(previous.directory,next.directory);previous.leaf.swap(next.leaf);
            previous.parent_device=next.parent_device;previous.parent_inode=next.parent_inode;
        }
        return 0;
    }
    if(move_error!=17 || !copy_allowed)return move_error;
    FileDescriptor input{::openat(first.value,from_name.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK)};
    if(input.value<0)return native_error();
    if(const auto error=authorize(input.value,0x80000000U))return error;
    struct stat original{};if(::fstat(input.value,&original)<0)return native_error();
    if(!S_ISREG(original.st_mode))return 17;
    FileDescriptor output{::openat(second.value,to_name.c_str(),O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK|(replace?0:O_EXCL),0666)};
    if(output.value<0)return native_error();
    if(const auto error=authorize(output.value,0x40000000U))return error;
    struct stat destination{};if(::fstat(output.value,&destination)<0)return native_error();
    if(!S_ISREG(destination.st_mode) || (original.st_dev==destination.st_dev && original.st_ino==destination.st_ino))return 5;
    if(::ftruncate(output.value,0)<0)return native_error();
    std::array<std::uint8_t,65536> buffer{};
    for(;;) {
        ssize_t count;do {count=::read(input.value,buffer.data(),buffer.size());}while(count<0 && errno==EINTR);
        process_io::record(process_io::Kind::Read,count<0?0:static_cast<std::uint64_t>(count));
        if(count<0)return native_error();if(!count)break;
        ssize_t position=0;
        while(position<count) {ssize_t bytes;do {bytes=::write(output.value,buffer.data()+position,static_cast<std::size_t>(count-position));}while(bytes<0 && errno==EINTR);process_io::record(process_io::Kind::Write,bytes<0?0:static_cast<std::uint64_t>(bytes));if(bytes<=0)return bytes<0?native_error():112;position+=bytes;}
    }
#if defined(__APPLE__)
    const timespec times[]{original.st_atimespec,original.st_mtimespec};
#else
    const timespec times[]{original.st_atim,original.st_mtim};
#endif
    if(::futimens(output.value,times)<0 || ::fchmod(output.value,original.st_mode&0777)<0)return native_error();
    if(write_through && ::fsync(output.value)<0)return native_error();
    // Windows 跨磁碟移動在複製成功後，即使來源刪除失敗仍回報成功。
    (void)::unlinkat(first.value,from_name.c_str(),0);return 0;
#endif
} catch(const std::bad_alloc&) {return 8;}

#include "readonly_file_control.inc"
}
