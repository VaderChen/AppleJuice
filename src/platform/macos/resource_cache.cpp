#include "x64arm/resource_cache.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <compression.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <map>
#include <set>
#include <memory>
#include <mutex>
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
bool publish(int directory,const std::string& name,const Bytes& key,Bytes& bytes,std::uint64_t limit,std::size_t capacity,bool compress,ResourceCache::Result& result){
    if(bytes.empty() || bytes.size()>maximum_resource || !capacity)return false;
    const auto encoding=Clock::now();Bytes packed;unsigned codec=0;
    if(compress && bytes.size()>=4096){packed.resize(bytes.size());const auto size=compression_encode_buffer(packed.data(),packed.size(),bytes.data(),bytes.size(),nullptr,COMPRESSION_LZ4);
        if(size && size+64<bytes.size()){packed.resize(size);codec=1;}else packed.clear();}
    auto& payload=codec?packed:bytes;result.encode_ms=elapsed(encoding);
    if(payload.size()>limit-header_size)return false;
    const auto writing=Clock::now();
    Fd lock{::openat(directory,"index.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600)};
    if(!lock || !safe(lock.value) || !lock_file(lock.value,LOCK_EX))return false;
    ResourceCache::Result existing;const auto previous=read_resource(directory,name,key,limit,existing);
    if(previous==bytes){result.stored_bytes=existing.stored_bytes;result.write_ms=elapsed(writing);return true;}
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
        total+=static_cast<std::uint64_t>(st.st_size);entries.push_back({std::string(current),static_cast<std::uint64_t>(st.st_size),st.st_mtimespec});
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
    if(!transfer(file.value,header,true) || !transfer(file.value,payload,true))return false;
    const bool stored=::renameat(directory,temporary.c_str(),directory,name.c_str())==0;
    result.write_ms=elapsed(writing);if(stored)result.stored_bytes=required;return stored;
}
bool tracing(){const auto* value=std::getenv("X64ARM_RESOURCE_CACHE_TRACE");return value && std::string_view(value)=="1";}
Bytes resource_key(std::string_view name){Bytes key(32);const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};for(unsigned i=0;i<32;++i)key[i]=static_cast<std::uint8_t>((digit(name[2*i])<<4)|digit(name[2*i+1]));return key;}
class MemoryResources {
    struct Entry {
        std::shared_ptr<Bytes> bytes;Bytes key;
        std::uint64_t use{},stored_bytes{};bool dirty{},touched{},compress{};
    };
    Fd directory_;std::uint64_t limit_,used_{},clock_{};std::size_t capacity_;
    std::map<std::string,Entry> entries_;std::set<std::string> building_;std::mutex mutex_;std::condition_variable ready_,finished_;std::thread writer_;
    bool stop_{},forced_{},writing_{};Clock::time_point first_dirty_{},last_dirty_{},last_flush_{Clock::now()};
    bool dirty()const{return std::any_of(entries_.begin(),entries_.end(),[](const auto& item){return item.second.dirty;});}
    bool touched()const{return std::any_of(entries_.begin(),entries_.end(),[](const auto& item){return item.second.touched;});}
    void trim(std::uint64_t incoming){
        while(!entries_.empty() && (used_>limit_-incoming || entries_.size()>=capacity_)){
            auto old=entries_.end();for(auto it=entries_.begin();it!=entries_.end();++it)if(!it->second.dirty && (old==entries_.end() || it->second.use<old->second.use))old=it;
            if(old==entries_.end())break;used_-=old->second.bytes->size();entries_.erase(old);
        }
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
            std::vector<std::pair<std::string,Entry>> batch;
            for(auto& [name,entry]:entries_)if(entry.dirty || entry.touched){batch.emplace_back(name,entry);entry.touched=false;}
            std::stable_sort(batch.begin(),batch.end(),[](const auto& a,const auto& b){return a.second.dirty!=b.second.dirty ? !a.second.dirty : a.second.use<b.second.use;});
            // 寫入期間到達的新產物屬於下一批，不能沿用已到期的舊期限而逐筆寫。
            first_dirty_=Clock::now();forced_=false;writing_=true;lock.unlock();
            for(auto& [name,entry]:batch){
                ResourceCache::Result timing;bool stored=false;
                if(entry.dirty){try{stored=publish(directory_.value,name,entry.key,*entry.bytes,limit_,capacity_,entry.compress,timing);}catch(const std::bad_alloc&){}
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
        struct File{std::string name;timespec modified;};std::vector<File> files;
        const auto copy=::openat(directory_.value,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);auto* stream=copy<0 ? nullptr : ::fdopendir(copy);
        if(stream){while(const auto* item=::readdir(stream))if(resource_name(item->d_name)){
            struct stat st{};if(!::fstatat(directory_.value,item->d_name,&st,AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode))files.push_back({item->d_name,st.st_mtimespec});}
            ::closedir(stream);
        }else if(copy>=0)::close(copy);
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.modified.tv_sec!=b.modified.tv_sec?a.modified.tv_sec>b.modified.tv_sec:a.modified.tv_nsec>b.modified.tv_nsec;});
        clock_=files.size();std::uint64_t recency=clock_;
        for(const auto& file:files){const auto use=recency--;if(entries_.size()>=capacity_ || used_==limit_ || preload_cancelled.load(std::memory_order_relaxed))break;const auto key=resource_key(file.name);ResourceCache::Result timing;
            try{auto bytes=read_resource(directory_.value,file.name,key,limit_,timing);if(bytes.empty() || bytes.size()>limit_-used_)continue;
                used_+=bytes.size();disk_bytes+=timing.stored_bytes;reads+=timing.read_ms;decodes+=timing.decode_ms;
                entries_.emplace(file.name,Entry{std::make_shared<Bytes>(std::move(bytes)),key,use,timing.stored_bytes,false,false,false});
            }catch(const std::bad_alloc&){break;}
        }
        if(tracing())std::fprintf(stderr,"resource_cache_preload pid=%d entries=%zu bytes=%llu stored_bytes=%llu read_ms=%.3f decode_ms=%.3f total_ms=%.3f\n",::getpid(),entries_.size(),static_cast<unsigned long long>(used_),static_cast<unsigned long long>(disk_bytes),reads,decodes,elapsed(started));
        try{writer_=std::thread([this]{write_loop();});}catch(const std::system_error&){}
    }
    ~MemoryResources(){if(writer_.joinable()){{std::lock_guard lock(mutex_);stop_=true;}ready_.notify_all();writer_.join();}}
    void flush(){if(!writer_.joinable())return;std::unique_lock lock(mutex_);forced_=true;ready_.notify_all();finished_.wait(lock,[&]{return !writing_ && !dirty() && !touched();});}
    ResourceCache::Result get(const Bytes& key,const std::function<Bytes()>& create,bool compress){
        ResourceCache::Result result;const auto name=hex(key)+".bin";
        const auto memory_hit=[&]{std::lock_guard lock(mutex_);const auto found=entries_.find(name);if(found==entries_.end())return false;
            auto& entry=found->second;entry.use=++clock_;entry.touched=!entry.dirty && entry.stored_bytes;result.bytes=*entry.bytes;result.hit=result.memory_hit=true;result.pending=entry.dirty;result.stored_bytes=entry.stored_bytes;ready_.notify_all();return true;};
        if(memory_hit())return result;
        // 同程序只建置一次；跨程序首次 miss 可各自建置，不等待延後落盤。
        // 檔案發布由 index.lock 序列化，啟動路徑不持有磁碟鎖。
        {std::unique_lock lock(mutex_);finished_.wait(lock,[&]{return !building_.contains(name);});building_.insert(name);}
        struct BuildGuard {MemoryResources& owner;const std::string& name;~BuildGuard(){std::lock_guard lock(owner.mutex_);owner.building_.erase(name);owner.finished_.notify_all();}} guard{*this,name};
        if(memory_hit())return result;
        if(writer_.joinable())try{result.bytes=read_resource(directory_.value,name,key,limit_,result);result.hit=!result.bytes.empty();}catch(const std::bad_alloc&){}
        if(!result.hit){const auto start=Clock::now();result.bytes=create();result.build_ms=elapsed(start);}
        if(result.bytes.empty() || result.bytes.size()>limit_ || !writer_.joinable())return result;
        // 只有額度壓力才提前補寫；一般呼叫把產物放入 RAM 後立即返回。
        {std::lock_guard lock(mutex_);trim(result.bytes.size());}
        bool pressure;{std::lock_guard lock(mutex_);pressure=used_>limit_-result.bytes.size() || entries_.size()>=capacity_;}
        if(pressure)flush();
        std::lock_guard lock(mutex_);trim(result.bytes.size());if(used_>limit_-result.bytes.size() || entries_.size()>=capacity_)return result;
        if(entries_.contains(name))return result;
        const bool pending=!result.hit;const auto now=Clock::now();if(pending){if(!dirty())first_dirty_=now;last_dirty_=now;}
        auto bytes=std::make_shared<Bytes>(result.bytes);used_+=bytes->size();
        entries_.emplace(name,Entry{std::move(bytes),key,++clock_,result.stored_bytes,pending,result.hit,compress});
        result.pending=pending;ready_.notify_all();return result;
    }
};
std::mutex stores_mutex;
using StoreKey=std::tuple<std::filesystem::path,std::uint64_t,std::size_t>;
std::map<StoreKey,std::shared_ptr<MemoryResources>> stores;
std::shared_ptr<MemoryResources> memory_resources(const std::filesystem::path& path,std::uint64_t limit,std::size_t entries){
    std::lock_guard lock(stores_mutex);const StoreKey key{path,limit,entries};auto& value=stores[key];if(!value)value=std::make_shared<MemoryResources>(path,limit,entries);return value;
}
}
ResourceCache::Result ResourceCache::get_or_create(std::span<const std::uint8_t> identity,const std::function<Bytes()>& create)const{
    if(identity.empty() || directory_.empty() || limit_<=header_size || !entries_){Result result;const auto start=Clock::now();result.bytes=create();result.build_ms=elapsed(start);return result;}
    const auto* mode=std::getenv("X64ARM_RESOURCE_CACHE_COMPRESSION");
    return memory_resources(directory_,limit_,entries_)->get(digest(identity),create,compress_ && !(mode && std::string_view(mode)=="0"));
}
void ResourceCache::preload()const{if(!directory_.empty() && limit_>header_size && entries_)(void)memory_resources(directory_,limit_,entries_);}
void ResourceCache::flush()const{if(!directory_.empty() && limit_>header_size && entries_)memory_resources(directory_,limit_,entries_)->flush();}
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
void flush_generated_resources(){std::vector<std::shared_ptr<MemoryResources>> copies;{std::lock_guard lock(stores_mutex);for(const auto& [key,value]:stores){(void)key;copies.push_back(value);}}for(const auto& store:copies)store->flush();}
std::filesystem::path generated_resource_cache_directory(){
    if(const auto* enabled=std::getenv("X64ARM_RESOURCE_CACHE");enabled && std::string_view(enabled)=="0")return {};
    if(const auto* path=std::getenv("X64ARM_RESOURCE_CACHE_DIR");path && *path)return path;
    const auto size=::confstr(_CS_DARWIN_USER_CACHE_DIR,nullptr,0);if(!size)return {};
    std::vector<char> path(size);if(!::confstr(_CS_DARWIN_USER_CACHE_DIR,path.data(),path.size()))return {};
    return std::filesystem::path(path.data())/"x64arm-generated-resources-v1";
}
}
