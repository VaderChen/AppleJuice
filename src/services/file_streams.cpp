#include "x64arm/file_streams.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <utility>
#include <openssl/evp.h>
#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace x64arm::file_streams {
std::string folded(std::string_view name){auto value=utf8_to_utf16_lossy(std::string(name));for(auto& c:value)c=unicode_case_unit(c,true);return utf16_to_utf8(value);}
Path parse(std::string_view text){
    Path result{std::string(text),{},0,false};
    std::size_t begin=0;
    if(text.starts_with("\\\\?\\") || text.starts_with("\\\\.\\") || text.starts_with("//?/") || text.starts_with("//./"))begin=4;
    else if(text.starts_with("\\??\\"))begin=4;
    if(text.size()>begin+1 && ((text[begin]>='A' && text[begin]<='Z') || (text[begin]>='a' && text[begin]<='z')) && text[begin+1]==':')begin+=2;
    const auto colon=text.find(':',begin);if(colon==std::string_view::npos)return result;
    result.specified=true;result.base=std::string(text.substr(0,colon));
    if(result.base.empty() || result.base.back()=='/' || result.base.back()=='\\'){result.error=123;return result;}
    auto suffix=text.substr(colon+1);const auto type=suffix.find(':');
    if(type!=std::string_view::npos){if(folded(suffix.substr(type+1))!="$DATA"){result.error=123;return result;}suffix=suffix.substr(0,type);}
    if(suffix.find_first_of("\\/:*?\"<>|")!=std::string_view::npos || std::any_of(suffix.begin(),suffix.end(),[](unsigned char c){return c<32;})){result.error=123;return result;}
    const auto unicode=utf8_to_utf16_lossy(std::string(suffix));if(utf16_to_utf8(unicode)!=suffix){result.error=123;return result;}
    if(unicode.size()>255){result.error=206;return result;}
    result.name=std::string(suffix);return result;
}
Directory::Directory(Directory&& other) noexcept:descriptor(std::exchange(other.descriptor,-1)),error(other.error){}
Directory& Directory::operator=(Directory&& other) noexcept{if(this!=&other){Directory previous(std::move(*this));descriptor=std::exchange(other.descriptor,-1);error=other.error;}return *this;}
Directory::~Directory(){
#if !defined(_WIN32)
    if(descriptor>=0)::close(descriptor);
#endif
}
#if !defined(_WIN32)
namespace {
std::uint32_t error(int code=errno){switch(code){case ENOENT:return 2;case ENOTDIR:return 3;case EACCES:case EPERM:case ELOOP:return 5;case EBADF:return 6;case ENOMEM:return 8;case EEXIST:return 183;case ENOSPC:case EDQUOT:return 112;case EINVAL:return 87;default:return 1117;}}
struct Fd {int value{-1};~Fd(){if(value>=0)::close(value);}};
// 每次取得獨立 open description，讓同程序多執行緒也由 flock 互斥。
struct Lock {int fd;bool held;explicit Lock(int descriptor):fd(::openat(descriptor,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)),held(fd>=0 && ::flock(fd,LOCK_EX)==0){}~Lock(){if(fd>=0)::close(fd);}};
std::string key(std::string_view name){
    const auto normalized=folded(name);std::array<unsigned char,EVP_MAX_MD_SIZE> digest{};unsigned length=0;
    if(EVP_Digest(normalized.data(),normalized.size(),digest.data(),&length,EVP_sha256(),nullptr)!=1 || length!=32)return {};
    constexpr char hex[]="0123456789abcdef";std::string result;result.reserve(64);for(unsigned i=0;i<length;++i){result+=hex[digest[i]>>4];result+=hex[digest[i]&15];}return result;
}
#if defined(__APPLE__)
constexpr auto name_attribute="com.x64arm.stream-name";
ssize_t get_name(int fd,void* bytes,std::size_t size){return ::fgetxattr(fd,name_attribute,bytes,size,0,0);}
int set_name(int fd,std::string_view name){return ::fsetxattr(fd,name_attribute,name.data(),name.size(),0,0);}
#else
constexpr auto name_attribute="user.x64arm.stream-name";
ssize_t get_name(int fd,void* bytes,std::size_t size){return ::fgetxattr(fd,name_attribute,bytes,size);}
int set_name(int fd,std::string_view name){return ::fsetxattr(fd,name_attribute,name.data(),name.size(),0);}
#endif
std::pair<std::string,std::uint32_t> name_of(int fd){std::array<char,1024> data{};const auto count=get_name(fd,data.data(),data.size());if(count<=0)return {{},13};std::string value(data.data(),static_cast<std::size_t>(count));const auto parsed=parse("base:"+value);if(parsed.error || parsed.name!=value)return {{},13};return {value,0};}
std::uint32_t validate(int fd){struct stat st{};if(::fstat(fd,&st)<0)return error();return S_ISREG(st.st_mode) && st.st_nlink==1 && st.st_uid==::geteuid()?0U:5U;}
int child_directory(int parent,const std::string& name,bool create){
    if(create && ::mkdirat(parent,name.c_str(),0700)<0 && errno!=EEXIST)return -1;
    const auto fd=::openat(parent,name.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return -1;
    struct stat st{};if(::fstat(fd,&st)<0 || !S_ISDIR(st.st_mode) || st.st_uid!=::geteuid() || (st.st_mode&0077)){::close(fd);errno=EACCES;return -1;}return fd;
}
std::uint32_t validate_name(std::string_view name){const auto parsed=parse("base:"+std::string(name));return parsed.error?parsed.error:parsed.name.empty() || parsed.name!=name?123U:0U;}
}
#endif
std::string storage_key(std::string_view name){
#if !defined(_WIN32)
    return key(name);
#else
    (void)name;return {};
#endif
}
std::pair<std::string,std::uint32_t> descriptor_name(int fd){
#if !defined(_WIN32)
    return name_of(fd);
#else
    (void)fd;return {{},50};
#endif
}
Directory directory(int sandbox_root,int base_descriptor,bool create){
#if defined(__APPLE__)
    struct stat base{};if(::fstat(base_descriptor,&base)<0)return {-1,error()};
    if(!S_ISREG(base.st_mode) && !S_ISDIR(base.st_mode))return {-1,5};
    // birthtime 與 generation 防止刪除後 inode 重用讀回舊資料；改名與硬連結仍共用同一組串流。
    const auto identity=std::to_string(static_cast<std::uint64_t>(base.st_dev))+"-"+std::to_string(base.st_ino)+"-"+std::to_string(base.st_birthtimespec.tv_sec)+"-"+std::to_string(base.st_birthtimespec.tv_nsec)+"-"+std::to_string(base.st_gen);
    Fd store{child_directory(sandbox_root,std::string(store_name),create)};if(store.value<0)return {-1,error()};
    const auto fd=child_directory(store.value,identity,create);return {fd,fd<0?error():0U};
#else
    (void)sandbox_root;(void)base_descriptor;(void)create;return {-1,50};
#endif
}
Open open(const Directory& dir,std::string_view name,int flags,bool create,bool exclusive){
#if !defined(_WIN32)
    if(dir.error)return {-1,dir.error,false};if(const auto code=validate_name(name))return {-1,code,false};
    const auto leaf=key(name);if(leaf.empty())return {-1,8,false};Lock lock(dir.descriptor);if(!lock.held)return {-1,error(),false};
    // 呼叫端僅指定資料能力；建立與截短須分開，避免在授權／分享檢查前破壞內容。
    if(flags&~(O_ACCMODE|O_SYNC|O_NONBLOCK))return {-1,87,false};flags|=O_CLOEXEC|O_NOFOLLOW;
    Fd file;bool existed=false;
    if(create){file.value=::openat(dir.descriptor,leaf.c_str(),flags|O_CREAT|O_EXCL,0600);if(file.value<0 && errno==EEXIST){existed=true;if(exclusive)return {-1,80,true};}}
    if(file.value<0){if(create && !existed)return {-1,error(),false};file.value=::openat(dir.descriptor,leaf.c_str(),flags);existed=true;}
    if(file.value<0)return {-1,error(),false};
    if(const auto code=validate(file.value))return {-1,code,existed};
    if(!existed){if(set_name(file.value,name)<0){const auto code=error();::unlinkat(dir.descriptor,leaf.c_str(),0);return {-1,code,false};}}
    else {const auto stored=name_of(file.value);if(stored.second || folded(stored.first)!=folded(name))return {-1,13,true};}
    return {std::exchange(file.value,-1),0,existed};
#else
    (void)dir;(void)name;(void)flags;(void)create;(void)exclusive;return {-1,50,false};
#endif
}
Listing list(const Directory& dir){
#if !defined(_WIN32)
    if(dir.error)return {{},dir.error};Lock lock(dir.descriptor);if(!lock.held)return {{},error()};
    const auto fd=::openat(dir.descriptor,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return {{},error()};
    struct Entries {DIR* value;~Entries(){if(value)::closedir(value);}} entries{::fdopendir(fd)};if(!entries.value){::close(fd);return {{},error()};}
    Listing result;
    errno=0;while(const auto* entry=::readdir(entries.value)){
        const std::string leaf=entry->d_name;if(leaf=="." || leaf=="..")continue;
        if(leaf.size()!=64 || leaf.find_first_not_of("0123456789abcdef")!=std::string::npos)return {{},13};
        Fd file{::openat(dir.descriptor,leaf.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};if(file.value<0)return {{},error()};
        if(const auto code=validate(file.value))return {{},code};const auto name=name_of(file.value);if(name.second || key(name.first)!=leaf)return {{},13};
        struct stat st{};if(::fstat(file.value,&st)<0)return {{},error()};result.entries.push_back({name.first,static_cast<std::uint64_t>(st.st_size),static_cast<std::uint64_t>(st.st_blocks)*512});errno=0;
    }
    if(errno)return {{},error()};std::sort(result.entries.begin(),result.entries.end(),[](const auto& a,const auto& b){return folded(a.name)<folded(b.name);});return result;
#else
    (void)dir;return {{},50};
#endif
}
std::uint32_t remove(const Directory& dir,std::string_view name){
#if !defined(_WIN32)
    if(dir.error)return dir.error;if(const auto code=validate_name(name))return code;const auto leaf=key(name);if(leaf.empty())return 8;
    Lock lock(dir.descriptor);if(!lock.held)return error();Fd file{::openat(dir.descriptor,leaf.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};if(file.value<0)return error();if(const auto code=validate(file.value))return code;
    const auto stored=name_of(file.value);if(stored.second || folded(stored.first)!=folded(name))return 13;return ::unlinkat(dir.descriptor,leaf.c_str(),0)<0?error():0U;
#else
    (void)dir;(void)name;return 50;
#endif
}
std::uint32_t rename(const Directory& dir,std::string_view source,std::string_view target,bool replace){
#if !defined(_WIN32)
    if(dir.error)return dir.error;if(const auto code=validate_name(source))return code;if(const auto code=validate_name(target))return code;
    const auto from=key(source),to=key(target);if(from.empty() || to.empty())return 8;Lock lock(dir.descriptor);if(!lock.held)return error();
    Fd file{::openat(dir.descriptor,from.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};if(file.value<0)return error();if(const auto code=validate(file.value))return code;
    const auto original=name_of(file.value);if(original.second || folded(original.first)!=folded(source))return 13;
    struct stat existing{};const bool exists=::fstatat(dir.descriptor,to.c_str(),&existing,AT_SYMLINK_NOFOLLOW)==0;
    if(exists && from!=to && !replace)return 183;if(!exists && errno!=ENOENT)return error();
    if(set_name(file.value,target)<0)return error();
    if(from!=to && ::renameat(dir.descriptor,from.c_str(),dir.descriptor,to.c_str())<0){const auto code=error();if(set_name(file.value,original.first)<0)return 1117;return code;}return 0;
#else
    (void)dir;(void)source;(void)target;(void)replace;return 50;
#endif
}
}
