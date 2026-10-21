#pragma once
#include "x64arm/windows_com_rot.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm::rot {
struct Table{std::uint32_t next_cookie{1};std::uint64_t next_sequence{1};std::vector<Entry> entries;};
namespace detail {
constexpr std::array<std::uint8_t,8> magic{'W','6','4','R','O','T','0','1'},lease_magic{'W','6','4','R','L','O','0','1'};
constexpr std::size_t table_limit=64U*1024U*1024U,entry_limit=4096,wire_limit=1024U*1024U;
inline void put(Bytes& bytes,std::size_t at,unsigned size,std::uint64_t value){for(unsigned i=0;i<size;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));}
inline std::uint64_t get(std::span<const std::uint8_t> bytes,std::size_t at,unsigned size){if(at>bytes.size() || size>bytes.size()-at)throw Error{13};std::uint64_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint64_t{bytes[at+i]}<<(i*8);return value;}
inline std::uint64_t checksum(std::span<const std::uint8_t> bytes){std::uint64_t value=14695981039346656037ULL;for(const auto byte:bytes)value=(value^byte)*1099511628211ULL;return value;}
inline std::string owner_name(const std::array<std::uint8_t,16>& value){constexpr char hex[]="0123456789abcdef";std::string name="rot-owner-";for(auto byte:value){name.push_back(hex[byte>>4]);name.push_back(hex[byte&15]);}return name;}
inline void append(Bytes& out,std::u16string_view value){for(auto c:value){out.push_back(static_cast<std::uint8_t>(c));out.push_back(static_cast<std::uint8_t>(c>>8));}}
inline std::u16string text(std::span<const std::uint8_t> data,std::size_t& offset,std::size_t count){if(offset>data.size() || count>(data.size()-offset)/2)throw Error{13};std::u16string value;value.reserve(count);for(std::size_t i=0;i<count;++i)value.push_back(static_cast<char16_t>(get(data,offset+i*2,2)));offset+=count*2;return value;}
inline Bytes encode(const Table& table){
    if(table.entries.size()>entry_limit || !table.next_cookie || !table.next_sequence)throw Error{13};
    Bytes bytes(32);std::copy(magic.begin(),magic.end(),bytes.begin());put(bytes,8,4,1);put(bytes,12,4,table.entries.size());put(bytes,16,4,table.next_cookie);put(bytes,24,8,table.next_sequence);
    for(const auto& entry:table.entries){
        if(!entry.cookie || !entry.sequence || !entry.native_pid || !entry.pid || !entry.thread || (entry.flags&~3U) || std::all_of(entry.owner.begin(),entry.owner.end(),[](auto byte){return !byte;}) || entry.scope.sid.empty() || entry.scope.sid.size()>256 || entry.scope.station.size()>260 || entry.comparison.size()>2048 || entry.object.size()>wire_limit || entry.moniker.size()>wire_limit)throw Error{13};
        const auto begin=bytes.size();bytes.resize(begin+88);put(bytes,begin+4,4,entry.cookie);put(bytes,begin+8,4,entry.flags);put(bytes,begin+12,4,entry.native_pid);put(bytes,begin+16,4,entry.pid);put(bytes,begin+20,4,entry.scope.session);
        put(bytes,begin+24,8,entry.sequence);put(bytes,begin+32,8,entry.thread);put(bytes,begin+40,8,entry.changed);std::copy(entry.owner.begin(),entry.owner.end(),bytes.data()+begin+48);
        put(bytes,begin+64,4,entry.scope.sid.size());put(bytes,begin+68,4,entry.scope.station.size());put(bytes,begin+72,4,entry.comparison.size());put(bytes,begin+76,4,entry.object.size());put(bytes,begin+80,4,entry.moniker.size());
        append(bytes,entry.scope.sid);append(bytes,entry.scope.station);for(const auto* part:{&entry.comparison,&entry.object,&entry.moniker})bytes.insert(bytes.end(),part->begin(),part->end());
        bytes.resize((bytes.size()+7)&~std::size_t{7});put(bytes,begin,4,bytes.size()-begin);if(bytes.size()>table_limit-8)throw Error{8};
    }
    const auto sum=checksum(bytes);const auto end=bytes.size();bytes.resize(end+8);put(bytes,end,8,sum);return bytes;
}
inline Table decode(std::span<const std::uint8_t> bytes){
    if(bytes.size()<40 || bytes.size()>table_limit || !std::equal(magic.begin(),magic.end(),bytes.begin()) || get(bytes,8,4)!=1 || get(bytes,20,4) || get(bytes,bytes.size()-8,8)!=checksum(bytes.first(bytes.size()-8)))throw Error{13};
    Table table;table.next_cookie=static_cast<std::uint32_t>(get(bytes,16,4));table.next_sequence=get(bytes,24,8);const auto count=get(bytes,12,4);
    if(count>entry_limit || !table.next_cookie || !table.next_sequence)throw Error{13};std::size_t offset=32;table.entries.reserve(count);
    for(std::size_t i=0;i<count;++i){
        const auto length=get(bytes,offset,4);if(length<88 || (length&7U) || length>bytes.size()-8-offset)throw Error{13};const auto block=bytes.subspan(offset,length);offset+=length;
        Entry entry;entry.cookie=static_cast<std::uint32_t>(get(block,4,4));entry.flags=static_cast<std::uint32_t>(get(block,8,4));entry.native_pid=static_cast<std::uint32_t>(get(block,12,4));entry.pid=static_cast<std::uint32_t>(get(block,16,4));entry.scope.session=static_cast<std::uint32_t>(get(block,20,4));
        entry.sequence=get(block,24,8);entry.thread=get(block,32,8);entry.changed=get(block,40,8);std::copy_n(block.begin()+48,16,entry.owner.begin());
        const auto sid=get(block,64,4),station=get(block,68,4),comparison=get(block,72,4),object=get(block,76,4),moniker=get(block,80,4);if(get(block,84,4) || sid>256 || station>260 || comparison>2048 || object>wire_limit || moniker>wire_limit)throw Error{13};
        std::size_t at=88;entry.scope.sid=text(block,at,sid);entry.scope.station=text(block,at,station);
        for(auto pair:{std::pair{&entry.comparison,comparison},std::pair{&entry.object,object},std::pair{&entry.moniker,moniker}}){if(pair.second>block.size()-at)throw Error{13};pair.first->assign(block.data()+at,block.data()+at+pair.second);at+=pair.second;}
        if(block.size()-at>7 || std::any_of(block.data()+at,block.data()+block.size(),[](auto byte){return byte!=0;}) || !entry.cookie || !entry.sequence || entry.sequence>=table.next_sequence || !entry.native_pid || !entry.pid || !entry.thread || std::all_of(entry.owner.begin(),entry.owner.end(),[](auto byte){return !byte;}) || entry.scope.sid.empty() || (entry.flags&~3U))throw Error{13};
        if(std::any_of(table.entries.begin(),table.entries.end(),[&](const auto& previous){return previous.cookie==entry.cookie || previous.sequence==entry.sequence;}))throw Error{13};table.entries.push_back(std::move(entry));
    }
    if(offset!=bytes.size()-8)throw Error{13};return table;
}
inline bool visible(const Entry& entry,const Scope& client){return (entry.flags&2U) || entry.scope==client;}
}

