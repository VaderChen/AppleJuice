#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <map>
#include <vector>
#include <cerrno>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm::detail {
// ObjectTypeIndex 是本虛擬平台的目錄索引，NtQueryObject 與系統快照共用。
inline constexpr std::array<std::u16string_view,17> system_object_types{
    u"Process",u"Thread",u"File",u"Token",u"PowerRequest",u"Job",u"IoCompletion",u"Section",
    u"Key",u"Event",u"Semaphore",u"Timer",u"WaitCompletionPacket",u"Mutant",u"Directory",u"TmTx",u"SymbolicLink"};
inline std::uint16_t system_object_type_index(std::u16string_view type){
    for(std::size_t n=0;n<system_object_types.size();++n)if(system_object_types[n]==type)return static_cast<std::uint16_t>(n+2);
    return 0;
}
inline std::string object_key(std::uint16_t type,char kind,std::initializer_list<std::uint64_t> words){
    std::string key;key.push_back(static_cast<char>(type));key.push_back(kind);
    for(auto word:words)for(unsigned n=0;n<8;++n)key.push_back(static_cast<char>(word>>(n*8)));
    return key;
}
inline std::string descriptor_object_key(std::uint16_t type,int descriptor){
#if !defined(_WIN32)
    struct stat info{};if(descriptor>=0 && !::fstat(descriptor,&info))
        return object_key(type,'d',{static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)});
#else
    (void)type;(void)descriptor;
#endif
    return {};
}
// 私有 session journal 將完整物件身分映到不透明的客體 Object 值。
// 不截斷／雜湊宿主指標；相同物件跨程序、跨位元數仍取得相同值。
// 只追加，持鎖驗證完整紀錄；短寫或損壞回報錯誤，不重用已公開的值。
class SystemObjectIdentities {
    int descriptor_{-1};std::uint32_t error_{},next_{0x80000010U};std::uint64_t end_{};
    std::map<std::string,std::uint32_t> entries_;
public:
    explicit SystemObjectIdentities(const std::string& session){
#if !defined(_WIN32)
        descriptor_=::open((session+"/system-object-identities").c_str(),O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
        if(descriptor_<0){error_=8;return;}
        while(::flock(descriptor_,LOCK_EX)<0){if(errno==EINTR)continue;error_=8;return;}
        struct stat info{};
        if(::fstat(descriptor_,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink!=1 || info.st_size<0 || info.st_size>64*1024*1024){error_=13;return;}
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));std::size_t done=0;
        while(done<bytes.size()){
            const auto count=::pread(descriptor_,bytes.data()+done,bytes.size()-done,static_cast<off_t>(done));
            if(count<0 && errno==EINTR)continue;if(count<=0){error_=13;return;}done+=static_cast<std::size_t>(count);
        }
        const auto get=[&](std::size_t at){std::uint32_t value=0;for(unsigned n=0;n<4;++n)value|=std::uint32_t{bytes[at+n]}<<(n*8);return value;};
        for(std::size_t at=0;at<bytes.size();){
            if(bytes.size()-at<8){error_=13;return;}
            const auto size=get(at),value=get(at+4);
            if(size<2 || size>65536 || size>bytes.size()-at-8 || value<0x80000010U || (value&15U) || value>0xffffffe0U){error_=13;return;}
            const std::string key(reinterpret_cast<const char*>(bytes.data()+at+8),size);
            if(!entries_.emplace(key,value).second){error_=13;return;}
            if(next_<=value)next_=value+16;at+=8+size;
        }
        end_=bytes.size();
#else
        (void)session;error_=50;
#endif
    }
    ~SystemObjectIdentities(){
#if !defined(_WIN32)
        if(descriptor_>=0)::close(descriptor_);
#endif
    }
    SystemObjectIdentities(const SystemObjectIdentities&)=delete;
    SystemObjectIdentities& operator=(const SystemObjectIdentities&)=delete;
    std::uint32_t error()const{return error_;}
    std::uint32_t find(const std::string& key)const{const auto at=entries_.find(key);return at==entries_.end()?0:at->second;}
    std::uint32_t resolve(const std::string& key,std::uint32_t alias=0){
        if(error_)return 0;
        if(const auto known=find(key)){if(alias && known!=alias){error_=13;return 0;}return known;}
        if(key.size()<2 || key.size()>65536){error_=13;return 0;}
        if(next_>0xffffffe0U || end_+8+key.size()>64*1024*1024){error_=8;return 0;}
        const auto value=alias?alias:next_;
#if !defined(_WIN32)
        std::vector<std::uint8_t> bytes;bytes.reserve(8+key.size());
        for(auto word:{static_cast<std::uint32_t>(key.size()),value})for(unsigned n=0;n<4;++n)bytes.push_back(static_cast<std::uint8_t>(word>>(n*8)));
        bytes.insert(bytes.end(),key.begin(),key.end());std::size_t done=0;
        while(done<bytes.size()){
            const auto count=::pwrite(descriptor_,bytes.data()+done,bytes.size()-done,static_cast<off_t>(end_+done));
            if(count<0 && errno==EINTR)continue;if(count<=0){error_=8;return 0;}done+=static_cast<std::size_t>(count);
        }
        end_+=bytes.size();entries_.emplace(key,value);if(!alias)next_+=16;return value;
#else
        (void)value;error_=50;return 0;
#endif
    }
};
}
