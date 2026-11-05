#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#if defined(__APPLE__)
#include <fcntl.h>
#include <libproc.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm::detail {
#if defined(__APPLE__)
// 上次完整掃描確認仍持有具名物件 fd 的程序。再次開啟同一 inode 時，先以一次 proc_pidfdinfo 核對該 fd
// 仍指向同一檔案：成立即代表核心中確有存活引用，不必在命名空間鎖內掃描全系統程序（Steam 執行遊戲時反覆
// 開啟同一 IPC 物件，原本每次都掃描全部程序並長時間持鎖，同一命名空間的遊戲排程因此等鎖）。核對失敗才回到
// 完整掃描並重新記錄；回收仍只依完整掃描的結果。X64ARM_NAMED_OBJECT_HOLDER_CACHE=0 關閉。
struct NamedObjectHolder {pid_t pid{};int fd{-1};};
struct NamedObjectHolderCache {std::mutex mutex;std::map<std::pair<std::uint64_t,std::uint64_t>,NamedObjectHolder> holders;};
inline NamedObjectHolderCache& named_object_holder_cache() {static NamedObjectHolderCache value;return value;}
inline bool named_object_holder_cache_enabled() {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_NAMED_OBJECT_HOLDER_CACHE");return !value || std::string_view(value)!="0";}();
    return enabled;
}
inline bool named_object_fd_matches(pid_t pid,int fd,const struct stat& identity) {
    vnode_fdinfo info{};
    if(fd<0 || ::proc_pidfdinfo(pid,fd,PROC_PIDFDVNODEINFO,&info,sizeof(info))!=static_cast<int>(sizeof(info)))return false;
    return static_cast<std::uint64_t>(info.pvi.vi_stat.vst_dev)==static_cast<std::uint64_t>(static_cast<std::uint32_t>(identity.st_dev)) &&
        static_cast<std::uint64_t>(info.pvi.vi_stat.vst_ino)==static_cast<std::uint64_t>(identity.st_ino);
}
inline int named_object_find_fd(pid_t pid,const struct stat& identity) {
    const int needed=::proc_pidinfo(pid,PROC_PIDLISTFDS,0,nullptr,0);
    if(needed<=0)return -1;
    std::vector<proc_fdinfo> fds(static_cast<std::size_t>(needed)/sizeof(proc_fdinfo)+32);
    const int bytes=::proc_pidinfo(pid,PROC_PIDLISTFDS,0,fds.data(),static_cast<int>(fds.size()*sizeof(proc_fdinfo)));
    for(std::size_t i=0;bytes>0 && i<static_cast<std::size_t>(bytes)/sizeof(proc_fdinfo);++i)
        if(fds[i].proc_fdtype==PROX_FDTYPE_VNODE && named_object_fd_matches(pid,fds[i].proc_fd,identity))return fds[i].proc_fd;
    return -1;
}
// 跨程序持有者提示：建立者（或完整掃描確認的持有者）把 {pid, fd} 寫在物件標頭未使用的尾端；之後其他程序開啟時，
// 先以一次 proc_pidfdinfo 核對該 fd 仍指向同一 inode，成立即證明仍有存活引用，不必掃描全系統程序（Steam IPC 每條
// 連線都新建對應檔、互斥鎖與事件，遊戲第一次開啟時原本每個物件都要完整掃描約百毫秒，協調者因此連續數秒無法服務）。
// 提示只作為存活證明，不作為死亡判定：核對失敗（程序結束、fd 已關或改指他檔）就回到完整掃描，舊版引擎不寫提示也
// 一樣走完整掃描。讀寫都在命名空間鎖內。X64ARM_NAMED_OBJECT_HOLDER_HINT=0 關閉。
struct NamedObjectHolderHint {std::uint64_t signature{};std::int32_t pid{},fd{-1};};
inline constexpr std::uint64_t named_object_hint_magic=UINT64_C(0x444c4f4852413458);
// 4 KiB 物件（事件、semaphore、mutex）的標頭結構都小於 1 KiB；對應檔的標頭區為 64 KiB（資料自 65536 起）。
inline constexpr off_t named_object_page_hint_offset=4096-64,named_object_mapping_hint_offset=65536-64;
inline bool named_object_hint_enabled() {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_NAMED_OBJECT_HOLDER_HINT");return !value || std::string_view(value)!="0";}();
    return enabled;
}
inline void named_object_record_holder(int descriptor,off_t offset,pid_t pid,int fd) {
    if(!named_object_hint_enabled() || descriptor<0 || fd<0)return;
    const NamedObjectHolderHint hint{named_object_hint_magic,static_cast<std::int32_t>(pid),fd};
    (void)::pwrite(descriptor,&hint,sizeof(hint),offset);
}
// probe 是本次探查用的 fd，不能算成持有者。
inline bool named_object_hint_alive(int probe,off_t offset,const struct stat& identity) {
    if(!named_object_hint_enabled())return false;
    NamedObjectHolderHint hint{};
    if(::pread(probe,&hint,sizeof(hint),offset)!=static_cast<ssize_t>(sizeof(hint)) || hint.signature!=named_object_hint_magic || hint.pid<=0 || hint.fd<0)return false;
    if(hint.pid==::getpid() && hint.fd==probe)return false;
    return named_object_fd_matches(hint.pid,hint.fd,identity);
}
#endif
// 呼叫端持有命名空間鎖。引用計數無法涵蓋 SIGKILL，因此只在核心確認
// 沒有 fd／映射／繼承中的引用後回收名稱；持久的內部 table 不呼叫此函式。
inline int reopen_live_named_object(const std::string& session,int directory,
                                   const std::string& name,int descriptor) {
#if defined(__APPLE__)
    if(descriptor<0)return descriptor;
    struct stat before{};
    std::array<std::uint8_t,576> header{};
    if(::fstat(descriptor,&before) || !S_ISREG(before.st_mode) || before.st_uid!=::geteuid() ||
       (before.st_mode&077) || ::pread(descriptor,header.data(),header.size(),0)!=static_cast<ssize_t>(header.size()))return descriptor;
    const auto qword=[&](unsigned at){std::uint64_t value;std::memcpy(&value,header.data()+at,sizeof(value));return value;};
    const auto dword=[&](unsigned at){std::uint32_t value;std::memcpy(&value,header.data()+at,sizeof(value));return value;};
    bool known=false;
    switch(qword(0)) {
    case UINT64_C(0x544e564552413458):known=before.st_size==4096 && dword(24)>0 && dword(24)<260;break;
    case UINT64_C(0x504d455352413458):known=before.st_size==4096 && dword(24)>0 && dword(24)<260 && dword(548)>0 && dword(548)<=INT32_MAX;break;
    case UINT64_C(0x5854554d52413458):known=before.st_size==4096 && dword(40)>0 && dword(40)<260;break;
    case UINT64_C(0x50414d4d52413458):known=before.st_size>65536 && qword(8)>0 &&
        qword(8)<=static_cast<std::uint64_t>(before.st_size)-65536 && dword(28)>0 && dword(28)<260;break;
    default:break;
    }
    if(!known)return descriptor;
    const off_t hint_offset=qword(0)==UINT64_C(0x50414d4d52413458)?named_object_mapping_hint_offset:named_object_page_hint_offset;
    const bool cache=named_object_holder_cache_enabled();
    const std::pair<std::uint64_t,std::uint64_t> identity{static_cast<std::uint64_t>(static_cast<std::uint32_t>(before.st_dev)),static_cast<std::uint64_t>(before.st_ino)};
    if(cache) {
        NamedObjectHolder holder;bool cached=false;
        {auto& value=named_object_holder_cache();std::lock_guard lock(value.mutex);
         if(const auto found=value.holders.find(identity);found!=value.holders.end()){holder=found->second;cached=true;}}
        // 本次探查用的 fd 不能算成持有者；其他 fd（含本程序既有 handle）指向同一 inode 即為存活引用。
        if(cached && !(holder.pid==::getpid() && holder.fd==descriptor) && named_object_fd_matches(holder.pid,holder.fd,before))
            return descriptor;
    }
    if(named_object_hint_alive(descriptor,hint_offset,before))return descriptor;
    // 本次探查的 fd 不得被算成仍然存在的 Windows handle。
    ::close(descriptor);
    std::array<int,256> holders{};
    const auto path=session+"/"+name;
    const int bytes=::proc_listpidspath(PROC_ALL_PIDS,0,path.c_str(),0,holders.data(),sizeof(holders));
    // 記錄一個確認持有 fd 的程序；只靠映射、沒有 fd 的持有者不記錄，下次照常完整掃描。
    NamedObjectHolder found{};
    if(cache || named_object_hint_enabled())
        for(std::size_t i=0;bytes>0 && i<static_cast<std::size_t>(bytes)/sizeof(int) && i<holders.size() && found.fd<0;++i)
            if(holders[i]>0){const auto fd=named_object_find_fd(holders[i],before);if(fd>=0)found={holders[i],fd};}
    if(cache) {
        auto& value=named_object_holder_cache();std::lock_guard lock(value.mutex);
        if(found.fd>=0){if(value.holders.size()>=4096)value.holders.clear();value.holders[identity]=found;}
        else value.holders.erase(identity);
    }
    struct stat current{};
    if(bytes==0 && ::fstatat(directory,name.c_str(),&current,AT_SYMLINK_NOFOLLOW)==0 &&
       current.st_dev==before.st_dev && current.st_ino==before.st_ino) {
        if(::unlinkat(directory,name.c_str(),0)<0)return -1;
    }
    // 查詢失敗、結果截斷或仍有核心引用時都保留原物件。
    const int reopened=::openat(directory,name.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    // 完整掃描確認存活：留下長期持有者（找不到有 fd 的持有者時改記本程序的新 fd），下一個開啟者免掃描。
    if(reopened>=0 && bytes>0) {
        struct stat after{};
        if(!::fstat(reopened,&after) && after.st_dev==before.st_dev && after.st_ino==before.st_ino)
            found.fd>=0?named_object_record_holder(reopened,hint_offset,found.pid,found.fd):named_object_record_holder(reopened,hint_offset,::getpid(),reopened);
    }
    return reopened;
#else
    (void)session;(void)directory;(void)name;return descriptor;
#endif
}
}
