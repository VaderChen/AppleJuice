#include "x64arm/resource_cache.hpp"
#include "x64arm/retained_capacity.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <compression.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <map>
#include <limits>
#include <set>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace x64arm {
namespace {
using Bytes=std::vector<std::uint8_t>;
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
struct ResourceCounters {
    std::atomic<std::uint64_t> stores{},entries{},retained_bytes{};
    std::atomic<std::uint64_t> payload_reserved_bytes{},peak_payload_reserved_bytes{},budget_rejections{},store_rejections{},store_evictions{},store_slots{};
    std::atomic<std::uint64_t> live_payloads{},live_payload_bytes{},peak_live_payload_bytes{};
    std::atomic<std::uint64_t> writer_held_bytes{},peak_writer_held_bytes{},active_loads{};
    std::atomic<std::uint64_t> requests{},memory_hits{},disk_hits{},build_calls{},build_failures{},wait_calls{},evictions{},bypasses{};
    std::atomic<std::uint64_t> build_ns{},read_ns{},decode_ns{},encode_ns{},write_ns{},write_failures{};
} counters;
constexpr std::uint64_t maximum_retained_bytes=256U*1024U*1024U;
constexpr std::size_t maximum_stores=16,maximum_index_entries=1024;
struct ResourcePolicy {std::uint64_t bytes;std::size_t stores;};
const ResourcePolicy& policy(){
    // 只在程序首次使用時讀取；環境選項僅能縮小內建上限，不能在使用中放大額度。
    static const ResourcePolicy value=[] {
        return ResourcePolicy{retained_capacity_setting("X64ARM_RESOURCE_CACHE_TOTAL_BYTES",maximum_retained_bytes),static_cast<std::size_t>(retained_capacity_setting("X64ARM_RESOURCE_CACHE_MAX_STORES",maximum_stores))};
    }();return value;
}
void add_live(std::atomic<std::uint64_t>& live,std::atomic<std::uint64_t>& peak,std::uint64_t bytes){
    const auto current=live.fetch_add(bytes,std::memory_order_relaxed)+bytes;
    auto previous=peak.load(std::memory_order_relaxed);while(current>previous && !peak.compare_exchange_weak(previous,current,std::memory_order_relaxed)){}
}
void add_time(std::atomic<std::uint64_t>& counter,double milliseconds){counter.fetch_add(static_cast<std::uint64_t>(milliseconds*1000000),std::memory_order_relaxed);}
bool reserve_payload(std::uint64_t bytes){
    return reserve_retained_capacity(bytes,policy().bytes,counters.payload_reserved_bytes,
        counters.peak_payload_reserved_bytes,counters.budget_rejections);
}
template<class T> struct RetainedAllocator {
    using value_type=T;
    RetainedAllocator()=default;
    template<class U> RetainedAllocator(const RetainedAllocator<U>&)noexcept{}
    T* allocate(std::size_t count){
        if(count>std::numeric_limits<std::size_t>::max()/sizeof(T))throw std::bad_array_new_length{};
        const auto bytes=count*sizeof(T);if(!reserve_payload(bytes))throw std::bad_alloc{};
        T* result{};try{result=std::allocator<T>{}.allocate(count);}catch(...){counters.payload_reserved_bytes.fetch_sub(bytes,std::memory_order_relaxed);throw;}
        add_live(counters.live_payload_bytes,counters.peak_live_payload_bytes,bytes);return result;
    }
    void deallocate(T* value,std::size_t count)noexcept{
        const auto bytes=count*sizeof(T);std::allocator<T>{}.deallocate(value,count);
        counters.live_payload_bytes.fetch_sub(bytes,std::memory_order_relaxed);counters.payload_reserved_bytes.fetch_sub(bytes,std::memory_order_relaxed);
    }
    template<class U> bool operator==(const RetainedAllocator<U>&)const noexcept{return true;}
};
using RetainedBytes=std::vector<std::uint8_t,RetainedAllocator<std::uint8_t>>;
std::shared_ptr<RetainedBytes> retained_payload(const Bytes& bytes){
    auto* payload=new RetainedBytes(bytes.begin(),bytes.end());
    counters.live_payloads.fetch_add(1,std::memory_order_relaxed);
    // shared_ptr 控制區配置失敗時也會呼叫 deleter；最後持有者才歸還計帳。
    return std::shared_ptr<RetainedBytes>(payload,[](RetainedBytes* value){delete value;counters.live_payloads.fetch_sub(1,std::memory_order_relaxed);});
}
void create_resource(ResourceCache::Result& result,const std::function<Bytes()>& create){
    counters.build_calls.fetch_add(1,std::memory_order_relaxed);const auto start=Clock::now();
    try{result.bytes=create();}catch(...){add_time(counters.build_ns,elapsed(start));counters.build_failures.fetch_add(1,std::memory_order_relaxed);throw;}
    result.build_ms=elapsed(start);add_time(counters.build_ns,result.build_ms);
}
constexpr std::size_t header_size=96,maximum_resource=256U*1024U*1024U;
// 背景預載尚未完成時程序結束，讓預載迴圈提早放棄，不讓短命程序等完整讀取。
std::atomic<bool> preload_cancelled{false};
constexpr std::array<std::uint8_t,8> magic{'X','6','4','R','S','C',2,0};
struct Fd {int value{-1};~Fd(){if(value>=0)::close(value);}explicit operator bool()const{return value>=0;}};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
Bytes digest(std::span<const std::uint8_t> bytes){CC_SHA256_CTX hash;CC_SHA256_Init(&hash);
    while(!bytes.empty()){const auto size=std::min<std::size_t>(bytes.size(),UINT32_MAX);CC_SHA256_Update(&hash,bytes.data(),static_cast<CC_LONG>(size));bytes=bytes.subspan(size);}
    Bytes result(32);CC_SHA256_Final(result.data(),&hash);return result;}
#pragma clang diagnostic pop
std::string hex(std::span<const std::uint8_t> bytes){std::string result;for(const auto c:bytes){result.push_back("0123456789abcdef"[c>>4]);result.push_back("0123456789abcdef"[c&15]);}return result;}
bool safe(int fd,bool directory=false){struct stat st{};return ::fstat(fd,&st)==0 && (directory?S_ISDIR(st.st_mode):S_ISREG(st.st_mode)) && st.st_uid==::getuid() && !(st.st_mode&077);}
bool lock_file(int fd,int mode){int result;do{result=::flock(fd,mode);}while(result<0 && errno==EINTR);return result==0;}
bool transfer(int fd,std::span<std::uint8_t> bytes,bool writing){
    while(!bytes.empty()){
        const auto count=writing ? ::write(fd,bytes.data(),bytes.size()) : ::read(fd,bytes.data(),bytes.size());
        if(count<0 && errno==EINTR)continue;if(count<=0)return false;bytes=bytes.subspan(static_cast<std::size_t>(count));
    }return true;
}
bool write_bytes(int fd,std::span<const std::uint8_t> bytes){
    while(!bytes.empty()){const auto count=::write(fd,bytes.data(),bytes.size());if(count<0 && errno==EINTR)continue;if(count<=0)return false;bytes=bytes.subspan(static_cast<std::size_t>(count));}return true;
}
bool resource_name(std::string_view name){return name.size()==68 && name.ends_with(".bin") && std::all_of(name.begin(),name.begin()+64,[](char c){return (c>='0' && c<='9') || (c>='a' && c<='f');});}
Bytes read_resource(int directory,const std::string& name,const Bytes& key,std::uint64_t limit,ResourceCache::Result& result){
    const auto started=Clock::now();
    Fd file{::openat(directory,name.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC)};struct stat st{};
    if(!file || !safe(file.value) || ::fstat(file.value,&st) || st.st_size<static_cast<off_t>(header_size) || static_cast<std::uint64_t>(st.st_size)>limit)return {};
    std::array<std::uint8_t,header_size> header{};if(!transfer(file.value,header,false) || !std::equal(magic.begin(),magic.end(),header.begin()) || !std::equal(key.begin(),key.end(),header.begin()+32))return {};
    std::uint64_t size=0,stored=0;for(unsigned i=0;i<8;++i){size|=std::uint64_t{header[8+i]}<<(i*8);stored|=std::uint64_t{header[16+i]}<<(i*8);}
    const auto codec=header[24];if(!size || size>maximum_resource || size>limit || !stored || stored!=static_cast<std::uint64_t>(st.st_size)-header_size || codec>1 ||
        std::any_of(header.begin()+25,header.begin()+32,[](auto byte){return byte!=0;}) || (!codec && stored!=size))return {};
    Bytes packed(static_cast<std::size_t>(stored));if(!transfer(file.value,packed,false))return {};result.read_ms=elapsed(started);result.stored_bytes=stored+header_size;
    const auto decoding=Clock::now();Bytes bytes;
    // decode_buffer 在輸出不足時也回 dst_size；多留一 byte 才能拒絕截斷的解壓。
    if(codec){bytes.resize(static_cast<std::size_t>(size)+1);if(compression_decode_buffer(bytes.data(),bytes.size(),packed.data(),packed.size(),nullptr,COMPRESSION_LZ4)!=size)return {};bytes.resize(static_cast<std::size_t>(size));}
    else bytes=std::move(packed);
    const auto hash=digest(bytes);if(!std::equal(hash.begin(),hash.end(),header.begin()+64))return {};result.decode_ms=elapsed(decoding);
    return bytes; // 命中只更新記憶體 LRU；磁碟時間戳由批次更新。
}
bool publish(int directory,const std::string& name,const Bytes& key,std::span<const std::uint8_t> bytes,std::uint64_t limit,std::size_t capacity,bool compress,ResourceCache::Result& result){
    if(bytes.empty() || bytes.size()>maximum_resource || !capacity)return false;
    const auto encoding=Clock::now();Bytes packed;unsigned codec=0;
    if(compress && bytes.size()>=4096){packed.resize(bytes.size());const auto size=compression_encode_buffer(packed.data(),packed.size(),bytes.data(),bytes.size(),nullptr,COMPRESSION_LZ4);
        if(size && size+64<bytes.size()){packed.resize(size);codec=1;}else packed.clear();}
    const std::span<const std::uint8_t> payload=codec?std::span<const std::uint8_t>(packed):bytes;result.encode_ms=elapsed(encoding);
    if(payload.size()>limit-header_size)return false;
    const auto writing=Clock::now();
    Fd lock{::openat(directory,"index.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600)};
    if(!lock || !safe(lock.value) || !lock_file(lock.value,LOCK_EX))return false;
    ResourceCache::Result existing;const auto previous=read_resource(directory,name,key,limit,existing);
    if(std::equal(previous.begin(),previous.end(),bytes.begin(),bytes.end())){result.stored_bytes=existing.stored_bytes;result.write_ms=elapsed(writing);return true;}
    struct Entry{std::string name;std::uint64_t size;timespec modified;};std::vector<Entry> entries;std::uint64_t total=0;
    const auto copy=::openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(copy<0)return false;
    auto* stream=::fdopendir(copy);if(!stream){::close(copy);return false;}
    while(const auto* item=::readdir(stream)){
        const std::string_view current(item->d_name);
        // 所有寫入者持有 index.lock；此時暫存檔只可能來自中斷的發布。
        if(current.starts_with(".pending-")){(void)::unlinkat(directory,item->d_name,0);continue;}
        if(!resource_name(current))continue;
        struct stat st{};if(::fstatat(directory,item->d_name,&st,AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_size<0)continue;
        if(current==name){if(::unlinkat(directory,item->d_name,0)){::closedir(stream);return false;}continue;}
        const auto size=static_cast<std::uint64_t>(st.st_size);
        if(entries.size()>=maximum_index_entries || size>std::numeric_limits<std::uint64_t>::max()-total){::closedir(stream);return false;}
        total+=size;entries.push_back({std::string(current),size,st.st_mtimespec});
    }
    ::closedir(stream);
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.modified.tv_sec!=b.modified.tv_sec?a.modified.tv_sec<b.modified.tv_sec:a.modified.tv_nsec<b.modified.tv_nsec;});
    const auto required=payload.size()+header_size;std::size_t removed=0;
    while(total>limit-required || entries.size()-removed>=capacity){
        if(removed==entries.size())return false;const auto& entry=entries[removed++];
        if(::unlinkat(directory,entry.name.c_str(),0) && errno!=ENOENT)return false;total-=entry.size;
    }
    const auto temporary=".pending-"+std::to_string(::getpid());
    Fd file{::openat(directory,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600)};if(!file)return false;
    struct Cleanup{int dir;const std::string& name;~Cleanup(){::unlinkat(dir,name.c_str(),0);}} cleanup{directory,temporary};
    std::array<std::uint8_t,header_size> header{};std::copy(magic.begin(),magic.end(),header.begin());
    for(unsigned i=0;i<8;++i){header[8+i]=static_cast<std::uint8_t>(bytes.size()>>(i*8));header[16+i]=static_cast<std::uint8_t>(payload.size()>>(i*8));}
    header[24]=static_cast<std::uint8_t>(codec);
    std::copy(key.begin(),key.end(),header.begin()+32);const auto hash=digest(bytes);std::copy(hash.begin(),hash.end(),header.begin()+64);
    if(!write_bytes(file.value,header) || !write_bytes(file.value,payload))return false;
    const bool stored=::renameat(directory,temporary.c_str(),directory,name.c_str())==0;
    result.write_ms=elapsed(writing);if(stored)result.stored_bytes=required;return stored;
}
bool tracing(){const auto* value=std::getenv("X64ARM_RESOURCE_CACHE_TRACE");return value && std::string_view(value)=="1";}
Bytes resource_key(std::string_view name){Bytes key(32);const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};for(unsigned i=0;i<32;++i)key[i]=static_cast<std::uint8_t>((digit(name[2*i])<<4)|digit(name[2*i+1]));return key;}
class MemoryResources {
    struct Entry {
        std::shared_ptr<RetainedBytes> bytes;Bytes key;
        std::uint64_t use{},stored_bytes{};bool dirty{},touched{},compress{};
    };
    Fd directory_;std::uint64_t limit_,used_{},clock_{};std::size_t capacity_;
    std::map<std::string,Entry> entries_;std::set<std::string> building_;std::mutex mutex_;std::condition_variable ready_,finished_;std::thread writer_;
    bool stop_{},forced_{},writing_{};Clock::time_point first_dirty_{},last_dirty_{},last_flush_{Clock::now()};
    struct Accounting {
        MemoryResources& owner;
        explicit Accounting(MemoryResources& value):owner(value){counters.stores.fetch_add(1,std::memory_order_relaxed);}
        ~Accounting(){counters.stores.fetch_sub(1,std::memory_order_relaxed);counters.entries.fetch_sub(owner.entries_.size(),std::memory_order_relaxed);counters.retained_bytes.fetch_sub(owner.used_,std::memory_order_relaxed);}
    } accounting_{*this};
    void added(std::uint64_t bytes){used_+=bytes;counters.retained_bytes.fetch_add(bytes,std::memory_order_relaxed);counters.entries.fetch_add(1,std::memory_order_relaxed);}
    bool dirty()const{return std::any_of(entries_.begin(),entries_.end(),[](const auto& item){return item.second.dirty;});}
    bool touched()const{return std::any_of(entries_.begin(),entries_.end(),[](const auto& item){return item.second.touched;});}
    bool evict_clean(){
            auto old=entries_.end();for(auto it=entries_.begin();it!=entries_.end();++it)if(!it->second.dirty && (old==entries_.end() || it->second.use<old->second.use))old=it;
            if(old==entries_.end())return false;const auto bytes=old->second.bytes->size();used_-=bytes;counters.retained_bytes.fetch_sub(bytes,std::memory_order_relaxed);counters.entries.fetch_sub(1,std::memory_order_relaxed);counters.evictions.fetch_add(1,std::memory_order_relaxed);entries_.erase(old);return true;
    }
    void trim(std::uint64_t incoming){
        while((used_>limit_-incoming || entries_.size()>=capacity_) && evict_clean()){}
    }
    void write_loop(){
        std::unique_lock lock(mutex_);
        for(;;){
            if(stop_ && !dirty() && !touched())return;
            if(!forced_ && !stop_){
                if(!dirty() && !touched()){ready_.wait(lock);continue;}
                // 新產物安靜 2 秒後合併寫入，持續產生時最多等 10 秒；命中時間戳每 30 秒合併。
                const auto deadline=dirty()?std::min(last_dirty_+std::chrono::seconds(2),first_dirty_+std::chrono::seconds(10)):last_flush_+std::chrono::seconds(30);
                if(Clock::now()<deadline){ready_.wait_until(lock,deadline);continue;}
            }
            struct WriterHold {
                std::uint64_t bytes{};
                void add(std::uint64_t size){bytes+=size;add_live(counters.writer_held_bytes,counters.peak_writer_held_bytes,size);}
                ~WriterHold(){counters.writer_held_bytes.fetch_sub(bytes,std::memory_order_relaxed);}
            } held;
            std::vector<std::pair<std::string,Entry>> batch;
            for(auto& [name,entry]:entries_)if(entry.dirty || entry.touched){batch.emplace_back(name,entry);held.add(entry.bytes->capacity());entry.touched=false;}
            std::stable_sort(batch.begin(),batch.end(),[](const auto& a,const auto& b){return a.second.dirty!=b.second.dirty ? !a.second.dirty : a.second.use<b.second.use;});
            // 寫入期間到達的新產物屬於下一批，不能沿用已到期的舊期限而逐筆寫。
            first_dirty_=Clock::now();forced_=false;writing_=true;lock.unlock();
            for(auto& [name,entry]:batch){
                ResourceCache::Result timing;bool stored=false;
                if(entry.dirty){try{stored=publish(directory_.value,name,entry.key,*entry.bytes,limit_,capacity_,entry.compress,timing);}catch(const std::bad_alloc&){}
                    add_time(counters.encode_ns,timing.encode_ms);add_time(counters.write_ns,timing.write_ms);if(!stored)counters.write_failures.fetch_add(1,std::memory_order_relaxed);
                    if(tracing())std::fprintf(stderr,"resource_cache_write pid=%d key=%s stored=%u bytes=%zu stored_bytes=%llu encode_ms=%.3f write_ms=%.3f\n",::getpid(),name.c_str(),stored,entry.bytes->size(),static_cast<unsigned long long>(timing.stored_bytes),timing.encode_ms,timing.write_ms);
                }else{Fd file{::openat(directory_.value,name.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC)};if(file && safe(file.value))(void)::futimens(file.value,nullptr);}
                lock.lock();if(auto found=entries_.find(name);found!=entries_.end() && found->second.bytes==entry.bytes){
                    if(entry.dirty){found->second.dirty=false;found->second.stored_bytes=stored?timing.stored_bytes:0;}
                }lock.unlock();
            }
            lock.lock();writing_=false;last_flush_=Clock::now();if(!dirty())first_dirty_={};finished_.notify_all();
        }
    }
public:
    MemoryResources(const std::filesystem::path& path,std::uint64_t limit,std::size_t capacity):limit_(limit),capacity_(capacity){
        std::error_code error;if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path(),error);
        if(error || (::mkdir(path.c_str(),0700) && errno!=EEXIST))return;
        directory_.value=::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(!directory_ || !safe(directory_.value,true))return;
        const auto started=Clock::now();double reads=0,decodes=0;std::uint64_t disk_bytes=0;
        static const bool eager=[] {const auto* value=std::getenv("X64ARM_RESOURCE_CACHE_PRELOAD");return value && std::string_view(value)=="1";}();
        struct File{std::string name;timespec modified;};std::vector<File> files;
        const auto copy=eager?::openat(directory_.value,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC):-1;auto* stream=copy<0 ? nullptr : ::fdopendir(copy);
        if(stream){while(const auto* item=::readdir(stream))if(resource_name(item->d_name)){
            struct stat st{};if(!::fstatat(directory_.value,item->d_name,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode)){
                // 舊目錄超過索引上限時退回逐項讀取，不為可選預載建立無界清單。
                if(files.size()>=maximum_index_entries){files.clear();break;}
                files.push_back({item->d_name,st.st_mtimespec});}}
            ::closedir(stream);
        }else if(copy>=0)::close(copy);
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.modified.tv_sec!=b.modified.tv_sec?a.modified.tv_sec>b.modified.tv_sec:a.modified.tv_nsec>b.modified.tv_nsec;});
        clock_=files.size();std::uint64_t recency=clock_;
        // 預設不整批解碼：目錄裡的字型產物約 180 MiB，每個程序啟動都解碼會多 200 MiB RSS 與數十 ms CPU，
        // 短命的 helper 子程序從未用到；get 未命中時再從磁碟讀單一產物。X64ARM_RESOURCE_CACHE_PRELOAD=1 回復整批預載。
        if(eager)for(const auto& file:files){const auto use=recency--;if(entries_.size()>=capacity_ || used_==limit_ || preload_cancelled.load(std::memory_order_relaxed))break;const auto key=resource_key(file.name);ResourceCache::Result timing;
            try{auto bytes=read_resource(directory_.value,file.name,key,limit_,timing);if(bytes.empty() || bytes.size()>limit_-used_)continue;
                const auto size=bytes.size();auto payload=retained_payload(std::move(bytes));
                entries_.emplace(file.name,Entry{std::move(payload),key,use,timing.stored_bytes,false,false,false});added(size);
                disk_bytes+=timing.stored_bytes;reads+=timing.read_ms;decodes+=timing.decode_ms;add_time(counters.read_ns,timing.read_ms);add_time(counters.decode_ns,timing.decode_ms);
            }catch(const std::bad_alloc&){break;}
        }
        if(tracing())std::fprintf(stderr,"resource_cache_preload pid=%d entries=%zu bytes=%llu stored_bytes=%llu read_ms=%.3f decode_ms=%.3f total_ms=%.3f\n",::getpid(),entries_.size(),static_cast<unsigned long long>(used_),static_cast<unsigned long long>(disk_bytes),reads,decodes,elapsed(started));
        try{writer_=std::thread([this]{write_loop();});}catch(const std::system_error&){}
    }
    ~MemoryResources(){if(writer_.joinable()){{std::lock_guard lock(mutex_);stop_=true;}ready_.notify_all();writer_.join();}}
    bool idle(){std::unique_lock lock(mutex_,std::try_to_lock);return lock.owns_lock() && !writing_ && !dirty() && !touched() && building_.empty();}
    void flush(){if(!writer_.joinable())return;std::unique_lock lock(mutex_);if(!writing_ && !dirty() && !touched())return;forced_=true;ready_.notify_all();finished_.wait(lock,[&]{return !writing_ && !dirty() && !touched();});}
    ResourceCache::Result get(const Bytes& key,const std::function<Bytes()>& create,bool compress){
        ResourceCache::Result result;const auto name=hex(key)+".bin";
        const auto memory_hit=[&]{std::lock_guard lock(mutex_);const auto found=entries_.find(name);if(found==entries_.end())return false;
            auto& entry=found->second;entry.use=++clock_;entry.touched=!entry.dirty && entry.stored_bytes;result.bytes.assign(entry.bytes->begin(),entry.bytes->end());result.hit=result.memory_hit=true;result.pending=entry.dirty;result.stored_bytes=entry.stored_bytes;counters.memory_hits.fetch_add(1,std::memory_order_relaxed);ready_.notify_all();return true;};
        if(memory_hit())return result;
        // 同程序只建置一次；跨程序首次 miss 可各自建置，不等待延後落盤。
        // 檔案發布由 index.lock 序列化，啟動路徑不持有磁碟鎖。
        bool tracked=false;
        {std::unique_lock lock(mutex_);if(building_.contains(name))counters.wait_calls.fetch_add(1,std::memory_order_relaxed);finished_.wait(lock,[&]{return !building_.contains(name);});
            if(building_.size()<capacity_)try{building_.insert(name);tracked=true;counters.active_loads.fetch_add(1,std::memory_order_relaxed);}catch(const std::bad_alloc&){}
        }
        if(!tracked){counters.bypasses.fetch_add(1,std::memory_order_relaxed);create_resource(result,create);return result;}
        struct BuildGuard {MemoryResources& owner;const std::string& name;~BuildGuard(){std::lock_guard lock(owner.mutex_);owner.building_.erase(name);counters.active_loads.fetch_sub(1,std::memory_order_relaxed);owner.finished_.notify_all();}} guard{*this,name};
        if(memory_hit())return result;
        if(writer_.joinable())try{result.bytes=read_resource(directory_.value,name,key,limit_,result);result.hit=!result.bytes.empty();}catch(const std::bad_alloc&){}
        add_time(counters.read_ns,result.read_ms);add_time(counters.decode_ns,result.decode_ms);if(result.hit)counters.disk_hits.fetch_add(1,std::memory_order_relaxed);
        if(!result.hit)create_resource(result,create);
        if(result.bytes.empty() || result.bytes.size()>limit_ || !writer_.joinable()){counters.bypasses.fetch_add(1,std::memory_order_relaxed);return result;}
        if(result.bytes.size()>policy().bytes){counters.budget_rejections.fetch_add(1,std::memory_order_relaxed);counters.bypasses.fetch_add(1,std::memory_order_relaxed);return result;}
        // 可選保留不等待背景寫入；無可回收的乾淨資料時略過保留，維持原操作結果。
        std::lock_guard lock(mutex_);trim(result.bytes.size());if(used_>limit_-result.bytes.size() || entries_.size()>=capacity_){counters.bypasses.fetch_add(1,std::memory_order_relaxed);return result;}
        const auto limit=policy().bytes;
        const auto room=[&]{const auto reserved=counters.payload_reserved_bytes.load(std::memory_order_relaxed);return reserved<=limit && result.bytes.size()<=limit-reserved;};
        while(!room() && evict_clean()){}
        if(entries_.contains(name))return result;
        const bool pending=!result.hit;const auto now=Clock::now();if(pending){if(!dirty())first_dirty_=now;last_dirty_=now;}
        try{auto bytes=retained_payload(result.bytes);
            entries_.emplace(name,Entry{std::move(bytes),key,++clock_,result.stored_bytes,pending,result.hit,compress});added(result.bytes.size());
        }catch(const std::bad_alloc&){counters.bypasses.fetch_add(1,std::memory_order_relaxed);return result;}
        result.pending=pending;ready_.notify_all();return result;
    }
};
std::mutex stores_mutex;
using StoreKey=std::tuple<std::filesystem::path,std::uint64_t,std::size_t>;
struct StoreSlot {StoreSlot(){counters.store_slots.fetch_add(1,std::memory_order_relaxed);}~StoreSlot(){counters.store_slots.fetch_sub(1,std::memory_order_relaxed);}};
// slot 先建構、最後解構：連退出中的 writer／資料成員仍占一個名額。
struct StoredResources:StoreSlot,MemoryResources {StoredResources(const std::filesystem::path& path,std::uint64_t limit,std::size_t entries):MemoryResources(path,limit,entries){}};
struct Store {std::shared_ptr<MemoryResources> value;std::uint64_t use;};
std::map<StoreKey,Store> stores;std::uint64_t store_clock{};
std::shared_ptr<MemoryResources> memory_resources(const std::filesystem::path& path,std::uint64_t limit,std::size_t entries,bool create=true){
    std::unique_lock lock(stores_mutex);const StoreKey key{path,limit,std::min(entries,maximum_index_entries)};
    const auto find=[&]() -> std::shared_ptr<MemoryResources> {const auto it=stores.find(key);if(it==stores.end())return {};it->second.use=++store_clock;return it->second.value;};
    if(auto found=find())return found;if(!create || !policy().bytes || !policy().stores)return {};
    if(counters.store_slots.load(std::memory_order_relaxed)>=policy().stores){
        auto old=stores.end();
        for(auto it=stores.begin();it!=stores.end();++it)if(it->second.value.use_count()==1 && it->second.value->idle() && (old==stores.end() || it->second.use<old->second.use))old=it;
        if(old!=stores.end()){
            auto retired=std::move(old->second.value);stores.erase(old);counters.store_evictions.fetch_add(1,std::memory_order_relaxed);
            lock.unlock();retired.reset();lock.lock(); // 不在登錄表鎖內等待 thread 結束。
            if(auto found=find())return found;
        }
        if(counters.store_slots.load(std::memory_order_relaxed)>=policy().stores){counters.store_rejections.fetch_add(1,std::memory_order_relaxed);return {};}
    }
    std::shared_ptr<MemoryResources> value=std::make_shared<StoredResources>(path,limit,std::get<2>(key));
    try{stores.emplace(key,Store{value,++store_clock});}catch(...){lock.unlock();value.reset();throw;}
    return value;
}
}
ResourceCacheStats generated_resource_cache_stats() noexcept {
    ResourceCacheStats result;
    result.payload_budget_bytes=policy().bytes;result.store_limit=policy().stores;result.entry_limit_per_store=maximum_index_entries;
    result.reserved_stores=counters.store_slots.load(std::memory_order_relaxed);
#define COPY_STAT(field) result.field=counters.field.load(std::memory_order_relaxed)
    COPY_STAT(stores);COPY_STAT(entries);COPY_STAT(retained_bytes);
    COPY_STAT(payload_reserved_bytes);COPY_STAT(peak_payload_reserved_bytes);COPY_STAT(budget_rejections);COPY_STAT(store_rejections);COPY_STAT(store_evictions);
    COPY_STAT(live_payloads);COPY_STAT(live_payload_bytes);COPY_STAT(peak_live_payload_bytes);
    COPY_STAT(writer_held_bytes);COPY_STAT(peak_writer_held_bytes);COPY_STAT(active_loads);
    COPY_STAT(requests);COPY_STAT(memory_hits);COPY_STAT(disk_hits);COPY_STAT(build_calls);COPY_STAT(build_failures);COPY_STAT(wait_calls);COPY_STAT(evictions);COPY_STAT(bypasses);
    COPY_STAT(build_ns);COPY_STAT(read_ns);COPY_STAT(decode_ns);COPY_STAT(encode_ns);COPY_STAT(write_ns);COPY_STAT(write_failures);
#undef COPY_STAT
    return result;
}
ResourceCache::Result ResourceCache::get_or_create(std::span<const std::uint8_t> identity,const std::function<Bytes()>& create)const{
    counters.requests.fetch_add(1,std::memory_order_relaxed);
    if(identity.empty() || directory_.empty() || limit_<=header_size || !entries_){counters.bypasses.fetch_add(1,std::memory_order_relaxed);Result result;create_resource(result,create);return result;}
    const auto* mode=std::getenv("X64ARM_RESOURCE_CACHE_COMPRESSION");
    std::shared_ptr<MemoryResources> store;try{store=memory_resources(directory_,limit_,entries_);}catch(const std::bad_alloc&){}
    if(store)return store->get(digest(identity),create,compress_ && !(mode && std::string_view(mode)=="0"));
    counters.bypasses.fetch_add(1,std::memory_order_relaxed);Result result;create_resource(result,create);return result;
}
void ResourceCache::preload()const{if(!directory_.empty() && limit_>header_size && entries_)try{(void)memory_resources(directory_,limit_,entries_);}catch(const std::bad_alloc&){};}
void ResourceCache::flush()const{if(!directory_.empty() && limit_>header_size && entries_)try{if(auto store=memory_resources(directory_,limit_,entries_,false))store->flush();}catch(const std::bad_alloc&){};}
void preload_generated_resources(){ResourceCache(generated_resource_cache_directory()).preload();}
namespace {
// 預載執行緒在 memory_resources 建構期間持有 stores_mutex，首次 get_or_create／flush 會等它完成。
// 函式內靜態物件晚於 stores 建構，因此先於 stores 解構：結束時先取消再 join，執行緒不會碰到已解構的表。
struct PreloadThread {
    std::thread worker;
    ~PreloadThread(){preload_cancelled.store(true,std::memory_order_relaxed);if(worker.joinable())worker.join();}
};
}
void preload_generated_resources_async(){
    static PreloadThread holder;
    if(holder.worker.joinable())return;
    try{holder.worker=std::thread([]{try{preload_generated_resources();}catch(...){}});}
    catch(const std::system_error&){preload_generated_resources();}
}
void flush_generated_resources(){
    std::vector<std::shared_ptr<MemoryResources>> copies;{std::lock_guard lock(stores_mutex);for(const auto& [key,value]:stores){(void)key;copies.push_back(value.value);}}for(const auto& store:copies)store->flush();
    if(tracing())try{
        const auto stats=generated_resource_cache_stats();std::ostringstream text;text<<"resource_cache_stats pid="<<::getpid();
#define APPEND_STAT(field) text<<" " #field "="<<stats.field
        APPEND_STAT(stores);APPEND_STAT(entries);APPEND_STAT(retained_bytes);
        APPEND_STAT(payload_budget_bytes);APPEND_STAT(payload_reserved_bytes);APPEND_STAT(peak_payload_reserved_bytes);APPEND_STAT(store_limit);APPEND_STAT(reserved_stores);APPEND_STAT(entry_limit_per_store);APPEND_STAT(budget_rejections);APPEND_STAT(store_rejections);APPEND_STAT(store_evictions);
        APPEND_STAT(live_payloads);APPEND_STAT(live_payload_bytes);APPEND_STAT(peak_live_payload_bytes);
        APPEND_STAT(writer_held_bytes);APPEND_STAT(peak_writer_held_bytes);APPEND_STAT(active_loads);
        APPEND_STAT(requests);APPEND_STAT(memory_hits);APPEND_STAT(disk_hits);APPEND_STAT(build_calls);APPEND_STAT(build_failures);APPEND_STAT(wait_calls);APPEND_STAT(evictions);APPEND_STAT(bypasses);
        APPEND_STAT(build_ns);APPEND_STAT(read_ns);APPEND_STAT(decode_ns);APPEND_STAT(encode_ns);APPEND_STAT(write_ns);APPEND_STAT(write_failures);
#undef APPEND_STAT
        std::fprintf(stderr,"%s\n",text.str().c_str());
    }catch(const std::bad_alloc&){} // 診斷配置失敗不能影響正常結束。
}
std::filesystem::path generated_resource_cache_directory(){
    if(const auto* enabled=std::getenv("X64ARM_RESOURCE_CACHE");enabled && std::string_view(enabled)=="0")return {};
    if(const auto* path=std::getenv("X64ARM_RESOURCE_CACHE_DIR");path && *path)return path;
    const auto size=::confstr(_CS_DARWIN_USER_CACHE_DIR,nullptr,0);if(!size)return {};
    std::vector<char> path(size);if(!::confstr(_CS_DARWIN_USER_CACHE_DIR,path.data(),path.size()))return {};
    return std::filesystem::path(path.data())/"x64arm-generated-resources-v1";
}
}
