#include "shader_memory_cache.hpp"
#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <compression.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

namespace x64arm::detail {
namespace {
constexpr std::uint64_t magic_value=0x3143485334365846ULL;
constexpr std::uint32_t version_value=1;
constexpr unsigned probe_limit=64;
constexpr std::uint64_t slots_offset=64;
constexpr std::uint32_t payload_limit=64U<<20;
// 區段標頭：建立者填好其餘欄位後才以 release 寫入 magic；開啟者以 acquire 讀到 magic 後才使用。
struct Header {
    std::atomic<std::uint64_t> magic;
    std::uint32_t version,slot_bits;
    std::uint64_t capacity,data_offset;
    std::atomic<std::uint64_t> used;
    std::array<std::uint8_t,16> identity;
};
static_assert(sizeof(Header)<=slots_offset && std::atomic<std::uint64_t>::is_always_lock_free);
// 記錄：完整鍵、原始／存放長度（相等表示未壓縮）與存放內容的 FNV-1a；其後緊接存放內容，8 位元組對齊。
struct Record {ShaderCacheKey key;std::uint32_t raw_size,stored_size;std::uint64_t checksum;};
static_assert(sizeof(Record)%8==0);
struct Counters {std::atomic<std::uint64_t> lookups{},hits{},misses{},stores{},rejects{},corrupt{};};
Counters& counters() noexcept {static Counters value;return value;}
std::uint64_t fnv(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash=1469598103934665603ULL;for(const auto value:bytes){hash^=value;hash*=1099511628211ULL;}return hash;
}
std::array<std::uint8_t,16> executable_identity() noexcept {
    std::array<std::uint8_t,16> result{};
    const auto* image=reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
    if(!image || image->magic!=MH_MAGIC_64)return result;
    const auto* at=reinterpret_cast<const std::uint8_t*>(image+1);
    for(std::uint32_t n=0;n<image->ncmds;++n){
        load_command command;std::memcpy(&command,at,sizeof command);
        if(command.cmd==LC_UUID && command.cmdsize>=sizeof(uuid_command)){
            uuid_command uuid;std::memcpy(&uuid,at,sizeof uuid);std::memcpy(result.data(),uuid.uuid,result.size());return result;
        }
        if(command.cmdsize<sizeof command)break;
        at+=command.cmdsize;
    }
    return result;
}
void trace() noexcept {
    static const bool enabled=std::getenv("X64ARM_SHADER_MEMORY_CACHE_TRACE")!=nullptr;
    auto& c=counters();
    if(!enabled || c.lookups.load(std::memory_order_relaxed)%256)return;
    std::fprintf(stderr,"[shader_memory_cache] pid=%d lookups=%llu hits=%llu misses=%llu stores=%llu rejects=%llu corrupt=%llu\n",static_cast<int>(::getpid()),
        static_cast<unsigned long long>(c.lookups.load()),static_cast<unsigned long long>(c.hits.load()),static_cast<unsigned long long>(c.misses.load()),
        static_cast<unsigned long long>(c.stores.load()),static_cast<unsigned long long>(c.rejects.load()),static_cast<unsigned long long>(c.corrupt.load()));
}
class Segment {
    std::uint8_t* base_{};std::uint64_t capacity_{},data_offset_{};std::uint32_t slot_bits_{};
    Header& header() const noexcept {return *reinterpret_cast<Header*>(base_);}
    std::atomic<std::uint64_t>* slots() const noexcept {return reinterpret_cast<std::atomic<std::uint64_t>*>(base_+slots_offset);}
    bool valid_offset(std::uint64_t offset) const noexcept {
        return offset>=data_offset_ && offset%8==0 && offset<=capacity_-sizeof(Record);
    }
    Record record(std::uint64_t offset) const noexcept {Record value;std::memcpy(&value,base_+offset,sizeof value);return value;}
public:
    bool open(){
        const auto identity=executable_identity();
        if(std::all_of(identity.begin(),identity.end(),[](auto value){return value==0;}))return false; // 無法辨識引擎身分時不共用。
        const auto* text=std::getenv("X64ARM_SHADER_MEMORY_CACHE_MB");
        const auto megabytes=std::clamp<std::uint64_t>(text?std::strtoull(text,nullptr,10):256,16,2048);
        capacity_=megabytes<<20;
        // 約每 4 KiB 一個索引槽（256 MiB 為 65,536 槽、512 KiB）；記錄平均遠大於 4 KiB 時負載很低。
        slot_bits_=static_cast<std::uint32_t>(std::bit_width(capacity_/4096)-1);
        data_offset_=(slots_offset+(std::uint64_t{1}<<slot_bits_)*8+4095)&~std::uint64_t{4095};
        // 名稱只含格式版本與容量；引擎身分寫在標頭，不符時解除舊名稱重建，同一設定最多保留一個區段名稱。
        std::uint8_t material[12];std::memcpy(material,&version_value,4);std::memcpy(material+4,&capacity_,8);
        std::array<std::uint8_t,CC_SHA256_DIGEST_LENGTH> digest{};CC_SHA256(material,sizeof material,digest.data());
        std::string name="/x64sh";
        for(unsigned n=0;n<10;++n){static constexpr char hex[]="0123456789abcdef";name.push_back(hex[digest[n]>>4]);name.push_back(hex[digest[n]&15]);}
        for(unsigned attempt=0;attempt<2;++attempt){
            if(const int fd=::shm_open(name.c_str(),O_RDWR|O_CREAT|O_EXCL,0600);fd>=0){
                if(::ftruncate(fd,static_cast<off_t>(capacity_))!=0){::close(fd);::shm_unlink(name.c_str());return false;}
                void* mapped=::mmap(nullptr,capacity_,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);::close(fd);
                if(mapped==MAP_FAILED){::shm_unlink(name.c_str());return false;}
                base_=static_cast<std::uint8_t*>(mapped);auto& h=header();
                h.version=version_value;h.slot_bits=slot_bits_;h.capacity=capacity_;h.data_offset=data_offset_;
                h.used.store(data_offset_,std::memory_order_relaxed);h.identity=identity;
                h.magic.store(magic_value,std::memory_order_release);
                return true;
            }
            if(errno!=EEXIST)return false;
            const int fd=::shm_open(name.c_str(),O_RDWR,0600);
            if(fd<0)return false;
            struct stat status{};
            if(::fstat(fd,&status)!=0 || static_cast<std::uint64_t>(status.st_size)<capacity_){::close(fd);::shm_unlink(name.c_str());continue;}
            void* mapped=::mmap(nullptr,capacity_,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);::close(fd);
            if(mapped==MAP_FAILED)return false;
            base_=static_cast<std::uint8_t*>(mapped);auto& h=header();
            // 建立者在 ftruncate 與寫入 magic 之間：最多等 200 ms；逾時視為建立者已中止。
            for(unsigned wait=0;wait<200 && h.magic.load(std::memory_order_acquire)!=magic_value;++wait)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if(h.magic.load(std::memory_order_acquire)==magic_value && h.version==version_value && h.slot_bits==slot_bits_ &&
               h.capacity==capacity_ && h.data_offset==data_offset_ && h.identity==identity)return true;
            // 其他引擎版本或未完成的區段：解除名稱後重建；已映射的程序保留原內容，最後一個解除映射時釋放。
            ::munmap(mapped,capacity_);base_=nullptr;::shm_unlink(name.c_str());
        }
        return false;
    }
    std::optional<std::vector<std::uint8_t>> find(const ShaderCacheKey& key) const {
        std::uint64_t hash;std::memcpy(&hash,key.data(),8);
        const auto mask=(std::uint64_t{1}<<slot_bits_)-1;
        for(unsigned probe=0;probe<probe_limit;++probe){
            const auto offset=slots()[(hash+probe)&mask].load(std::memory_order_acquire);
            if(!offset)return std::nullopt;
            if(!valid_offset(offset)){++counters().corrupt;return std::nullopt;}
            const auto found=record(offset);
            if(found.key!=key)continue;
            if(found.raw_size>payload_limit || found.stored_size>found.raw_size || found.stored_size>capacity_-offset-sizeof(Record)){++counters().corrupt;return std::nullopt;}
            const std::span<const std::uint8_t> stored(base_+offset+sizeof(Record),found.stored_size);
            if(fnv(stored)!=found.checksum){++counters().corrupt;return std::nullopt;}
            std::vector<std::uint8_t> result(found.raw_size);
            if(found.stored_size==found.raw_size)std::memcpy(result.data(),stored.data(),stored.size());
            else if(compression_decode_buffer(result.data(),result.size(),stored.data(),stored.size(),nullptr,COMPRESSION_LZ4)!=result.size()){
                ++counters().corrupt;return std::nullopt;
            }
            return result;
        }
        return std::nullopt;
    }
    void store(const ShaderCacheKey& key,std::span<const std::uint8_t> payload) const {
        if(payload.empty() || payload.size()>payload_limit){++counters().rejects;return;}
        // 壓不小（或壓縮失敗）就存原始內容；以存放長度等於原始長度表示。
        std::vector<std::uint8_t> compressed(payload.size());
        auto stored_size=compression_encode_buffer(compressed.data(),compressed.size(),payload.data(),payload.size(),nullptr,COMPRESSION_LZ4);
        const bool raw=!stored_size || stored_size>=payload.size();
        const std::span<const std::uint8_t> stored=raw?payload:std::span<const std::uint8_t>(compressed.data(),stored_size);
        const auto need=(sizeof(Record)+stored.size()+7)&~std::size_t{7};
        auto& h=header();
        if(need>capacity_-data_offset_ || h.used.load(std::memory_order_relaxed)>capacity_-need){++counters().rejects;return;}
        const auto offset=h.used.fetch_add(need,std::memory_order_relaxed);
        if(offset>capacity_-need){++counters().rejects;return;}
        const Record value{key,static_cast<std::uint32_t>(payload.size()),static_cast<std::uint32_t>(stored.size()),fnv(stored)};
        std::memcpy(base_+offset,&value,sizeof value);std::memcpy(base_+offset+sizeof value,stored.data(),stored.size());
        // 內容完整寫入後才以 release 發布索引；同一鍵已有記錄（其他程序同時翻譯）時保留先發布者。
        std::uint64_t hash;std::memcpy(&hash,key.data(),8);
        const auto mask=(std::uint64_t{1}<<slot_bits_)-1;
        for(unsigned probe=0;probe<probe_limit;++probe){
            auto& slot=slots()[(hash+probe)&mask];std::uint64_t expected=0;
            if(slot.compare_exchange_strong(expected,offset,std::memory_order_release,std::memory_order_acquire)){++counters().stores;return;}
            if(valid_offset(expected) && record(expected).key==key)return;
        }
        ++counters().rejects;
    }
};
const Segment* segment() noexcept {
    static const Segment* value=[]()->const Segment* {
        try{auto* created=new Segment;if(created->open())return created;delete created;}catch(...){}
        return nullptr;
    }();
    return value;
}
}
bool shader_memory_cache_enabled() noexcept {
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_SHADER_MEMORY_CACHE");return !value || std::string_view(value)!="0";}();
    return enabled;
}
ShaderCacheKey shader_cache_key(std::uint32_t stage,std::span<const std::uint8_t> bytecode) noexcept {
    CC_SHA256_CTX context;CC_SHA256_Init(&context);
    CC_SHA256_Update(&context,&stage,sizeof stage);
    CC_SHA256_Update(&context,bytecode.data(),static_cast<CC_LONG>(bytecode.size()));
    ShaderCacheKey key{};CC_SHA256_Final(key.data(),&context);
    return key;
}
std::optional<std::vector<std::uint8_t>> shader_memory_cache_find(const ShaderCacheKey& key) {
    const auto* cache=segment();if(!cache)return std::nullopt;
    ++counters().lookups;
    auto result=cache->find(key);
    if(result)++counters().hits;else ++counters().misses;
    trace();
    return result;
}
void shader_memory_cache_store(const ShaderCacheKey& key,std::span<const std::uint8_t> payload) noexcept {
    const auto* cache=segment();if(!cache)return;
    try{cache->store(key,payload);}catch(...){++counters().rejects;}
}
}
#else
namespace x64arm::detail {
// 其他平台沒有共享記憶體快取：一律未命中，翻譯照常進行。
bool shader_memory_cache_enabled() noexcept {return false;}
ShaderCacheKey shader_cache_key(std::uint32_t,std::span<const std::uint8_t>) noexcept {return {};}
std::optional<std::vector<std::uint8_t>> shader_memory_cache_find(const ShaderCacheKey&) {return std::nullopt;}
void shader_memory_cache_store(const ShaderCacheKey&,std::span<const std::uint8_t>) noexcept {}
}
#endif