// directory 必須由 sandbox Windows C 的安全路徑取得。所有交易重讀同一機器資料，
// 來源 lease 使用 nonce 與 flock 辨認，不能靠 PID 或檔案日期回收其他程序。
class SharedRot {
    int directory_{-1},lock_{-1},owner_{-1};std::uint32_t native_pid_{};
    std::array<std::uint8_t,16> nonce_{};std::string owner_name_;
    std::function<void(int)> initialize_file_;
    struct Lock{int fd;explicit Lock(int value):fd(value){
#if !defined(_WIN32)
        while(::flock(fd,LOCK_EX)){if(errno!=EINTR)throw Error{5};}
#else
        throw Error{50};
#endif
    }~Lock(){
#if !defined(_WIN32)
        (void)::flock(fd,LOCK_UN);
#endif
    }};
    static void validate(int fd,bool directory=false){
#if !defined(_WIN32)
        struct stat info{};if(::fstat(fd,&info) || (directory?!S_ISDIR(info.st_mode):!S_ISREG(info.st_mode)) || info.st_uid!=::geteuid() || (info.st_mode&077) || (!directory && info.st_nlink!=1))throw Error{5};
#else
        (void)fd;(void)directory;throw Error{50};
#endif
    }
    static void write_all(int fd,std::span<const std::uint8_t> bytes){
#if !defined(_WIN32)
        std::size_t done=0;while(done<bytes.size()){const auto count=::pwrite(fd,bytes.data()+done,bytes.size()-done,static_cast<off_t>(done));if(count<0 && errno==EINTR)continue;if(count<=0)throw Error{29};done+=static_cast<std::size_t>(count);}if(::ftruncate(fd,static_cast<off_t>(bytes.size())))throw Error{29};
#else
        (void)fd;(void)bytes;throw Error{50};
#endif
    }
    void validate_file(int fd){validate(fd);if(initialize_file_)initialize_file_(fd);}
    Table read(){
#if !defined(_WIN32)
        const int fd=::openat(directory_,"rot-table",O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0){if(errno==ENOENT)return {};throw Error{5};}
        try{validate_file(fd);struct stat info{};if(::fstat(fd,&info) || info.st_size<40 || info.st_size>static_cast<off_t>(detail::table_limit))throw Error{13};Bytes bytes(static_cast<std::size_t>(info.st_size));
            std::size_t done=0;while(done<bytes.size()){const auto count=::pread(fd,bytes.data()+done,bytes.size()-done,static_cast<off_t>(done));if(count<0 && errno==EINTR)continue;if(count<=0)throw Error{13};done+=static_cast<std::size_t>(count);}auto table=detail::decode(bytes);::close(fd);return table;
        }catch(...){::close(fd);throw;}
#else
        throw Error{50};
#endif
    }
    void write(const Table& table){
#if !defined(_WIN32)
        const auto bytes=detail::encode(table);const auto temporary=owner_name_+".update";
        const int fd=::openat(directory_,temporary.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)throw Error{5};
        try{validate_file(fd);write_all(fd,bytes);if(::fsync(fd))throw Error{29};if(::renameat(directory_,temporary.c_str(),directory_,"rot-table"))throw Error{5};::close(fd);}
        catch(...){::close(fd);(void)::unlinkat(directory_,temporary.c_str(),0);throw;}
#else
        (void)table;throw Error{50};
#endif
    }
    bool alive(const Entry& entry){
#if !defined(_WIN32)
        const auto name=detail::owner_name(entry.owner);const int fd=::openat(directory_,name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0){if(errno==ENOENT)return false;throw Error{5};}
        try{validate_file(fd);struct stat info{};std::array<std::uint8_t,32> bytes{};
            if(::fstat(fd,&info) || info.st_size!=static_cast<off_t>(bytes.size()) || ::pread(fd,bytes.data(),bytes.size(),0)!=static_cast<ssize_t>(bytes.size()) || !std::equal(detail::lease_magic.begin(),detail::lease_magic.end(),bytes.begin()) || !std::equal(entry.owner.begin(),entry.owner.end(),bytes.begin()+8) || detail::get(bytes,24,4)!=entry.native_pid || detail::get(bytes,28,4))throw Error{13};
            const int result=::flock(fd,LOCK_EX|LOCK_NB);const int error=errno;if(result && error!=EWOULDBLOCK && error!=EAGAIN)throw Error{5};::close(fd);return result!=0;
        }catch(...){::close(fd);throw;}
#else
        (void)entry;throw Error{50};
#endif
    }
    bool prune(Table& table){const auto size=table.entries.size();std::erase_if(table.entries,[&](const auto& entry){return !alive(entry);});return table.entries.size()!=size;}
public:
    SharedRot(int directory,const std::array<std::uint8_t,16>& nonce,std::function<void(int)> initialize={}):nonce_(nonce),owner_name_(detail::owner_name(nonce)),initialize_file_(std::move(initialize)){
#if !defined(_WIN32)
        if(std::all_of(nonce.begin(),nonce.end(),[](auto byte){return !byte;}))throw Error{87};native_pid_=static_cast<std::uint32_t>(::getpid());
        directory_=::fcntl(directory,F_DUPFD_CLOEXEC,0);if(directory_<0)throw Error{5};
        try{validate(directory_,true);lock_=::openat(directory_,"rot-lock",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(lock_<0)throw Error{5};validate_file(lock_);Lock guard(lock_);
            owner_=::openat(directory_,owner_name_.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(owner_<0)throw Error{183};validate_file(owner_);if(::flock(owner_,LOCK_EX|LOCK_NB))throw Error{5};
            Bytes bytes(32);std::copy(detail::lease_magic.begin(),detail::lease_magic.end(),bytes.begin());std::copy(nonce.begin(),nonce.end(),bytes.begin()+8);detail::put(bytes,24,4,native_pid_);write_all(owner_,bytes);
        }catch(...){if(owner_>=0){::close(owner_);(void)::unlinkat(directory_,owner_name_.c_str(),0);}if(lock_>=0)::close(lock_);::close(directory_);throw;}
#else
        (void)directory;throw Error{50};
#endif
    }
    SharedRot(const SharedRot&)=delete;SharedRot& operator=(const SharedRot&)=delete;
    const std::array<std::uint8_t,16>& nonce()const noexcept{return nonce_;}
    int duplicate_directory()const{
#if !defined(_WIN32)
        const auto descriptor=::fcntl(directory_,F_DUPFD_CLOEXEC,0);if(descriptor<0)throw Error{5};return descriptor;
#else
        throw Error{50};
#endif
    }
    ~SharedRot(){
#if !defined(_WIN32)
        if(directory_>=0 && owner_>=0 && native_pid_==static_cast<std::uint32_t>(::getpid()))try{Lock guard(lock_);auto table=read();const auto count=table.entries.size();std::erase_if(table.entries,[&](const auto& entry){return entry.owner==nonce_ && entry.native_pid==native_pid_;});if(count!=table.entries.size())write(table);(void)::unlinkat(directory_,owner_name_.c_str(),0);}catch(...){}
        if(owner_>=0)::close(owner_);if(lock_>=0)::close(lock_);if(directory_>=0)::close(directory_);
#endif
    }
    std::pair<Entry,bool> publish(Entry entry,bool allow_any_authorized){
        if((entry.flags&~3U) || ((entry.flags&2U) && !allow_any_authorized))throw Error{(entry.flags&~3U)?87U:5U};
        Lock guard(lock_);auto table=read();(void)prune(table);if(table.entries.size()>=detail::entry_limit || table.next_sequence==UINT64_MAX)throw Error{8};
        const bool duplicate=!entry.comparison.empty() && std::any_of(table.entries.begin(),table.entries.end(),[&](const auto& previous){return previous.comparison==entry.comparison && detail::visible(previous,entry.scope);});
        auto cookie=table.next_cookie;while(!cookie || std::any_of(table.entries.begin(),table.entries.end(),[&](const auto& previous){return previous.cookie==cookie;}))++cookie;
        entry.cookie=cookie;entry.sequence=table.next_sequence++;entry.native_pid=native_pid_;entry.owner=nonce_;table.next_cookie=cookie+1;if(!table.next_cookie)table.next_cookie=1;
        table.entries.push_back(entry);write(table);return {std::move(entry),duplicate};
    }
    std::optional<Entry> revoke(std::uint32_t cookie){Lock guard(lock_);auto table=read();const bool stale=prune(table);const auto found=std::find_if(table.entries.begin(),table.entries.end(),[&](const auto& entry){return entry.cookie==cookie && entry.owner==nonce_ && entry.native_pid==native_pid_;});if(found==table.entries.end()){if(stale)write(table);return std::nullopt;}auto entry=*found;table.entries.erase(found);write(table);return entry;}
    bool change(std::uint32_t cookie,std::uint64_t filetime){Lock guard(lock_);auto table=read();const bool stale=prune(table);const auto found=std::find_if(table.entries.begin(),table.entries.end(),[&](const auto& entry){return entry.cookie==cookie && entry.owner==nonce_ && entry.native_pid==native_pid_;});if(found==table.entries.end()){if(stale)write(table);return false;}found->changed=filetime;write(table);return true;}
    void move_thread(std::uint64_t from,std::uint64_t to){
        if(!from || !to)throw Error{87};Lock guard(lock_);auto table=read();bool changed=prune(table);
        for(auto& entry:table.entries)if(entry.owner==nonce_ && entry.native_pid==native_pid_ && entry.thread==from){entry.thread=to;changed=true;}
        if(changed)write(table);
    }
    void retire_thread(std::uint64_t thread){
        if(!thread)throw Error{87};Lock guard(lock_);auto table=read();const bool stale=prune(table);const auto size=table.entries.size();
        std::erase_if(table.entries,[&](const auto& entry){return entry.owner==nonce_ && entry.native_pid==native_pid_ && entry.thread==thread;});
        if(stale || size!=table.entries.size())write(table);
    }
    std::vector<Entry> snapshot(const Scope& scope){Lock guard(lock_);auto table=read();if(prune(table))write(table);std::erase_if(table.entries,[&](const auto& entry){return !detail::visible(entry,scope);});return std::move(table.entries);}
    std::optional<Entry> find(const Scope& scope,std::span<const std::uint8_t> comparison){auto entries=snapshot(scope);for(auto& entry:entries)if(std::equal(entry.comparison.begin(),entry.comparison.end(),comparison.begin(),comparison.end()))return std::move(entry);return std::nullopt;}
};
}
