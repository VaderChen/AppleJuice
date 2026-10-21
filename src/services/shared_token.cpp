#include "x64arm/shared_token.hpp"
#include "x64arm/security_descriptor.hpp"
#include <cerrno>
#include <atomic>
#include <filesystem>
#include <map>
#include <limits>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace x64arm {
namespace {
constexpr std::size_t token_limit=262144;
constexpr std::uint64_t signature=UINT64_C(0x32544b5741523458);
struct Header {std::uint64_t magic{signature},generation{},length{},reserved{};};
constexpr std::size_t storage_size=sizeof(Header)+token_limit;
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
std::mutex registry_mutex;
std::map<std::pair<std::uint64_t,std::uint64_t>,std::weak_ptr<SharedToken>> registry;
#if !defined(_WIN32)
void record_lock(int descriptor,short type){
    struct flock lock{};lock.l_type=type;lock.l_whence=SEEK_SET;
    while(::fcntl(descriptor,F_SETLKW,&lock)<0){if(errno==EINTR)continue;throw SecurityDescriptorError{13};}
}
void transfer_bytes(int fd,std::vector<std::uint8_t>& bytes,bool writing){
    std::size_t at=0;while(at<bytes.size()){
        const auto count=writing?::pwrite(fd,bytes.data()+at,bytes.size()-at,static_cast<off_t>(sizeof(Header)+at)) : ::pread(fd,bytes.data()+at,bytes.size()-at,static_cast<off_t>(sizeof(Header)+at));
        if(count<0 && errno==EINTR)continue;if(count<=0)throw SecurityDescriptorError{writing?8U:13U};at+=static_cast<std::size_t>(count);
    }
}
#endif
}
SharedToken::~SharedToken(){
#if !defined(_WIN32)
    if(mapped_)::munmap(mapped_,sizeof(Header));
    if(descriptor_>=0)::close(descriptor_);
#endif
}
void SharedToken::map_storage(){
#if !defined(_WIN32)
    mapped_=::mmap(nullptr,sizeof(Header),PROT_READ|PROT_WRITE,MAP_SHARED,descriptor_,0);
    if(mapped_==MAP_FAILED){mapped_=nullptr;throw SecurityDescriptorError{8};}
#else
    throw SecurityDescriptorError{50};
#endif
}
void SharedToken::validate_storage()const{
#if !defined(_WIN32)
    // 仍檢查失效／截短的物件；快取不依賴可能相同的檔案時間戳。
    struct stat info{};if(::fstat(descriptor_,&info) || info.st_size!=static_cast<off_t>(storage_size) || !mapped_)throw SecurityDescriptorError{13};
    const auto& header=*static_cast<const Header*>(mapped_);
    if(header.magic!=signature || header.reserved)throw SecurityDescriptorError{13};
#else
    throw SecurityDescriptorError{50};
#endif
}
SharedToken::Transaction::Transaction(SharedToken& token):token_(token),lock_(token.mutex_){
#if !defined(_WIN32)
    // POSIX 記錄鎖屬於程序；dup／SCM_RIGHTS 不會共用 flock 的鎖擁有者。
    if(!token_.lock_depth_)record_lock(token_.descriptor_,F_WRLCK);++token_.lock_depth_;
#else
    throw SecurityDescriptorError{50};
#endif
}
SharedToken::Transaction::~Transaction(){
#if !defined(_WIN32)
    if(!--token_.lock_depth_){try{record_lock(token_.descriptor_,F_UNLCK);}catch(const SecurityDescriptorError&){}}
#endif
}
VirtualToken SharedToken::Transaction::read()const{
#if !defined(_WIN32)
    token_.validate_storage();auto& header=*static_cast<Header*>(token_.mapped_);
    const auto generation=std::atomic_ref(header.generation).load(std::memory_order_acquire);
    if(!generation || (generation&1) || !header.length || header.length>token_limit)throw SecurityDescriptorError{13};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(header.length));transfer_bytes(token_.descriptor_,bytes,false);
    auto value=decode_virtual_token(bytes);if(!value)throw SecurityDescriptorError{13};return std::move(*value);
#else
    throw SecurityDescriptorError{50};
