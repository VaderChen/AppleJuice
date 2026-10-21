#include "x64arm/file_reparse.hpp"
#include "x64arm/windows_paths.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <span>
#include <stdexcept>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <sys/xattr.h>
#endif
namespace x64arm::file_reparse {
namespace {
#if !defined(_WIN32)
// 保留上游既有的可攜連結格式，並拒絕未經客體建立的宿主 symlink。
FileReparse read_legacy(int directory,const std::string& leaf){
    FileReparse r;struct stat before{},after{};
    if(::fstatat(directory,leaf.c_str(),&before,AT_SYMLINK_NOFOLLOW)<0){r.error=errno==ENOENT?2U:errno==ENOTDIR?3U:5U;return r;}
    if(!S_ISLNK(before.st_mode))return r;
    std::array<char,32768> bytes{};
    const auto size=::readlinkat(directory,leaf.c_str(),bytes.data(),bytes.size());
    if(size<0 || static_cast<std::size_t>(size)==bytes.size()){r.error=5;return r;}
    const std::string stored(bytes.data(),static_cast<std::size_t>(size));
    if(!stored.starts_with("x64arm-link-v1:0:") && !stored.starts_with("x64arm-link-v1:1:")){r.error=5;return r;}
    if(::fstatat(directory,leaf.c_str(),&after,AT_SYMLINK_NOFOLLOW)<0 || before.st_dev!=after.st_dev || before.st_ino!=after.st_ino){r.error=32;return r;}
    r.target=stored.substr(17);if(r.target.empty()){r.error=5;return r;}
    r.physical=stored;r.kind=stored[15]=='1'?2U:1U;r.attributes=r.kind==2?0x410U:0x420U;
    r.device=static_cast<std::uint64_t>(before.st_dev);r.id=before.st_ino;r.error=0;return r;
}
#endif
#if defined(__APPLE__)
constexpr auto attribute="com.x64arm.reparse";
constexpr std::array<std::uint8_t,8> magic{'x','6','4','r','l','k',1,0};
struct Fd {int value{-1};~Fd(){if(value>=0)::close(value);}};
std::uint32_t error(){switch(errno){case ENOENT:return 2;case ENOTDIR:return 3;case EACCES:case EPERM:case ELOOP:return 5;case EINVAL:return 87;case ENOTSUP:return 50;case ENOSPC:case EDQUOT:return 112;case ENAMETOOLONG:return 206;default:return 1117;}}
std::uint32_t number(std::span<const std::uint8_t> bytes,std::size_t at){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t{bytes[at+i]}<<(i*8);return v;}
void put(std::vector<std::uint8_t>& bytes,std::uint32_t v){for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(v>>(i*8)));}
#endif
}
// Darwin 的 O_SYMLINK 固定開啟連結本身；與 O_NOFOLLOW 併用反而回 ELOOP。
FileReparse read(int directory,std::string_view name){
#if defined(__APPLE__)
    FileReparse r;const std::string leaf(name);Fd fd{::openat(directory,leaf.c_str(),O_RDONLY|O_SYMLINK|O_CLOEXEC)};
    if(fd.value<0){r.error=error();return r;}struct stat info{};if(::fstat(fd.value,&info)<0){r.error=error();return r;}
    if(!S_ISLNK(info.st_mode))return r;
    const auto size=::fgetxattr(fd.value,attribute,nullptr,0,0,0);
    if(size<0 && errno==ENOATTR){
        auto legacy=read_legacy(directory,leaf);
        if(!legacy.error && (legacy.device!=static_cast<std::uint64_t>(info.st_dev) || legacy.id!=info.st_ino))legacy.error=32;
        std::array<std::uint8_t,8> dos{};
        if(!legacy.error && ::fgetxattr(fd.value,"com.x64arm.dosattributes",dos.data(),dos.size(),0,0)==8 && dos[0]=='x' && dos[1]=='6' && dos[2]=='4' && dos[3]==1)
            legacy.attributes=(number(dos,4)&0x3127U)|0x400U|(legacy.kind==2?0x10U:0U);
        return legacy;
    }
    if(size<20 || size>131072){r.error=5;return r;}
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));if(::fgetxattr(fd.value,attribute,bytes.data(),bytes.size(),0,0)!=size){r.error=5;return r;}
    if(!std::equal(magic.begin(),magic.end(),bytes.begin())){r.error=5;return r;}
    r.kind=number(bytes,8);const auto a=number(bytes,12),b=number(bytes,16);
    if((r.kind!=1 && r.kind!=2) || !a || !b || a>65536 || b>65536 || std::uint64_t{a}+b+20!=bytes.size()){r.error=5;return r;}
    r.target.assign(reinterpret_cast<const char*>(bytes.data()+20),a);r.physical.assign(reinterpret_cast<const char*>(bytes.data()+20+a),b);
    if(r.target.find('\0')!=r.target.npos || r.physical.find('\0')!=r.physical.npos){r.error=5;return r;}
    std::vector<char> physical(static_cast<std::size_t>(b)+1);const auto actual=::readlinkat(directory,leaf.c_str(),physical.data(),physical.size());
    struct stat current{};if(actual!=static_cast<ssize_t>(b) || std::string_view(physical.data(),b)!=r.physical || ::fstatat(directory,leaf.c_str(),&current,AT_SYMLINK_NOFOLLOW)<0 || current.st_dev!=info.st_dev || current.st_ino!=info.st_ino){r.error=5;return r;}
    r.attributes=r.kind==2?0x410U:0x420U;std::array<std::uint8_t,8> dos{};
    if(::fgetxattr(fd.value,"com.x64arm.dosattributes",dos.data(),dos.size(),0,0)==8 && dos[0]=='x' && dos[1]=='6' && dos[2]=='4' && dos[3]==1)
        r.attributes=(number(dos,4)&0x3127U)|0x400U|(r.kind==2?0x10U:0U);
    r.device=static_cast<std::uint64_t>(info.st_dev);r.id=info.st_ino;r.error=0;return r;
