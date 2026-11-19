// 核心既有目錄的宿主表示；只保存虛擬物件身分與安全描述元，不映射宿主路徑。
// 動態目錄、符號連結與私有命名空間以 session 目錄中的固定大小紀錄跨程序共享：暫時項目以參考數及
// 既有具名物件的存活判定（持有者提示＋程序掃描）回收，永久與內建項目保留到 session 結束。
#include "shared_object_lifetime.hpp"
#include "x64arm/windows_paths.hpp"
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#if !defined(_WIN32)
#include <dirent.h>
#endif
namespace x64arm {
namespace {
constexpr std::uint64_t directory_signature=0x315249444f343658ULL;
struct DirectoryRecord {std::uint64_t signature{directory_signature};std::uint32_t index{},reserved{};};
std::uint32_t directory_io_error(){return errno==EACCES || errno==EPERM?5U:errno==ENOMEM || errno==ENOSPC?8U:6U;}
constexpr std::uint64_t namespace_signature=0x53504e4f52413458ULL;
constexpr std::size_t namespace_record_size=16384;
constexpr off_t namespace_target_offset=4096;
struct NamespaceRecord {
    std::uint64_t signature{namespace_signature},references{};
    std::uint32_t kind{},flags{},path_chars{},target_chars{};
    std::uint64_t created{},serial{};
    std::array<char16_t,object_namespace_path_limit> path{};
};
static_assert(sizeof(NamespaceRecord)<=static_cast<std::size_t>(detail::named_object_page_hint_offset));
static_assert(offsetof(NamespaceRecord,flags)==20 && offsetof(NamespaceRecord,path_chars)==24);
std::uint32_t namespace_status(){return errno==EACCES || errno==EPERM?0xc0000022U:errno==ENOMEM || errno==ENOSPC?0xc000009aU:0xc0000001U;}
std::string namespace_file_name(std::u16string_view key){
    std::uint64_t a=14695981039346656037ULL,b=0x84222325cbf29ce4ULL;
    for(auto c:key){a=(a^c)*1099511628211ULL;b=(b^(c+1U))*0x100000001b3ULL;}
    std::ostringstream out;out<<"object-ns-"<<std::hex<<std::setw(16)<<std::setfill('0')<<a<<std::setw(16)<<b;return out.str();
}
#if !defined(_WIN32)
bool read_namespace(int file,NamespaceRecord& record,std::u16string* target){
    struct stat info{};
    if(::fstat(file,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink>1 ||
       info.st_size!=static_cast<off_t>(namespace_record_size) || ::pread(file,&record,sizeof(record),0)!=static_cast<ssize_t>(sizeof(record)) ||
       record.signature!=namespace_signature || !record.path_chars || record.path_chars>object_namespace_path_limit ||
       record.target_chars>object_namespace_target_limit || (record.kind!=1 && record.kind!=2))return false;
    if(target){
        target->assign(record.target_chars,u'\0');
        if(record.target_chars && ::pread(file,target->data(),record.target_chars*2U,namespace_target_offset)!=static_cast<ssize_t>(record.target_chars*2U))return false;
    }
    return true;
}
ObjectNamespaceEntry namespace_entry(const NamespaceRecord& record,std::u16string target){
    ObjectNamespaceEntry entry;entry.path.assign(record.path.data(),record.path_chars);entry.target=std::move(target);
    entry.kind=static_cast<ObjectNamespaceKind>(record.kind);entry.flags=record.flags;entry.created=record.created;entry.serial=record.serial;return entry;
}
std::uint64_t namespace_now(){
    const auto now=std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()/100)+116444736000000000ULL;
}
#endif
}
std::u16string ObjectDirectory::path_key(std::u16string_view path){std::u16string key(path);for(auto& c:key)c=windows_path_upper(c);return key;}
ObjectDirectory::~ObjectDirectory(){
#if !defined(_WIN32)
    if(counted_ && file_>=0 && directory_>=0){
        try{
            SharedEventLock lock(directory_);NamespaceRecord record;
            if(::pread(file_,&record,sizeof(record),0)==static_cast<ssize_t>(sizeof(record)) && record.signature==namespace_signature && record.references){
                --record.references;(void)::pwrite(file_,&record.references,sizeof(record.references),offsetof(NamespaceRecord,references));
                struct stat own{},linked{};
                if(!record.references && !(record.flags&(object_namespace_permanent|object_namespace_builtin)) && !::fstat(file_,&own) &&
                   !::fstatat(directory_,file_name_.c_str(),&linked,AT_SYMLINK_NOFOLLOW) && own.st_dev==linked.st_dev && own.st_ino==linked.st_ino)
                    (void)::unlinkat(directory_,file_name_.c_str(),0);
            }
        }catch(...){}
    }
    if(file_>=0)::close(file_);if(directory_>=0)::close(directory_);
#endif
}
std::shared_ptr<ObjectDirectory> ObjectDirectory::open(const std::string& session,std::uint32_t index,const ObjectSecurityFactory& security){
#if !defined(_WIN32)
    if(index>=builtin_object_directories.size())throw SecurityDescriptorError{87};
    auto object=std::make_shared<ObjectDirectory>();object->index_=index;
    object->directory_=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat info{};if(object->directory_<0 || ::fstat(object->directory_,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))throw SecurityDescriptorError{5};
    SharedEventLock lock(object->directory_);const auto name="object-directory-"+std::to_string(index);
    object->file_=::openat(object->directory_,name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    const bool created=object->file_>=0;
    if(!created && errno==EEXIST)object->file_=::openat(object->directory_,name.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(object->file_<0)throw SecurityDescriptorError{directory_io_error()};
    if(created){
        try{DirectoryRecord record;record.index=index;
            if(::pwrite(object->file_,&record,sizeof(record),0)!=sizeof(record))throw SecurityDescriptorError{directory_io_error()};
            set_shared_object_security(object->file_,security());
        }catch(...){(void)::unlinkat(object->directory_,name.c_str(),0);throw;}
    }
    DirectoryRecord record;
    if(::fstat(object->file_,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink!=1 || info.st_size!=sizeof(record) ||
       ::pread(object->file_,&record,sizeof(record),0)!=sizeof(record) || record.signature!=directory_signature || record.index!=index)throw SecurityDescriptorError{6};
    return object;
#else
    (void)session;(void)index;(void)security;throw SecurityDescriptorError{50};
#endif
}
std::shared_ptr<ObjectDirectory> ObjectDirectory::inherit(const std::string& session,int descriptor){
#if !defined(_WIN32)
    auto object=std::make_shared<ObjectDirectory>();object->file_=descriptor;
    (void)::fcntl(descriptor,F_SETFD,FD_CLOEXEC);
    object->directory_=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat info{};DirectoryRecord record;
    if(object->directory_<0 || ::fstat(object->directory_,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))throw SecurityDescriptorError{5};
    SharedEventLock lock(object->directory_);
    if(::fstat(descriptor,&info))throw SecurityDescriptorError{6};
    if(info.st_size==static_cast<off_t>(namespace_record_size)){
        // 動態項目：紀錄鍵對應的檔名須仍指向同一 inode（私有命名空間刪除名稱後仍可繼承既有控制代碼）。
        NamespaceRecord entry;std::u16string target;if(!read_namespace(descriptor,entry,&target))throw SecurityDescriptorError{6};
        object->entry_=namespace_entry(entry,std::move(target));object->index_=dynamic_index;
        object->file_name_=namespace_file_name(entry_key(object->entry_));
        struct stat expected{};
        if(!(object->entry_.flags&object_namespace_private) && (::fstatat(object->directory_,object->file_name_.c_str(),&expected,AT_SYMLINK_NOFOLLOW) ||
           info.st_dev!=expected.st_dev || info.st_ino!=expected.st_ino))throw SecurityDescriptorError{6};
        ++entry.references;if(::pwrite(descriptor,&entry.references,sizeof(entry.references),offsetof(NamespaceRecord,references))!=sizeof(entry.references))throw SecurityDescriptorError{directory_io_error()};
        object->counted_=true;return object;
    }
    if(!S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink!=1 || info.st_size!=sizeof(record) ||
       ::pread(descriptor,&record,sizeof(record),0)!=sizeof(record) || record.signature!=directory_signature || record.index>=builtin_object_directories.size())throw SecurityDescriptorError{6};
    struct stat expected{};const auto name="object-directory-"+std::to_string(record.index);
    if(::fstatat(object->directory_,name.c_str(),&expected,AT_SYMLINK_NOFOLLOW) || info.st_dev!=expected.st_dev || info.st_ino!=expected.st_ino)throw SecurityDescriptorError{6};
    object->index_=record.index;return object;
#else
    (void)session;(void)descriptor;throw SecurityDescriptorError{50};
#endif
}
ObjectDirectory::Opened ObjectDirectory::open_entry(const std::string& session,std::u16string_view key,const ObjectNamespaceEntry* create,const ObjectSecurityFactory& security){
#if !defined(_WIN32)
    auto object=std::make_shared<ObjectDirectory>();object->index_=dynamic_index;
    object->directory_=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat info{};if(object->directory_<0 || ::fstat(object->directory_,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))return {{},0xc0000022U};
    if(create && (create->path.empty() || create->path.size()>object_namespace_path_limit))return {{},0xc0000106U};
    if(create && create->target.size()>object_namespace_target_limit)return {{},0xc0000106U};
    SharedEventLock lock(object->directory_);object->file_name_=namespace_file_name(key);
    int file=::openat(object->directory_,object->file_name_.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    // 被強制結束的程序留下的暫時項目：沒有任何程序持有時移除，與具名事件／mutex 相同判定。
    file=detail::reopen_live_named_object(session,object->directory_,object->file_name_,file);
    bool created=false;
    if(file<0){
        if(errno!=ENOENT)return {{},namespace_status()};
        if(!create)return {{},0xc0000034U};
        file=::openat(object->directory_,object->file_name_.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(file<0)return {{},namespace_status()};
        created=true;
        NamespaceRecord record;record.kind=static_cast<std::uint32_t>(create->kind);record.flags=create->flags;
        record.path_chars=static_cast<std::uint32_t>(create->path.size());record.target_chars=static_cast<std::uint32_t>(create->target.size());
        record.created=create->created?create->created:namespace_now();record.serial=create->serial?create->serial:(static_cast<std::uint64_t>(::arc4random())<<32|::arc4random());
        std::copy(create->path.begin(),create->path.end(),record.path.begin());
        const auto failed=[&](std::uint32_t status){(void)::unlinkat(object->directory_,object->file_name_.c_str(),0);::close(file);return Opened{{},status};};
        if(::ftruncate(file,static_cast<off_t>(namespace_record_size)) || ::pwrite(file,&record,sizeof(record),0)!=static_cast<ssize_t>(sizeof(record)) ||
           (!create->target.empty() && ::pwrite(file,create->target.data(),create->target.size()*2U,namespace_target_offset)!=static_cast<ssize_t>(create->target.size()*2U)))return failed(namespace_status());
        try{if(security)set_shared_object_security(file,security());}catch(const SecurityDescriptorError& error){return failed(error.code==5?0xc0000022U:error.code==8?0xc000009aU:0xc000000dU);}
        detail::named_object_record_holder(file,detail::named_object_page_hint_offset,::getpid(),file);
    }
    object->file_=file;NamespaceRecord record;std::u16string target;
    if(!read_namespace(file,record,&target))return {{},0xc0000024U};
    object->entry_=namespace_entry(record,std::move(target));
    const auto stored=entry_key(object->entry_);
    if(stored!=key)return {{},0xc0000024U};
    ++record.references;
    if(::pwrite(file,&record.references,sizeof(record.references),offsetof(NamespaceRecord,references))!=sizeof(record.references))return {{},namespace_status()};
    object->counted_=true;return {object,0,!created};
#else
    (void)session;(void)key;(void)create;(void)security;return {{},0xc00000bbU};
#endif
}
std::uint32_t ObjectDirectory::set_permanent(bool permanent){
#if !defined(_WIN32)
    if(!dynamic())return permanent?0U:0xc0000022U; // 核心既有目錄本來就是永久物件。
    SharedEventLock lock(directory_);NamespaceRecord record;
    if(::pread(file_,&record,sizeof(record),0)!=static_cast<ssize_t>(sizeof(record)) || record.signature!=namespace_signature)return 0xc0000024U;
    if(record.flags&object_namespace_builtin)return permanent?0U:0xc0000022U;
    record.flags=permanent?record.flags|object_namespace_permanent:record.flags&~object_namespace_permanent;
    if(::pwrite(file_,&record.flags,sizeof(record.flags),offsetof(NamespaceRecord,flags))!=sizeof(record.flags))return namespace_status();
    entry_.flags=record.flags;return 0;
#else
    (void)permanent;return 0xc00000bbU;
#endif
}
std::uint32_t ObjectDirectory::update_target(std::u16string_view target){
#if !defined(_WIN32)
    if(!dynamic() || target.size()>object_namespace_target_limit)return 0xc000000dU;
    if(target==entry_.target)return 0;
    SharedEventLock lock(directory_);const auto chars=static_cast<std::uint32_t>(target.size());
    if((chars && ::pwrite(file_,target.data(),chars*2U,namespace_target_offset)!=static_cast<ssize_t>(chars*2U)) ||
       ::pwrite(file_,&chars,sizeof(chars),offsetof(NamespaceRecord,target_chars))!=sizeof(chars))return namespace_status();
    entry_.target.assign(target);return 0;
#else
    (void)target;return 0xc00000bbU;
#endif
}
std::uint32_t ObjectDirectory::remove_name(){
#if !defined(_WIN32)
    if(!private_namespace())return 0xc0000024U;
    SharedEventLock lock(directory_);struct stat own{},linked{};
    if(!::fstat(file_,&own) && !::fstatat(directory_,file_name_.c_str(),&linked,AT_SYMLINK_NOFOLLOW) && own.st_dev==linked.st_dev && own.st_ino==linked.st_ino)
        (void)::unlinkat(directory_,file_name_.c_str(),0);
    return 0;
#else
    return 0xc00000bbU;
#endif
}
std::vector<ObjectNamespaceEntry> ObjectDirectory::list_entries(const std::string& session){
    std::vector<ObjectNamespaceEntry> result;
#if !defined(_WIN32)
    const int directory=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(directory<0)return result;
    struct stat info{};if(::fstat(directory,&info) || info.st_uid!=::geteuid() || (info.st_mode&077)){::close(directory);return result;}
    const int listing=::dup(directory);DIR* stream=listing>=0?::fdopendir(listing):nullptr;
    if(!stream){if(listing>=0)::close(listing);::close(directory);return result;}
    try{
        SharedEventLock lock(directory);
        while(const auto* item=::readdir(stream)){
            if(std::strncmp(item->d_name,"object-ns-",10))continue;
            const int file=::openat(directory,item->d_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(file<0)continue;
            NamespaceRecord record;std::u16string target;
            if(read_namespace(file,record,&target) && (record.references || (record.flags&(object_namespace_permanent|object_namespace_builtin))))
                result.push_back(namespace_entry(record,std::move(target)));
            ::close(file);
        }
    }catch(...){}
    ::closedir(stream);::close(directory);
#else
    (void)session;
#endif
    return result;
}
}