#endif
}
void SharedToken::Transaction::write(const VirtualToken& token){
#if !defined(_WIN32)
    auto bytes=encode_virtual_token(token);if(bytes.empty() || bytes.size()>token_limit)throw SecurityDescriptorError{8};
    token_.validate_storage();auto& header=*static_cast<Header*>(token_.mapped_);auto generation=std::atomic_ref(header.generation);
    const auto before=generation.load(std::memory_order_acquire);
    if((before&1) || before>std::numeric_limits<std::uint64_t>::max()-2)throw SecurityDescriptorError{13};
    // 奇數表示尚未提交，失敗或寫入程序中斷時不得沿用舊的權限快照。
    generation.store(before+1,std::memory_order_seq_cst);
    transfer_bytes(token_.descriptor_,bytes,true);header.length=bytes.size();
    generation.store(before+2,std::memory_order_release);
#else
    (void)token;throw SecurityDescriptorError{50};
#endif
}
VirtualToken SharedToken::read(){Transaction transaction(*this);return transaction.read();}
bool SharedToken::refresh(VirtualToken& value,std::uint64_t& revision){
#if !defined(_WIN32)
    // 每次客體 API 都會檢查；未變更時只讀映射的 generation。儲存檔建立後即固定大小、已 unlink，
    // 只經引擎程序間傳遞且 create／inherit 已核對大小；有新版本時 Transaction::read 仍完整驗證。
    std::lock_guard local_lock(mutex_);if(!mapped_)throw SecurityDescriptorError{13};auto& header=*static_cast<Header*>(mapped_);
    if(header.magic!=signature || header.reserved)throw SecurityDescriptorError{13};
    const auto generation=std::atomic_ref(header.generation).load(std::memory_order_acquire);
    if(generation && !(generation&1) && generation==revision)return false;
    Transaction transaction(*this);auto next=transaction.read();
    value=std::move(next);revision=std::atomic_ref(header.generation).load(std::memory_order_acquire);return true;
#else
    (void)value;(void)revision;throw SecurityDescriptorError{50};
#endif
}
std::shared_ptr<SharedToken> SharedToken::create(const VirtualToken& token){
#if !defined(_WIN32)
    auto result=std::make_shared<SharedToken>();auto pattern=(std::filesystem::temp_directory_path()/"x64arm-token-XXXXXX").string();std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
    result->descriptor_=::mkstemp(name.data());if(result->descriptor_<0)throw SecurityDescriptorError{8};(void)::unlink(name.data());
    if(::fcntl(result->descriptor_,F_SETFD,FD_CLOEXEC) || ::fchmod(result->descriptor_,0600))throw SecurityDescriptorError{5};
    if(::ftruncate(result->descriptor_,static_cast<off_t>(storage_size)))throw SecurityDescriptorError{8};
    result->map_storage();std::construct_at(static_cast<Header*>(result->mapped_));
    struct stat info{};if(::fstat(result->descriptor_,&info))throw SecurityDescriptorError{13};result->identity_={static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
    Transaction transaction(*result);transaction.write(token);
    {std::lock_guard lock(registry_mutex);std::erase_if(registry,[](const auto& entry){return entry.second.expired();});registry[result->identity_]=result;}return result;
#else
    (void)token;throw SecurityDescriptorError{50};
#endif
}
std::shared_ptr<SharedToken> SharedToken::inherit(int descriptor){
#if !defined(_WIN32)
    struct stat info{};if(::fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink || info.st_size!=static_cast<off_t>(storage_size) || (::fcntl(descriptor,F_GETFL)&O_ACCMODE)!=O_RDWR)throw SecurityDescriptorError{13};
    std::lock_guard registry_lock(registry_mutex);const std::pair<std::uint64_t,std::uint64_t> identity{static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};
    if(const auto existing=registry[identity].lock())return existing;
    auto result=std::make_shared<SharedToken>();result->descriptor_=::fcntl(descriptor,F_DUPFD_CLOEXEC,0);if(result->descriptor_<0)throw SecurityDescriptorError{8};
    result->identity_={static_cast<std::uint64_t>(info.st_dev),static_cast<std::uint64_t>(info.st_ino)};result->map_storage();(void)result->read();registry[identity]=result;return result;
#else
    (void)descriptor;throw SecurityDescriptorError{50};
#endif
}
}