#elif !defined(_WIN32)
    return read_legacy(directory,std::string(name));
#else
    (void)directory;(void)name;return {{},{},0,0,0,50};
#endif
}
std::uint32_t mark(int directory,std::string_view name,std::uint32_t kind,std::string_view target,std::string_view physical){
#if defined(__APPLE__)
    if((kind!=1 && kind!=2) || target.empty() || physical.empty() || target.size()>65536 || physical.size()>65536)return 87;
    const std::string leaf(name);Fd fd{::openat(directory,leaf.c_str(),O_RDONLY|O_SYMLINK|O_CLOEXEC)};if(fd.value<0)return error();
    struct stat info{};if(::fstat(fd.value,&info)<0)return error();if(!S_ISLNK(info.st_mode))return 5;
    std::vector<char> actual(physical.size()+1);const auto size=::readlinkat(directory,leaf.c_str(),actual.data(),actual.size());
    if(size!=static_cast<ssize_t>(physical.size()) || std::string_view(actual.data(),physical.size())!=physical)return 5;
    std::vector<std::uint8_t> bytes(magic.begin(),magic.end());put(bytes,kind);put(bytes,static_cast<std::uint32_t>(target.size()));put(bytes,static_cast<std::uint32_t>(physical.size()));
    bytes.insert(bytes.end(),target.begin(),target.end());bytes.insert(bytes.end(),physical.begin(),physical.end());
    if(::fsetxattr(fd.value,attribute,bytes.data(),bytes.size(),0,0)<0)return error();const auto verified=read(directory,name);
    return verified.error?verified.error:verified.device!=static_cast<std::uint64_t>(info.st_dev) || verified.id!=info.st_ino?5U:0U;
#else
    (void)directory;(void)name;(void)kind;(void)target;(void)physical;return 50;
#endif
}
std::uint32_t set_attributes(int directory,std::string_view name,std::uint32_t attributes){
#if defined(__APPLE__)
    if(attributes&~0x31b7U)return 50;const auto original=read(directory,name);if(original.error)return original.error;
    const std::string leaf(name);Fd fd{::openat(directory,leaf.c_str(),O_RDONLY|O_SYMLINK|O_CLOEXEC)};if(fd.value<0)return error();
    struct stat info{};if(::fstat(fd.value,&info)<0)return error();if(static_cast<std::uint64_t>(info.st_dev)!=original.device || info.st_ino!=original.id)return 32;
    std::array<std::uint8_t,8> data{'x','6','4',1};attributes&=0x3127U;for(unsigned i=0;i<4;++i)data[4+i]=static_cast<std::uint8_t>(attributes>>(i*8));
    return ::fsetxattr(fd.value,"com.x64arm.dosattributes",data.data(),data.size(),0,0)<0?error():0;
#else
    (void)directory;(void)name;(void)attributes;return 50;
#endif
}
std::string target_path(std::string_view link,std::string_view input,std::string_view current,bool strict_root){
    std::string target(input);std::replace(target.begin(),target.end(),'\\','/');if(target.empty() || target.find('\0')!=target.npos)throw std::invalid_argument("空白或無效連結目標");
    std::string prefix;
    if(const auto volume=windows_volume_letter(std::string_view(target))){prefix=windows_volume_path(volume);std::replace(prefix.begin(),prefix.end(),'\\','/');target.erase(0,49);}
    else if(target.starts_with("//?/"))target.erase(0,4);
    if(prefix.empty() && target.starts_with("//"))throw std::invalid_argument("未映射的 UNC 連結目標");
    const auto parent=std::string(link.substr(0,link.find_last_of('/')+1));
    const bool internal=link.starts_with("relative:/");
    std::string link_root=internal?"relative:/":windows_volume_letter(link)?std::string(link.substr(0,49)):std::string(link.substr(0,2))+"/";
    std::replace(link_root.begin(),link_root.end(),'\\','/');
    if(!prefix.empty()){}
    else if(target.size()>=2 && target[1]==':'){
        char letter=target[0];if(letter>='a' && letter<='z')letter=static_cast<char>(letter-32);if(letter<'A' || letter>'Z')throw std::invalid_argument("無效磁碟代號");
        prefix=std::string(1,letter)+":/";target.erase(0,2);
        if(!target.empty() && target[0]!='/'){
            if(current.size()>1 && current[1]==':' && (current[0]==letter || current[0]==letter+32))target=std::string(current.substr(3))+"/"+target;
        }
    }else if(target[0]=='/')prefix=current.size()>1 && current[1]==':'?std::string(current.substr(0,2))+"/":link_root;
    else {prefix=link_root;target=parent.substr(prefix.size())+target;}
    if(target.find(':')!=target.npos)throw std::invalid_argument("目標路徑包含非法分隔符");
    std::vector<std::string> parts;for(std::size_t at=0;at<target.size();){const auto end=target.find('/',at);const auto part=target.substr(at,end==target.npos?target.size()-at:end-at);at=end==target.npos?target.size():end+1;
        if(part==".."){if(!parts.empty())parts.pop_back();else if(strict_root)throw std::invalid_argument("讀取能力不允許連結跨越根目錄");}else if(!part.empty() && part!=".")parts.push_back(part);}
    for(const auto& part:parts){if(prefix.back()!='/')prefix+='/';prefix+=part;}return prefix;
}
}
