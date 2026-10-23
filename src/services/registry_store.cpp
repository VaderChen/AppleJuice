#include "x64arm/registry_store.hpp"
#include "x64arm/security_descriptor.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace x64arm {
namespace {
thread_local RegistryIoScope* io_scope{};
constexpr std::size_t limit=32U*1024U*1024U;
// 與共用慢服務診斷一起啟用；正常路徑不讀時鐘、不寫診斷。
class RegistryTrace {
    bool enabled_{};const char* phase_;std::chrono::steady_clock::time_point start_{};
public:
    explicit RegistryTrace(const char* phase):phase_(phase){
        static const bool enabled=std::getenv("X64ARM_TRACE_SLOW_CALLS")!=nullptr;
        enabled_=enabled;if(enabled_)start_=std::chrono::steady_clock::now();
    }
    std::int64_t next(const char* phase){
        if(!enabled_)return 0;
        const auto now=std::chrono::steady_clock::now();
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(now-start_).count();
        if(elapsed>=2000)std::fprintf(stderr,"slow_registry pid=%d phase=%s duration_us=%lld end_unix_us=%lld\n",
            static_cast<int>(::getpid()),phase_,static_cast<long long>(elapsed),
            static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
        phase_=phase;start_=now;return elapsed;
    }
    ~RegistryTrace(){next("finished");}
};
struct Descriptor {int value{-1};void close() noexcept {if(value>=0){::close(value);value=-1;}}~Descriptor(){close();}};
std::array<std::uint64_t,7> file_stamp(const struct stat& status){
#if defined(__APPLE__)
    const auto modified=status.st_mtimespec,changed=status.st_ctimespec;
#else
    const auto modified=status.st_mtim,changed=status.st_ctim;
#endif
    return {static_cast<std::uint64_t>(status.st_dev),static_cast<std::uint64_t>(status.st_ino),static_cast<std::uint64_t>(status.st_size),static_cast<std::uint64_t>(modified.tv_sec),static_cast<std::uint64_t>(modified.tv_nsec),static_cast<std::uint64_t>(changed.tv_sec),static_cast<std::uint64_t>(changed.tv_nsec)};
}
void cache_bytes(RegistryStoreCache* cache,const std::filesystem::path& path,
                 const std::array<std::uint64_t,7>& stamp,std::vector<std::uint8_t>&& bytes) noexcept {
    if(!cache)return;
    // 快取配置失敗不能改變已完成的 I/O 結果；容量超限時立即釋放舊快照。
    std::vector<std::uint8_t>{}.swap(cache->bytes);
    if(bytes.capacity()>RegistryStoreCache::byte_limit)return;
    try{cache->byte_path=path;cache->byte_stamp=stamp;cache->bytes=std::move(bytes);}catch(...){ }
}
struct Reader {
    std::span<const std::uint8_t> data;std::size_t offset{};
    std::uint64_t number(unsigned size){if(size>data.size()-offset)throw std::runtime_error("登錄檔截斷");std::uint64_t value=0;for(unsigned i=0;i<size;++i)value|=std::uint64_t{data[offset++]}<<(i*8);return value;}
    std::u16string text(){const auto size=number(4);if(size>32768 || size>(data.size()-offset)/2)throw std::runtime_error("登錄字串超過範圍");std::u16string result;for(std::uint64_t i=0;i<size;++i)result.push_back(static_cast<char16_t>(number(2)));return result;}
};
void number(std::vector<std::uint8_t>& data,std::uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)data.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
void text(std::vector<std::uint8_t>& data,const std::u16string& value){number(data,value.size(),4);for(auto c:value)number(data,c,2);}
std::uint32_t io_error(){return errno==EACCES || errno==EPERM || errno==ELOOP?5:errno==ENOENT?3:29;}
// 呼叫端持有交易鎖；原子取代使其他程序立即看見變更，耐久化另由 lazy writer 或明確 Flush 完成。
std::uint32_t flush_file(const std::filesystem::path& path){
    std::filesystem::path parent;
    try{parent=path.parent_path();}catch(const std::bad_alloc&){return 8;}
    RegistryTrace trace("flush_file_open");
    Descriptor file{::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};
    if(file.value<0)return errno==ENOENT?0:io_error();
    trace.next("flush_file_sync");
    if(::fsync(file.value))return io_error();
    trace.next("flush_directory_open");
    Descriptor directory{::open(parent.empty()?".":parent.c_str(),O_RDONLY|O_CLOEXEC)};
    trace.next("flush_directory_sync");
    return directory.value<0 || ::fsync(directory.value)?io_error():0;
}
struct LazyWriter {
    // 發布前先保留固定容量的排程槽；發布後只改旗標，不再配置記憶體。
    // 同一路徑共用槽，背景 fsync 期間的新提交仍保留下一輪 dirty 標記。
    struct Slot {std::filesystem::path path;std::size_t reservations{};bool dirty{},flushing{};};
    std::mutex mutex;std::condition_variable changed;std::array<Slot,64> pending;bool stopping{};
    bool ready() const {return std::any_of(pending.begin(),pending.end(),[](const auto& slot){return slot.dirty;});}
    void reclaim(Slot& slot) noexcept {if(!slot.reservations && !slot.dirty && !slot.flushing)slot.path.clear();}
    std::optional<std::size_t> reserve(const std::filesystem::path& path){
        auto owned_path=path;std::lock_guard lock(mutex);
        for(std::size_t i=0;i<pending.size();++i)if(pending[i].path==path){++pending[i].reservations;return i;}
        for(std::size_t i=0;i<pending.size();++i)if(pending[i].path.empty()){
            pending[i].path=std::move(owned_path);pending[i].reservations=1;return i;
        }
        return {};
    }
    void release(std::size_t index,bool published) noexcept {
        {std::lock_guard lock(mutex);auto& slot=pending[index];slot.dirty|=published;--slot.reservations;reclaim(slot);}
        changed.notify_one();
    }
    struct Reservation {
        LazyWriter& writer;std::optional<std::size_t> index;bool published{};
        Reservation(LazyWriter& owner,const std::filesystem::path& path):writer(owner),index(owner.reserve(path)){}
        Reservation(const Reservation&)=delete;
        Reservation& operator=(const Reservation&)=delete;
        ~Reservation(){if(index)writer.release(*index,published);}
    };
    std::thread worker;
    LazyWriter():worker([this]{
        std::unique_lock lock(mutex);
        for(;;){
            changed.wait(lock,[&]{return stopping || ready();});
            if(!stopping)changed.wait_for(lock,std::chrono::seconds(1),[&]{return stopping;});
            const bool last=stopping;
            for(auto& slot:pending){
                if(!slot.dirty)continue;
                slot.dirty=false;slot.flushing=true;lock.unlock();
                // 提交以 rename 發布不可變快照；已開啟的 fd 在下一次取代後仍有效。
                // 背景落盤不持有交易鎖，避免 fsync 停頓連帶阻塞所有登錄 API
                // 及同一協調者上的音訊。並行提交仍會排入下一批，明確 Flush
                // 則維持呼叫端的交易鎖，保證其呼叫前已發布的內容完成落盤。
                const auto error=flush_file(slot.path);
                lock.lock();slot.flushing=false;if(error && !last)slot.dirty=true;reclaim(slot);
            }
            if(last)break;
        }
    }){}
    ~LazyWriter(){ {std::lock_guard lock(mutex);stopping=true;}changed.notify_one();worker.join(); }
};
LazyWriter& lazy_writer(){static LazyWriter writer;return writer;}
}
RegistryIoScope::RegistryIoScope(Runner runner):runner_(std::move(runner)),previous_(io_scope){io_scope=this;}
RegistryIoScope::~RegistryIoScope(){io_scope=previous_;}
void run_registry_io(const RegistryIoScope::Work& work){if(io_scope && io_scope->runner_)io_scope->runner_(work);else work();}
RegistryTransaction::RegistryTransaction(RegistryState& state,const std::filesystem::path& path,RegistryStoreCache* cache,bool readonly):state_(state),path_(path),cache_(cache),readonly_(readonly){
    if(path.empty())return;
    // 只把不可變的 metadata 複本交給 I/O；worker 不讀寫 RegistryState 或 cache。
    const auto io_path=path;const auto lock_path=path.string()+".lock";
    const bool parsed_candidate=cache_ && cache_->valid && cache_->owner==&state && cache_->path==path;
    const bool byte_candidate=cache_ && cache_->byte_path==path;
    const auto parsed_stamp=cache_?cache_->stamp:std::array<std::uint64_t,7>{};
    const auto byte_stamp=cache_?cache_->byte_stamp:std::array<std::uint64_t,7>{};
    const auto byte_size=cache_?cache_->bytes.size():0;
    struct Load {
        Descriptor lock;std::uint32_t error{};std::array<std::uint64_t,7> stamp{};
        std::vector<std::uint8_t> bytes;std::int64_t read_us{};
        bool found{},parsed_hit{},byte_hit{},read_attempted{},stable{};
    } io;
    // Runner 若因客體終止拋出，必須先 drain；這個局部 fd 保證 constructor
    // 尚未完成時也會釋放 flock，不依賴不會執行的 RegistryTransaction destructor。
    try {run_registry_io([&]{
        RegistryTrace trace("open_lock");
        io.lock.value=::open(lock_path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
        if(io.lock.value<0){io.error=io_error();return;}
        trace.next("lock");
        if(::flock(io.lock.value,LOCK_EX)){io.error=io_error();return;}
        trace.next("open_file");
        Descriptor file{::open(io_path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};
        if(file.value<0){if(errno!=ENOENT)io.error=io_error();return;}
        trace.next("stat");
        struct stat status{};
        if(::fstat(file.value,&status) || !S_ISREG(status.st_mode) || status.st_size<16 || static_cast<std::uint64_t>(status.st_size)>limit){io.error=13;return;}
        io.found=true;io.stamp=file_stamp(status);
        io.parsed_hit=parsed_candidate && parsed_stamp==io.stamp;
        io.byte_hit=byte_candidate && byte_stamp==io.stamp && byte_size==static_cast<std::size_t>(status.st_size);
        if(!io.parsed_hit && !io.byte_hit){
            trace.next("read");
            io.bytes.resize(static_cast<std::size_t>(status.st_size));std::size_t read=0;io.read_attempted=true;
            while(read<io.bytes.size()){
                const auto n=::read(file.value,io.bytes.data()+read,io.bytes.size()-read);
                if(n<0 && errno==EINTR)continue;if(n<=0){io.error=13;return;}read+=static_cast<std::size_t>(n);
            }
            io.read_us=trace.next("validate_file");
        }
        // 不合作的外部寫入者可能沒有持鎖；讀取期間變更時不保存該快照。
        struct stat latest{};io.stable=!::fstat(file.value,&latest) && file_stamp(latest)==io.stamp;
        trace.next("close_file");file.close();
    });}catch(...){if(cache_)cache_->valid=false;throw;}
    lock_=io.lock.value;io.lock.value=-1;error=io.error;
    if(cache_ && io.read_attempted)++cache_->disk_reads;
    if(cache_ && (error || (io.found && !io.parsed_hit)))cache_->valid=false;
    if(error || !io.found)return;
    if(io.parsed_hit){++cache_->hits;return;}
    auto& loaded_bytes=io.bytes;const auto& stamp=io.stamp;const bool byte_hit=io.byte_hit;
    std::span<const std::uint8_t> data=byte_hit?std::span<const std::uint8_t>{cache_->bytes}:std::span<const std::uint8_t>{loaded_bytes};
    if(byte_hit)++cache_->byte_hits;
    RegistryTrace trace("decode");
    if(!byte_hit && cache_ && io.read_us>=2000)std::fprintf(stderr,
        "slow_registry_cache pid=%d bytes=%llu disk_reads=%llu byte_hits=%llu parsed_hits=%llu prior_bytes=%zu stamp_match=%d\n",
        static_cast<int>(::getpid()),static_cast<unsigned long long>(stamp[2]),
        static_cast<unsigned long long>(cache_->disk_reads),static_cast<unsigned long long>(cache_->byte_hits),
        static_cast<unsigned long long>(cache_->hits),cache_->bytes.size(),cache_->byte_path==path && cache_->byte_stamp==stamp);
    try {
        Reader reader{data};if(reader.number(8)!=UINT64_C(0x314745524d524158))throw std::runtime_error("未知登錄格式");
        const auto version=reader.number(4);if(version<1 || version>3)throw std::runtime_error("未知登錄版本");
        RegistryState loaded;const auto count=reader.number(4);if(count>4096)throw std::runtime_error("登錄索引過多");
        for(std::uint64_t i=0;i<count;++i){
            const auto key_path=reader.text();if(key_path.empty() || key_path[0]>6 || key_path[0]==4 || loaded.keys.contains(key_path))throw std::runtime_error("無效登錄索引");
            loaded.display_names[key_path]=reader.text();loaded.modified_times[key_path]=reader.number(8);auto& values=loaded.keys[key_path];const auto value_count=reader.number(4);
            if(value_count>4096-loaded.values)throw std::runtime_error("登錄值過多");loaded.values+=value_count;
            for(std::uint64_t j=0;j<value_count;++j){const auto original=reader.text();auto name=original;for(auto& c:name)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);if(values.contains(name))throw std::runtime_error("重複登錄值");RegistryValue value;if(original!=name)value.display_name=original;value.type=static_cast<std::uint32_t>(reader.number(4));const auto size=reader.number(4);
                if(size>16U*1024U*1024U-loaded.bytes || size>data.size()-reader.offset)throw std::runtime_error("登錄值超過範圍");loaded.bytes+=size;
                value.bytes.assign(data.begin()+static_cast<std::ptrdiff_t>(reader.offset),data.begin()+static_cast<std::ptrdiff_t>(reader.offset+size));reader.offset+=static_cast<std::size_t>(size);values.emplace(name,std::move(value));
            }
            if(version>=2){const auto size=reader.number(4);if(size>262144 || size>data.size()-reader.offset)throw std::runtime_error("安全描述元超過範圍");
                if(size){std::vector<std::uint8_t> descriptor(data.begin()+static_cast<std::ptrdiff_t>(reader.offset),data.begin()+static_cast<std::ptrdiff_t>(reader.offset+size));(void)security::parse_relative_descriptor(descriptor);loaded.security_descriptors.emplace(key_path,std::move(descriptor));}
                reader.offset+=static_cast<std::size_t>(size);}
            if(version>=3){auto class_name=reader.text();if(!class_name.empty())loaded.classes.emplace(key_path,std::move(class_name));const auto options=reader.number(4);if(options>1)throw std::runtime_error("登錄選項無效");if(options)loaded.volatile_keys.insert(key_path);}
        }
        if(version>=3){const auto mounts=reader.number(4);if(mounts>128)throw std::runtime_error("掛載 hive 過多");
            for(std::uint64_t i=0;i<mounts;++i){const auto mount_root=reader.text();RegistryHiveMount mount;mount.source=reader.text();mount.device=reader.number(8);mount.id=reader.number(8);mount.minor=static_cast<std::uint32_t>(reader.number(4));
                if(!loaded.keys.contains(mount_root) || loaded.hives.contains(mount_root) || mount.source.empty() || !mount.id || (mount.minor!=3 && mount.minor!=5))throw std::runtime_error("掛載 hive 紀錄無效");loaded.hives.emplace(mount_root,std::move(mount));}}
        if(reader.offset!=data.size())throw std::runtime_error("登錄檔有未解析資料");
        loaded.handles=std::move(state.handles);loaded.next_handle=state.next_handle;state=std::move(loaded);
        if(cache_){
            ++cache_->loads;
            if(io.stable){
                cache_->owner=&state;cache_->path=path;cache_->stamp=stamp;cache_->valid=true;
                if(!byte_hit)cache_bytes(cache_,path,stamp,std::move(loaded_bytes));
            }
        }
    }catch(const SecurityDescriptorError&){error=13;}catch(const std::exception&){error=13;}
}
RegistryTransaction::~RegistryTransaction(){
    RegistryTrace trace("release_lock");
    // 可寫操作即使失敗也可能改過暫存狀態；下一次必須以持久資料重新建立快照。
    if(cache_ && (!readonly_ || error))cache_->valid=false;
    if(lock_>=0){::flock(lock_,LOCK_UN);::close(lock_);}
}
std::uint32_t RegistryTransaction::commit(){
    if(error)return error;if(path_.empty())return 0;
    RegistryTrace trace("serialize");
    const unsigned version=state_.classes.empty() && state_.volatile_keys.empty() && state_.hives.empty()?2U:3U;
    const auto generated=[&](const std::u16string& path){return std::any_of(state_.generated_keys.begin(),state_.generated_keys.end(),[&](const auto& root){return path==root || path.starts_with(root+u"\\");});};
    const auto key_count=static_cast<std::uint64_t>(std::count_if(state_.keys.begin(),state_.keys.end(),[&](const auto& item){return !generated(item.first);}));
    std::vector<std::uint8_t> data;number(data,UINT64_C(0x314745524d524158),8);number(data,version,4);number(data,key_count,4);
    for(const auto& [path,values]:state_.keys){if(generated(path))continue;text(data,path);const auto display=state_.display_names.find(path);text(data,display==state_.display_names.end()?path.substr(path.find_last_of(u'\\')+1):display->second);const auto modified=state_.modified_times.find(path);number(data,modified==state_.modified_times.end()?0:modified->second,8);number(data,values.size(),4);
        for(const auto& [name,value]:values){text(data,value.display_name.empty()?name:value.display_name);number(data,value.type,4);number(data,value.bytes.size(),4);data.insert(data.end(),value.bytes.begin(),value.bytes.end());}
        const auto descriptor=state_.security_descriptors.find(path);const auto size=descriptor==state_.security_descriptors.end()?0:descriptor->second.size();
        if(size>262144)return 13;
        if(size){try{(void)security::parse_relative_descriptor(descriptor->second);}catch(const SecurityDescriptorError&){return 13;}}
        number(data,size,4);if(size)data.insert(data.end(),descriptor->second.begin(),descriptor->second.end());
        if(version>=3){const auto item=state_.classes.find(path);text(data,item==state_.classes.end()?std::u16string{}:item->second);number(data,state_.volatile_keys.contains(path)?1:0,4);}
        if(data.size()>limit)return 8;
    }
    if(version>=3){if(state_.hives.size()>128)return 8;number(data,state_.hives.size(),4);for(const auto& [path,mount]:state_.hives){if(!state_.keys.contains(path) || mount.source.empty() || !mount.id || (mount.minor!=3 && mount.minor!=5))return 13;text(data,path);text(data,mount.source);number(data,mount.device,8);number(data,mount.id,8);number(data,mount.minor,4);}}
    if(data.size()>limit)return 8;
    trace.next("prepare_io");
    const auto io_path=path_;const bool want_stamp=cache_!=nullptr;
    struct Commit {std::uint32_t error{};bool stamp_valid{};std::array<std::uint64_t,7> stamp{};} io;
    run_registry_io([&]{
        RegistryTrace trace("prepare_flush");
        // 所有可能配置或啟動執行緒的動作都在 rename 前完成。槽滿時回報
        // 資源不足且未發布；成功發布後不能再回傳會誘發 caller rollback 的錯誤。
        std::optional<LazyWriter::Reservation> reservation;
        try{reservation.emplace(lazy_writer(),io_path);}
        catch(const std::bad_alloc&){io.error=8;return;}
        catch(const std::system_error&){io.error=29;return;}
        if(!reservation->index){io.error=8;return;}
        auto pattern=io_path.string()+".tmp-XXXXXX";
        std::vector<char> temporary(pattern.begin(),pattern.end());temporary.push_back(0);
        trace.next("create_file");
        Descriptor file{::mkstemp(temporary.data())};if(file.value<0){io.error=io_error();return;}
        std::size_t written=0;trace.next("write");
        while(written<data.size()){
            const auto n=::write(file.value,data.data()+written,data.size()-written);
            if(n<0 && errno==EINTR)continue;if(n<=0){io.error=io_error();break;}written+=static_cast<std::size_t>(n);
        }
        struct stat written_status{};
        const bool written_stamp_valid=want_stamp && !io.error && !::fstat(file.value,&written_status);
        trace.next("rename");
        if(!io.error && ::rename(temporary.data(),io_path.c_str()))io.error=io_error();
        trace.next("published_stat");
        if(!io.error){
            reservation->published=true;
            // rename 可能更新 ctime；dev/ino/size/mtime 必須與寫入的 inode 一致。
            struct stat published{};const auto before_stamp=file_stamp(written_status);
            if(written_stamp_valid && !::fstat(file.value,&published)){
                io.stamp=file_stamp(published);
                io.stamp_valid=std::equal(before_stamp.begin(),before_stamp.begin()+5,io.stamp.begin());
            }
        }else ::unlink(temporary.data());
        trace.next("close_file");file.close();
        trace.next("schedule_flush");reservation.reset();
    });
    trace.next("cache_publish");
    if(!io.error && cache_){
        // worker 只回傳 metadata；快取發布及獨立 byte 快照的所有權留在協調者。
        cache_->valid=false;
        if(io.stamp_valid)cache_bytes(cache_,path_,io.stamp,std::move(data));
        else std::vector<std::uint8_t>{}.swap(cache_->bytes);
    }
    return io.error;
}
std::uint32_t RegistryTransaction::flush(){
    if(error || path_.empty())return error;
    const auto path=path_;std::uint32_t result{};
    run_registry_io([&]{result=flush_file(path);});return result;
}
}
