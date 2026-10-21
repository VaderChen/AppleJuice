#include "x64arm/readonly_files.hpp"
#include "x64arm/windows_paths.hpp"
#include <algorithm>
#include <cerrno>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace x64arm {
namespace {
#if !defined(_WIN32)
struct Fd {int value{-1};~Fd(){if(value>=0)::close(value);}};
[[maybe_unused]] std::uint32_t error(){switch(errno){case ENOENT:return 2;case ENOTDIR:return 3;case EACCES:case EPERM:case ELOOP:return 5;case EEXIST:return 183;case EINVAL:return 87;case ENOTSUP:return 50;case ENAMETOOLONG:return 206;case ENOSPC:case EDQUOT:return 112;default:return 1117;}}
#endif
}
FileReparse ReadOnlyFiles::reparse(std::string path) const {
#if !defined(_WIN32)
    std::string leaf;const auto parent=resolve(path,true,&leaf);Fd fd{parent.descriptor};
    if(parent.error && parent.readonly_file){
        // 唯讀 EXE／來源目錄不提供可寫 parent 能力；仍須允許原有的唯讀開啟。
        // 以原本 NOFOLLOW 解析取得內容 descriptor，不能略過真正的 symlink 拒絕。
        const auto mapped=resolve(std::move(path));Fd source{mapped.descriptor};struct stat info{};
        if(mapped.error)return {{},{},0,0,0,mapped.error};
        if(::fstat(source.value,&info)<0)return {{},{},0,0,0,error()};
        if(mapped.readonly_file && (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode)))return {};
    }
    if(parent.error)return {{},{},0,0,0,parent.error};struct stat info{};
    if(::fstatat(fd.value,leaf.c_str(),&info,AT_SYMLINK_NOFOLLOW)<0)return {{},{},0,0,0,error()};
    if(S_ISDIR(info.st_mode)){
        Fd directory{::openat(fd.value,leaf.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};if(directory.value<0)return {};
        const auto point=volumes_->reparse(directory.value);if(point.error==4390 || point.error==50)return {};if(point.error)return {{},{},0,0,0,point.error};
        return {windows_volume_path(point.mount.target),{},point.mount.device,point.mount.inode,3,0,0x410U};
    }
    if(!S_ISLNK(info.st_mode))return {};return file_reparse::read(fd.value,leaf);
#else
    (void)path;return {{},{},0,0,0,50};
#endif
}
std::pair<std::string,std::uint32_t> ReadOnlyFiles::follow_reparse(std::string path,bool strict_root) const {
    for(unsigned i=0;i<=32;++i){const auto link=reparse(path);if(link.error)return {std::move(path),link.error==4390 || link.error==2 || link.error==3?0U:link.error};
        if(i==32)return {{},1921};std::string leaf;const auto parent=resolve(path,true,&leaf);
#if !defined(_WIN32)
        Fd fd{parent.descriptor};
#endif
        if(parent.error)return {{},parent.error};try{path=file_reparse::target_path(parent.key,link.target,current_directory_,strict_root);if(path.starts_with("relative:/"))path.erase(0,10);}catch(const std::invalid_argument&){return {{},5};}
    }return {{},1921};
}
FileInformation ReadOnlyFiles::target_information(std::string path){
    const auto followed=follow_reparse(path);if(followed.second && followed.second!=50)return {{},{},followed.second};
    return information(followed.second?std::move(path):followed.first);
}
std::pair<std::filesystem::path,std::uint32_t> ReadOnlyFiles::link_host_path(std::string path) const {
    std::replace(path.begin(),path.end(),'\\','/');const bool relative=path.starts_with("relative:/");
    const auto guid=windows_volume_letter(std::string_view(path));
    if((!relative && !guid && (path.size()<3 || path[1]!=':' || path[2]!='/')) || path.find('\0')!=path.npos)return {{},87};
    auto suffix=path.substr(relative?10:guid?49:3);char volume=relative?relative_volume_:guid;
    if(!relative && !guid){char drive=path[0];if(drive>='a' && drive<='z')drive=static_cast<char>(drive-32);const auto state=volumes_->snapshot();if(state.error)return {{},state.error};if(const auto found=state.drives.find(drive);found!=state.drives.end())volume=found->second;}
    if(volume)path=std::string(1,volume)+":/"+suffix;
    auto key=path;for(auto& c:key)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
    if(const auto found=readonly_files_.find(key);found!=readonly_files_.end())return {found->second,0};
    const std::pair<const std::string,std::filesystem::path>* mount=nullptr;
    for(const auto& entry:readonly_mounts_)if((key.starts_with(entry.first) || key+'/'==entry.first) && (!mount || entry.first.size()>mount->first.size()))mount=&entry;
    if(mount)return {mount->second/(path.size()>=mount->first.size()?path.substr(mount->first.size()):std::string{}),0};
    auto root=relative?root_:std::filesystem::path{};for(const auto& entry:volumes_->volumes())if(entry.id==volume)root=entry.host_path;
    if(!relative && !install_root_.empty() && (key=="c:/program files" || key.starts_with("c:/program files/"))){root=install_root_;suffix=suffix.size()>14?suffix.substr(14):std::string{};}
    if(root.empty()){for(const auto& [prefix,host]:application_sources_)if(key.starts_with(prefix))return {host/path.substr(prefix.size()),0};return {{},3};}
    return {root/suffix,0};
}
std::uint32_t ReadOnlyFiles::symbolic_link_reparse(std::string target,std::string link,bool directory) try {
    if(!writable_)return 5;
#if !defined(__APPLE__)
    (void)target;(void)link;(void)directory;return 50;
#else
    std::string leaf;const auto parent=resolve(std::move(link),true,&leaf);Fd fd{parent.descriptor};if(parent.error)return parent.error;if(parent.readonly_file)return 5;
    std::string absolute;try{absolute=file_reparse::target_path(parent.key,target,current_directory_);}catch(const std::invalid_argument&){return 87;}
    auto host=link_host_path(absolute);if(host.second)return host.second;
    std::string target_leaf;const auto existing=resolve(absolute,true,&target_leaf);Fd target_fd{existing.descriptor};
    if(!existing.error && !existing.host_path.empty())host.first=existing.host_path;
    std::replace(target.begin(),target.end(),'\\','/');const bool cwd_relative=target.size()>1 && target[1]==':' && (target.size()==2 || target[2]!='/');
    if(cwd_relative || (!target.empty() && target[0]=='/' && !target.starts_with("//")))target=absolute;
    const bool relative=target.find(':')==target.npos && !target.starts_with("//");
    const auto physical=(relative?host.first.lexically_relative(parent.host_path.parent_path()):host.first).generic_string();if(physical.empty())return 87;
    if(::symlinkat(physical.c_str(),fd.value,leaf.c_str())<0)return error();struct stat created{};
    if(::fstatat(fd.value,leaf.c_str(),&created,AT_SYMLINK_NOFOLLOW)<0)return error();
    const auto marked=file_reparse::mark(fd.value,leaf,directory?2U:1U,target,physical);
    if(marked){struct stat current{};if(::fstatat(fd.value,leaf.c_str(),&current,AT_SYMLINK_NOFOLLOW)==0 && current.st_dev==created.st_dev && current.st_ino==created.st_ino)(void)::unlinkat(fd.value,leaf.c_str(),0);return marked;}
    cache_=std::make_shared<Cache>();return 0;
#endif
}catch(const std::bad_alloc&){return 8;}
}
