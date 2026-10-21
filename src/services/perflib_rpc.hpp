#pragma once
#include "perflib_data.hpp"
#include <cerrno>
#include <atomic>
#include <memory>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm::perflib::rpc {
inline constexpr std::uint32_t family=1,add=1,remove=2,enumerate=3,collect=5,close_query=7,probe_endpoint=8;
inline constexpr std::uint32_t ensure_counter=9,undo_counter=10,reconcile_query=11;
inline constexpr std::uint32_t endpoint_live=1,endpoint_retired=2;
inline constexpr std::uint64_t unaccepted_busy=UINT64_C(0x3159535542465250); // PRFBUSY1
inline constexpr std::size_t limit=16U*1024U*1024U,header_size=32;
inline constexpr std::uint64_t signature=UINT64_C(0x3151465250343658); // X64PRFQ1
// accounting：輸入長度、輸出容量、publication、provider handle、query cookie。
// 資料檔先放 request，結果自輸入長度後開始；不傳遞客體指標或私人 offset。
// 回覆 accounting[3]：Collect 為 BEGIN status；Probe 為 endpoint_live／endpoint_retired。
// Ensure 回覆 accounting[4] 表示本請求是否新建參照；Undo 的 header[24..31] 綁定原 Add／Ensure 序號。
// Reconcile 傳送需保留的 selector，回覆為 {0,0,0,endpoint_state,0}；不回傳 counter payload。
// 尚未接受的忙碌請求：error=170、mode=0、accounting={0,0,0,unaccepted_busy,0}。
// 此標記保證未建立工作或變更訂閱；未帶標記的 provider 錯誤不可自動重送。
inline bool supported(std::uint32_t operation){return operation==add || operation==remove || operation==enumerate || operation==collect || operation==close_query || operation==probe_endpoint || operation==ensure_counter || operation==undo_counter || operation==reconcile_query;}
inline bool subscribing(std::uint32_t operation){return operation==add || operation==ensure_counter;}
inline bool removing(std::uint32_t operation){return operation==remove || operation==undo_counter;}
inline bool empty_request(std::uint32_t operation){return operation==close_query || operation==probe_endpoint;}
inline bool no_output(std::uint32_t operation){return empty_request(operation) || operation==reconcile_query;}
inline Bytes encoded_identifier(const Identifier& value){
    auto encoded=identifier(value);
    if(value.size>encoded.size() && value.name.empty()){
        if(value.size>limit || value.size%8)throw Error(87);encoded.resize(value.size);put(encoded,20,encoded.size());
    }
    return encoded;
}
inline Bytes request(std::uint32_t operation,std::span<const Identifier> identifiers,std::uint64_t undo_sequence=0){
    if(!supported(operation) || (identifiers.empty() && !empty_request(operation) && operation!=reconcile_query) || identifiers.size()>65536 ||
       (empty_request(operation) && !identifiers.empty()) ||
       ((operation==enumerate || operation==ensure_counter || operation==undo_counter) && identifiers.size()!=1) ||
       (operation==undo_counter?!undo_sequence:undo_sequence!=0))throw Error(87);
    Bytes result(header_size);put(result,0,signature,8);put(result,8,1);put(result,12,operation);put(result,20,identifiers.size());
    put(result,24,undo_sequence,8);
    for(auto value:identifiers){
        value.status=0;auto encoded=encoded_identifier(value);
        append(result,encoded);
    }
    put(result,16,result.size());return result;
}
inline std::vector<Identifier> parse_request(std::span<const std::uint8_t> bytes,std::uint32_t operation){
    if(bytes.size()<header_size || bytes.size()>limit)throw Error(87);
    Reader reader{bytes};const auto count=reader.u32(20);
    if(reader.integer(0,8)!=signature || reader.u32(8)!=1 || reader.u32(12)!=operation || reader.u32(16)!=bytes.size() ||
       (operation==undo_counter?!reader.integer(24,8):reader.integer(24,8)!=0) || (!count && !empty_request(operation) && operation!=reconcile_query) || count>65536 || !supported(operation) ||
       (empty_request(operation) && count) ||
       ((operation==enumerate || operation==ensure_counter || operation==undo_counter) && count!=1))throw Error(87);
    auto result=identifiers(bytes.subspan(header_size));if(result.size()!=count)throw Error(87);
    for(const auto& value:result)if(value.status)throw Error(87);
    return result;
}
inline void validate_instances(std::span<const std::uint8_t> bytes){
    if(bytes.size()>limit)throw Error(13);Reader reader{bytes};std::size_t count=0;
    for(std::size_t at=0;at<bytes.size();){
        if(bytes.size()-at<8)throw Error(13);const auto size=reader.u32(at);
        if(size<16 || size%8 || size>bytes.size()-at || ++count>65536)throw Error(13);
        try{(void)reader.text(at+8,at+size);}catch(const Error&){throw Error(13);}at+=size;
    }
}
class Channel {
    int descriptor_{-1};
    std::size_t input_{},capacity_{};
    std::pair<std::uint64_t,std::uint64_t> identity_{};
    void validate()const{
#if !defined(_WIN32)
        struct stat info{};const auto flags=::fcntl(descriptor_,F_GETFL);
        if(descriptor_<0 || flags<0 || (flags&O_ACCMODE)!=O_RDWR || (flags&O_APPEND) || ::fstat(descriptor_,&info) || !S_ISREG(info.st_mode) ||
           info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink || info.st_size<0 ||
           static_cast<std::uint64_t>(info.st_size)!=input_+capacity_)throw Error(13);
#else
        throw Error(50);
#endif
    }
public:
    // 成功建立 Channel 才接管 descriptor；建構失敗由呼叫端關閉。
    Channel(int descriptor,std::uint64_t input,std::uint64_t capacity):descriptor_(descriptor){
        if(input<header_size || input>limit || capacity>limit)throw Error(87);
        input_=static_cast<std::size_t>(input);capacity_=static_cast<std::size_t>(capacity);validate();
#if !defined(_WIN32)
        struct stat info{};if(::fstat(descriptor_,&info))throw Error(13);
        identity_={static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
#endif
    }
    ~Channel(){
#if !defined(_WIN32)
        if(descriptor_>=0)::close(descriptor_);
#endif
    }
    Channel(const Channel&)=delete;
    Channel& operator=(const Channel&)=delete;
    int descriptor()const{return descriptor_;}
    const std::pair<std::uint64_t,std::uint64_t>& identity()const{return identity_;}
    static std::shared_ptr<Channel> create(const std::string& session,std::span<const std::uint8_t> input,std::size_t capacity){
        if(input.size()<header_size || input.size()>limit || capacity>limit)throw Error(87);
#if !defined(_WIN32)
        struct File {int descriptor{-1};~File(){if(descriptor>=0)::close(descriptor);}};
        File directory{::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW)};struct stat info{};
        if(directory.descriptor<0 || ::fstat(directory.descriptor,&info) || !S_ISDIR(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077))throw Error(5);
        static std::atomic<std::uint64_t> ordinal{1};File file;std::string name;
        for(unsigned attempt=0;attempt<64;++attempt){
            const auto id=ordinal.fetch_add(1);if(!id || id==UINT64_MAX)throw Error(8);
            name="perflib-rpc-"+std::to_string(::getpid())+"-"+std::to_string(id);
            file.descriptor=::openat(directory.descriptor,name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
            if(file.descriptor>=0)break;if(errno!=EEXIST)throw Error(errno==EACCES?5:8);
        }
        if(file.descriptor<0)throw Error(8);
        struct Pending {
            int directory,descriptor;const std::string& name;
            ~Pending(){struct stat held{},named{};
                if(!::fstat(descriptor,&held) && !::fstatat(directory,name.c_str(),&named,AT_SYMLINK_NOFOLLOW) && held.st_dev==named.st_dev && held.st_ino==named.st_ino)
                    (void)::unlinkat(directory,name.c_str(),0);}
        } pending{directory.descriptor,file.descriptor,name};
        if(::fchmod(file.descriptor,0600) || ::unlinkat(directory.descriptor,name.c_str(),0))throw Error(5);
        if(::ftruncate(file.descriptor,static_cast<off_t>(input.size()+capacity)))throw Error(8);
        for(std::size_t at=0;at<input.size();){const auto count=::pwrite(file.descriptor,input.data()+at,input.size()-at,static_cast<off_t>(at));
            if(count<0 && errno==EINTR)continue;if(count<=0)throw Error(13);at+=static_cast<std::size_t>(count);}
        auto result=std::make_shared<Channel>(file.descriptor,input.size(),capacity);file.descriptor=-1;return result;
#else
        (void)session;throw Error(50);
#endif
    }
    Bytes read()const{
        validate();Bytes bytes(input_);
#if !defined(_WIN32)
        for(std::size_t at=0;at<bytes.size();){const auto count=::pread(descriptor_,bytes.data()+at,bytes.size()-at,static_cast<off_t>(at));
            if(count<0 && errno==EINTR)continue;if(count<=0)throw Error(13);at+=static_cast<std::size_t>(count);}
#endif
        return bytes;
    }
    Bytes read_output(std::uint64_t size)const{
        if(size>capacity_ || size>limit)throw Error(13);validate();Bytes bytes(static_cast<std::size_t>(size));
#if !defined(_WIN32)
        for(std::size_t at=0;at<bytes.size();){const auto count=::pread(descriptor_,bytes.data()+at,bytes.size()-at,static_cast<off_t>(input_+at));
            if(count<0 && errno==EINTR)continue;if(count<=0)throw Error(13);at+=static_cast<std::size_t>(count);}
#endif
        return bytes;
    }
    std::uint32_t write(std::span<const std::uint8_t> bytes)const{
        if(bytes.size()>limit)throw Error(8);validate();if(bytes.size()>capacity_)return 8;
#if !defined(_WIN32)
        for(std::size_t at=0;at<bytes.size();){const auto count=::pwrite(descriptor_,bytes.data()+at,bytes.size()-at,static_cast<off_t>(input_+at));
            if(count<0 && errno==EINTR)continue;if(count<=0)throw Error(13);at+=static_cast<std::size_t>(count);}
#endif
        return 0;
    }
};
}
