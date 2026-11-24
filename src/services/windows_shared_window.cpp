#include "ansi_codepage.hpp"
#include "vidpn_ownership.hpp"
#include "mouse_move_history.hpp"
#include "gui_window_menu.hpp"
#include "window_class_name.hpp"
#include "shared_window_order.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/resource_usage.hpp"
#include "x64arm/process_identity.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include "window_presentation_fault.hpp"
#include "clipboard_formats.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <mutex>
#include <type_traits>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace x64arm {
namespace {
// 偶發黑畫面診斷：記錄第一次與最長無畫面間隔（以及結束時刻），用量快照即使事後恢復也保留證據。
void record_frame_timing(const char* first_key,const char* gap_key,std::int64_t& last_ms,std::int64_t& max_gap_ms){
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if(!last_ms)set_diagnostic(first_key,"unix_ms="+std::to_string(now));
    else if(now-last_ms>max_gap_ms){max_gap_ms=now-last_ms;set_diagnostic(gap_key,"gap_ms="+std::to_string(max_gap_ms)+" end_unix_ms="+std::to_string(now));}
    last_ms=now;
}
}
namespace {
constexpr std::uint32_t window_slots = 8192, window_magic = 0x57473458;
static_assert(std::is_trivially_copyable_v<SharedWindowInfo> && sizeof(SharedWindowInfo) % 4 == 0);
static_assert(sizeof(SharedWindowInfo)==1672 && offsetof(SharedWindowInfo,display_affinity)==1668);
struct SharedWindowSlot {
    std::uint32_t handle{}, owner{}, sequence{}, frame{};
    std::array<std::uint32_t, sizeof(SharedWindowInfo) / 4> info{};
};
struct SharedWindowTable {
    std::uint32_t magic{}, version{}, next{}, count{};
    std::array<SharedWindowSlot, window_slots> slots{};
};
struct FrameHeader {
    std::uint32_t magic{window_magic}, width{}, height{}, format{}, version{2}, stride{};
    std::uint64_t bytes{};
};
#if !defined(_WIN32)
// 獨立 page-aligned payload，Metal 直接存取，CPU 只讀寫小型標頭。
struct FrameMapping {
    void* data{MAP_FAILED};
    std::size_t size{};
    dev_t device{};ino_t inode{};
    FrameMapping(int fd,std::size_t bytes,const struct stat& status):size(bytes),device(status.st_dev),inode(status.st_ino){
        data=::mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,::sysconf(_SC_PAGESIZE));
        if(data==MAP_FAILED)throw HostCallFault("共用影格映射失敗");
    }
    ~FrameMapping(){if(data!=MAP_FAILED)::munmap(data,size);}
    FrameMapping(const FrameMapping&)=delete;
    FrameMapping& operator=(const FrameMapping&)=delete;
};
// X64ARM_SHARED_FRAME_TRACE=frames 逐幀記錄發布／匯入時間，供跨程序幀節拍診斷。
bool trace_shared_frames(){
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_SHARED_FRAME_TRACE");return value && std::string_view(value)=="frames";}();
    return enabled;
}
long long trace_unix_us(){
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}
std::uint64_t frame_bytes(std::uint32_t stride,std::uint32_t height){
    const auto page=static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    return (std::uint64_t{stride}*height+page-1)/page*page;
}
struct SharedFd {
    int fd{-1};
    ~SharedFd() {
        if (fd >= 0)
            ::close(fd);
    }
};
int lock_file(int fd, int operation) {
    int result;
    do {
        result = ::flock(fd, operation);
    } while (result && errno == EINTR);
    return result;
}
struct FileLock {
    int fd;
    explicit FileLock(int value) : fd(value) {
        if (lock_file(fd, LOCK_EX))
            throw HostCallFault("共用視窗檔案鎖失敗");
    }
    ~FileLock() { (void)lock_file(fd, LOCK_UN); }
};
int open_window_file(const std::string &name) {
    const auto fd = ::open(name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        throw HostCallFault("無法開啟共用視窗檔案");
    struct stat st{};
    if (::fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 077) ||
        st.st_nlink != 1) {
        ::close(fd);
        throw HostCallFault("共用視窗檔案擁有者／模式不符");
    }
    return fd;
}
#endif
} // namespace
std::uint32_t vidpn_ownership(GuiState& gui,const std::string& directory,std::uint32_t self,std::uint32_t pid,
                              std::uint32_t device,std::optional<std::uint32_t> type,std::uint32_t& current,std::uint32_t* owner_pid){
    struct Owner { std::uint32_t pid{},device{},type{},reserved{};std::uint64_t birth{}; };
    const auto update=[&](Owner& owner){
        if(owner_pid)*owner_pid=owner.pid;
        current=!pid || owner.pid==pid?owner.type:0U;
        if(!type)return 0U;
        const bool same=owner.pid==pid && (!device || owner.device==device);
        if(!*type){if(same)owner={};return 0U;}
        if(*type==3 || *type>4 || (same && owner.type==2 && (*type==1 || *type==4)) ||
           (same && owner.type==4 && *type==2))return 0xc000000dU;
        if(*type==1 || (owner.type && !same))return 0xc01e0342U;
        owner.pid=pid;owner.device=device;owner.type=*type;return 0U;
    };
    Owner owner{};
#if !defined(_WIN32)
    if(!directory.empty()){
        SharedFd file{open_window_file(directory+"/vidpn-source-0-v1")};
        FileLock lock(file.fd);
        ssize_t n;
        do{n=::pread(file.fd,&owner,sizeof(owner),0);}while(n<0 && errno==EINTR);
        if(n!=0 && n!=sizeof(owner))throw HostCallFault("讀取 VidPn 擁有權失敗");
        const auto prior=owner;
        if(owner.pid && process_identity::query(owner.pid).gone({owner.pid,owner.birth}))owner={};
        const auto status=update(owner);
        if(owner.pid && !owner.birth){
            const auto process=process_identity::query(owner.pid);
            if(!process.identity || process.exited)return 0xc000000dU;
            owner.birth=process.identity.birth;
        }
        if(prior.pid!=owner.pid || prior.device!=owner.device || prior.type!=owner.type || prior.birth!=owner.birth){
            do{n=::pwrite(file.fd,&owner,sizeof(owner),0);}while(n<0 && errno==EINTR);
            if(n!=sizeof(owner))throw HostCallFault("寫入 VidPn 擁有權失敗");
        }
        gui.kmt_vidpn_owner=owner.pid==self?owner.device:0;
        gui.kmt_vidpn_owner_type=owner.pid==self?owner.type:0;
        return status;
    }
#endif
    if(gui.kmt_vidpn_owner)owner={self,gui.kmt_vidpn_owner,gui.kmt_vidpn_owner_type,0,0};
    const auto status=update(owner);
    gui.kmt_vidpn_owner=owner.device;gui.kmt_vidpn_owner_type=owner.type;
    return status;
}
std::uint32_t clipboard_format_id(GuiStationNamespace& station, const std::string& directory,
                                  std::u16string_view station_name, std::u16string_view name,
                                  std::uint32_t id) {
    const auto fold = [](std::u16string_view text) {
        std::u16string key(text);
        for (auto& ch : key) ch = unicode_case_unit(ch, true);
        return key;
    };
    const auto key = fold(name);
    const auto lookup = [&]() -> std::uint32_t {
        if (name.empty()) return station.clipboard_format_names.contains(id) ? id : 0;
        const auto found = station.clipboard_formats.find(key);
        return found == station.clipboard_formats.end() ? 0 : found->second;
    };
    if (name.empty() && (id < 0xc000 || id > 0xffff)) return 0;
    if (const auto known = lookup()) return known;
    const auto remember = [&](std::u16string_view text) {
        const auto next = station.clipboard_next_format;
        station.clipboard_format_names.emplace(next, text);
        station.clipboard_formats.emplace(fold(text), next);
        ++station.clipboard_next_format;
        return next;
    };
#if !defined(_WIN32)
    if (!directory.empty()) {
        const auto station_key = fold(station_name);
        std::uint64_t hash = UINT64_C(14695981039346656037);
        for (auto ch : station_key) { hash ^= ch; hash *= UINT64_C(1099511628211); }
        SharedFd file{open_window_file(directory + "/clipboard-formats-v1-" + std::to_string(hash))};
        FileLock lock(file.fd);
        using Record = std::array<char16_t, 256>;
        struct stat info{};
        if (::fstat(file.fd, &info) || info.st_size > static_cast<off_t>(0x4001 * sizeof(Record)))
            throw HostCallFault("剪貼簿格式表大小無效");
        // 僅發布完整紀錄；中斷寫入留下的尾端由持鎖的下一次寫入覆蓋。
        const auto records = static_cast<std::size_t>(info.st_size) / sizeof(Record);
        const auto transfer = [&](Record& record, std::size_t index, bool write) {
            const auto offset = static_cast<off_t>(index * sizeof(Record));
            ssize_t count;
            do {
                count = write ? ::pwrite(file.fd, record.data(), sizeof(record), offset)
                              : ::pread(file.fd, record.data(), sizeof(record), offset);
            } while (count < 0 && errno == EINTR);
            if (count != sizeof(record)) throw HostCallFault("剪貼簿格式表讀寫失敗");
        };
        if (station_key.size() >= Record{}.size()) throw HostCallFault("剪貼簿視窗站名稱過長");
        Record header{};
        std::copy(station_key.begin(), station_key.end(), header.begin());
        if (!records) transfer(header, 0, true);
        else {
            Record existing{};
            transfer(existing, 0, false);
            if (existing != header) throw HostCallFault("剪貼簿格式表視窗站不符");
        }
        for (std::size_t i = 1 + station.clipboard_next_format - 0xc000; i < records; ++i) {
            Record slot{};
            transfer(slot, i, false);
            const auto end = std::find(slot.begin(), slot.end(), u'\0');
            // 一般名稱沿用零結尾紀錄。計長字串若內含零，末格保存其 UTF-16 長度。
            const auto length=slot.back()?static_cast<std::size_t>(slot.back()):static_cast<std::size_t>(end-slot.begin());
            if(!length || length>=slot.size())throw HostCallFault("無效的剪貼簿格式名稱");
            remember({slot.data(),length});
        }
        if (const auto known = lookup()) return known;
        if (name.empty() || station.clipboard_next_format > 0xffff) return 0;
        Record slot{};
        std::copy(name.begin(), name.end(), slot.begin());
        if(name.find(u'\0')!=name.npos)slot.back()=static_cast<char16_t>(name.size());
        transfer(slot, 1 + station.clipboard_next_format - 0xc000, true);
    }
#endif
    if (name.empty() || station.clipboard_next_format > 0xffff) return 0;
    return remember(name);
}
struct SharedWindowState {
    std::string directory;
    SharedWindowTable *table{};
    struct Order { std::uint32_t count; std::array<std::uint32_t,window_slots> handles; };
    Order *order{};
    // 多一個槽位，寫入途中若失去 producer，已發布的 64 筆資料仍完整。
    struct MouseHistory {std::uint64_t sequence;std::array<MouseMoveSample,65> samples;};
    MouseHistory* mouse_history{};
    // 獨立擴充表保留既有共用視窗槽位 ABI。每筆把 HWND 與編碼一起發布，槽位重用時仍可辨識擁有者。
    using ProcedureEncodings=std::array<std::uint32_t,window_slots>;
    ProcedureEncodings *encodings{};
    std::map<std::uint64_t, SharedWindowInfo> published;
    std::map<std::uint64_t, std::u16string> long_captions;
    std::map<std::uint64_t,std::u16string> base_classes;
    std::string class_path(std::uint64_t handle) const {return directory+"/baseclass-"+std::to_string(handle);}
    std::string caption_path(std::uint64_t handle) const {
        return directory + "/caption-" + std::to_string(handle);
    }
    std::map<std::uint64_t, std::uint32_t> frames;
    std::map<std::uint64_t, std::shared_ptr<RasterSurface>> surfaces;
    // 各影格表面建立時綁定的原生 view；view 晚於第一張影格出現或被重建時須改綁。
    std::map<std::uint64_t, void*> surface_views;
    // 各影格表面目前是否依 alpha 合成（所屬頂層視窗為 DWM 整片玻璃或 Chromium 透明視窗）。
    std::map<std::uint64_t, bool> surface_glass;
    // 依 alpha 合成的頂層視窗：影格自何時起持續完全透明，以及目前已讓滑鼠穿透者。
    std::map<std::uint64_t, std::chrono::steady_clock::time_point> transparent_since;
    std::set<std::uint64_t> input_transparent;
    // 重新顯示時暫緩揭露的頂層視窗：記下當時各跨程序子視窗的影格序號，送達新影格或逾時才揭露。
    struct RevealHold {std::chrono::steady_clock::time_point start,deadline;std::map<std::uint64_t,std::uint32_t> sequences;};
    std::map<std::uint64_t,RevealHold> reveals;
#if !defined(_WIN32)
    std::map<std::uint64_t,std::weak_ptr<FrameMapping>> mappings;
    std::uint64_t mapping_builds{},mapping_hits{};
    bool cache_frames{[] {const auto value=std::getenv("X64ARM_SHARED_FRAME_CACHE");return !value || std::string_view(value)!="0";}()};
    std::shared_ptr<FrameMapping> frame_mapping(std::uint64_t handle,int fd,std::size_t bytes,const struct stat& status){
        auto mapping=cache_frames?mappings[handle].lock():std::shared_ptr<FrameMapping>{};
        if(mapping && mapping->size==bytes && mapping->device==status.st_dev && mapping->inode==status.st_ino){++mapping_hits;return mapping;}
        mapping=std::make_shared<FrameMapping>(fd,bytes,status);++mapping_builds;
        if(cache_frames){
            // 強參照由仍在使用的 Metal binding 持有；這張表不延長已關閉視窗的 payload。
            if(mappings.size()>=64)std::erase_if(mappings,[](const auto& item){return item.second.expired();});
            mappings[handle]=mapping;
        }
        return mapping;
    }
#endif
    std::set<std::uint64_t> allocated;
    int descriptor{-1};
    std::recursive_mutex local_lock;
    unsigned lock_depth{};
    // Shell 註冊會在同一個交易內查詢及發布；flock 本身不記錄巢狀深度。
    void lock() {
        local_lock.lock();
#if !defined(_WIN32)
        if (!lock_depth && lock_file(descriptor, LOCK_EX)) {
            local_lock.unlock();
            throw HostCallFault("共用視窗表鎖失敗");
        }
#endif
        ++lock_depth;
    }
    void unlock() {
        --lock_depth;
#if !defined(_WIN32)
        if (!lock_depth)
            (void)lock_file(descriptor, LOCK_UN);
#endif
        local_lock.unlock();
    }
    ~SharedWindowState() {
#if !defined(_WIN32)
        if(std::getenv("X64ARM_SHARED_FRAME_TRACE"))std::fprintf(stderr,"[shared-frame-mapping] cache=%u builds=%llu hits=%llu\n",cache_frames?1U:0U,static_cast<unsigned long long>(mapping_builds),static_cast<unsigned long long>(mapping_hits));
        if (table) {
            const bool locked = (!long_captions.empty() || !base_classes.empty()) && !lock_file(descriptor, LOCK_EX);
            for (auto h : allocated) {
                auto &slot = table->slots[h % window_slots];
                if (__atomic_load_n(&slot.handle, __ATOMIC_ACQUIRE) == h)
                    __atomic_store_n(&slot.handle, 0U, __ATOMIC_RELEASE);
            }
            if (locked) {
                for (const auto& item : base_classes) (void)::unlink(class_path(item.first).c_str());
                for (const auto& item : long_captions) (void)::unlink(caption_path(item.first).c_str());
                (void)lock_file(descriptor, LOCK_UN);
            }
            ::munmap(table, sizeof(SharedWindowTable));
        }
        if(encodings)::munmap(encodings,sizeof(ProcedureEncodings));
        if(order)::munmap(order,sizeof(Order));
        if(mouse_history)::munmap(mouse_history,sizeof(MouseHistory));
        if (descriptor >= 0)
            ::close(descriptor);
#endif
    }
};
void WindowsServices::initialize_shared_windows() {
    if (shared_windows_ || !processes_ || !runtime_.network.external_io)
        return;
#if !defined(_WIN32)
    auto value = std::make_shared<SharedWindowState>();
    value->directory = mapping_session();
    value->descriptor = open_window_file(value->directory + "/windows");
    FileLock lock(value->descriptor);
    struct stat st{};
    if (::fstat(value->descriptor, &st))
        throw HostCallFault("無法讀取共用視窗表尺寸");
    const bool fresh = st.st_size == 0;
    if (fresh && ::ftruncate(value->descriptor, sizeof(SharedWindowTable)))
        throw HostCallFault("無法配置共用視窗表");
    if (!fresh && st.st_size != sizeof(SharedWindowTable))
        throw HostCallFault("共用視窗表版本不符");
    auto *address =
        ::mmap(nullptr, sizeof(SharedWindowTable), PROT_READ | PROT_WRITE, MAP_SHARED, value->descriptor, 0);
    if (address == MAP_FAILED)
        throw HostCallFault("無法映射共用視窗表");
    value->table = static_cast<SharedWindowTable *>(address);
    if (fresh) {
        value->table->version = 4;
        value->table->next = 0x10000;
        value->table->count = window_slots;
        value->table->magic = window_magic;
    }
    if (value->table->magic != window_magic || value->table->version != 4 || value->table->count != window_slots)
        throw HostCallFault("共用視窗表標頭不符");
    SharedFd order_file{open_window_file(value->directory+"/windows-order-v1")};
    struct stat order_stat{};
    const auto size=sizeof(SharedWindowState::Order);
    if(::fstat(order_file.fd,&order_stat) || (order_stat.st_size && order_stat.st_size!=static_cast<off_t>(size)) ||
       (!order_stat.st_size && ::ftruncate(order_file.fd,static_cast<off_t>(size))))
        throw HostCallFault("共用視窗順序表尺寸錯誤");
    auto *order=::mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,order_file.fd,0);
    if(order==MAP_FAILED)throw HostCallFault("無法映射共用視窗順序");
    value->order=static_cast<SharedWindowState::Order*>(order);
    SharedFd encoding_file{open_window_file(value->directory+"/window-encodings-v1")};
    struct stat encoding_stat{};
    const auto bytes=sizeof(SharedWindowState::ProcedureEncodings);
    if(::fstat(encoding_file.fd,&encoding_stat) ||
       (encoding_stat.st_size ? encoding_stat.st_size!=static_cast<off_t>(bytes) : ::ftruncate(encoding_file.fd,bytes)!=0))
        throw HostCallFault("共用視窗編碼表尺寸錯誤");
    auto* encodings=::mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,encoding_file.fd,0);
    if(encodings==MAP_FAILED)throw HostCallFault("無法映射共用視窗編碼表");
    value->encodings=static_cast<SharedWindowState::ProcedureEncodings*>(encodings);
    SharedFd history_file{open_window_file(value->directory+"/mouse-history-v1")};
    constexpr auto history_size=sizeof(SharedWindowState::MouseHistory);
    struct stat history_stat{};
    if(::fstat(history_file.fd,&history_stat) || (history_stat.st_size ? history_stat.st_size!=history_size :
       ::ftruncate(history_file.fd,history_size)!=0))throw HostCallFault("共用滑鼠歷程尺寸錯誤");
    auto* history=::mmap(nullptr,history_size,PROT_READ|PROT_WRITE,MAP_SHARED,history_file.fd,0);
    if(history==MAP_FAILED)throw HostCallFault("無法映射共用滑鼠歷程");
    value->mouse_history=static_cast<SharedWindowState::MouseHistory*>(history);
    shared_windows_ = std::move(value);
#endif
}
void append_shared_mouse_move(SharedWindowState& shared,const MouseMoveSample& sample){
    std::lock_guard lock(shared);
    auto& history=*shared.mouse_history;
    const auto n=history.sequence;
    history.samples[n%history.samples.size()]=sample;
    __atomic_store_n(&history.sequence,n+1,__ATOMIC_RELEASE);
}
std::vector<MouseMoveSample> shared_mouse_moves(SharedWindowState& shared){
    std::lock_guard lock(shared);
    const auto& history=*shared.mouse_history;
    const auto end=history.sequence,count=std::min<std::uint64_t>(end,64);
    std::vector<MouseMoveSample> samples;
    samples.reserve(count);
    for(auto i=end-count;i<end;++i)samples.push_back(history.samples[i%history.samples.size()]);
    return samples;
}
#include "windows_clipboard_notifications.inc"
std::vector<std::uint64_t> snapshot_window_order(SharedWindowState* shared,const std::vector<std::uint64_t>& local){
    if(!shared || !shared->order)return local;
    std::lock_guard lock(*shared);
    const auto& order=*shared->order;
    if(order.count>window_slots)throw HostCallFault("共用視窗順序表超出範圍");
    std::vector<std::uint64_t> handles(order.handles.begin(),order.handles.begin()+order.count);
    for(const auto h:local)if(!shared->allocated.contains(h))handles.push_back(h);
    return handles;
}
void publish_window_order(SharedWindowState* shared,std::uint64_t window,std::uint64_t after){
    if(!shared || !shared->order || !shared->allocated.contains(window))return;
    std::lock_guard lock(*shared);
    auto& order=*shared->order;
    if(order.count>window_slots)throw HostCallFault("共用視窗順序表超出範圍");
    auto begin=order.handles.begin(),end=begin+order.count;
    // 重新使用槽位時，同時回收已結束程序留下的舊 handle。
    end=std::remove_if(begin,end,[&](auto h){return h%window_slots==window%window_slots;});
    if(end==order.handles.end())throw HostCallFault("共用視窗順序表已滿");
    auto pos=after==0?begin:end;
    if(after>1){pos=std::find(begin,end,after);if(pos!=end)++pos;}
    std::move_backward(pos,end,end+1);
    *pos=static_cast<std::uint32_t>(window);
    order.count=static_cast<std::uint32_t>(end+1-begin);
}
std::uint64_t WindowsServices::allocate_gui_window() {
    initialize_shared_windows();
    if (!shared_windows_)
        return gui_.next_window++;
#if !defined(_WIN32)
    auto &shared = *shared_windows_;
    std::lock_guard lock(shared);
    for (unsigned attempt = 0; attempt < window_slots; ++attempt) {
        const auto h = shared.table->next++;
        if (h >= 0x0fffffff)
            throw std::bad_alloc{};
        auto &slot = shared.table->slots[h % window_slots];
        const auto previous = __atomic_load_n(&slot.handle, __ATOMIC_ACQUIRE),
                   owner = __atomic_load_n(&slot.owner, __ATOMIC_ACQUIRE);
        if (previous && (owner == process_id() || remote_process_available(owner)))
            continue;
        shared.allocated.insert(h);
        __atomic_store_n(&slot.handle, 0U, __ATOMIC_RELEASE);
        __atomic_store_n(&slot.sequence, 0U, __ATOMIC_RELEASE);
        __atomic_store_n(&slot.frame, 0U, __ATOMIC_RELEASE);
        __atomic_store_n(&slot.owner, process_id(), __ATOMIC_RELEASE);
        __atomic_store_n(&slot.handle, h, __ATOMIC_RELEASE);
        gui_.next_window = shared.table->next;
        return h;
    }
#endif
    throw std::bad_alloc{};
}
SharedWindowInfo WindowsServices::describe_gui_window(std::uint64_t handle) {
    const auto &w = gui_.windows.at(handle);
    SharedWindowInfo info{};
    info.handle = handle;
    info.process = process_id();
    info.thread = w.thread;
    info.parent = w.parent;
    info.owner = w.owner;
    info.instance = w.instance;
    info.id = gui_menu_or_control_id(w);
    info.desktop = w.desktop_object;
    info.job = current_job_identity();
    info.class_atom = static_cast<std::uint32_t>(w.class_atom);
    info.active = gui_.active == handle;
    info.shell_desktop = w.shell_desktop;
    info.display_affinity = w.display_affinity;
    info.width = w.width;
    info.height = w.height;
    const auto outer = gui_outer_size(w);
    info.outer_width = outer[0];
    info.outer_height = outer[1];
    info.style = w.style;
    info.ex_style = w.ex_style;
    info.visible = w.visible;
    for (auto parent = w.parent; info.visible && parent && parent != gui_message_parent;) {
        const auto ancestor = gui_.windows.find(parent);
        if (ancestor == gui_.windows.end()) {
            info.visible = false;
            break;
        }
        info.visible = ancestor->second.visible;
        parent = ancestor->second.parent;
    }
    info.enabled = w.enabled;
    info.show = w.show_command;
    const auto origin = gui_client_origin(gui_, handle);
    const auto inset = gui_frame_insets(w);
    info.x = static_cast<std::int32_t>(origin[0] - inset.left);
    info.y = static_cast<std::int32_t>(origin[1] - inset.top);
    info.client_x = static_cast<std::int32_t>(origin[0]);
    info.client_y = static_cast<std::int32_t>(origin[1]);
    const auto cls=window_class_name(w,!w.class_atom || gui_.system_classes.contains(w.class_name),false);
    std::copy_n(cls.begin(), std::min(cls.size(), info.class_name.size() - 1),
                info.class_name.begin());
    std::copy_n(w.title.begin(), std::min(w.title.size(), info.title.size() - 1), info.title.begin());
    return info;
}
void WindowsServices::publish_gui_window(std::uint64_t handle) {
    if (!shared_windows_ || !gui_.windows.contains(handle))
        return;
    auto &shared = *shared_windows_;
    if (!shared.allocated.contains(handle))
        return;
    const auto info = describe_gui_window(handle);
    const auto& title = gui_.windows.at(handle).title;
    const bool overflow = title.size() >= info.title.size();
    const auto& w=gui_.windows.at(handle);
    const auto base=window_class_name(w,!w.class_atom || gui_.system_classes.contains(w.class_name),true);
    const bool distinct=base!=std::u16string_view(info.class_name.data());
    auto& encoding=(*shared.encodings)[handle%window_slots];
    const auto tagged=(static_cast<std::uint32_t>(handle)<<1) | static_cast<unsigned>(w.unicode);
    if(__atomic_load_n(&encoding,__ATOMIC_RELAXED)!=tagged)__atomic_store_n(&encoding,tagged,__ATOMIC_RELEASE);
    const auto old_base=shared.base_classes.find(handle);
    const bool base_changed=distinct?(old_base==shared.base_classes.end() || old_base->second!=base):old_base!=shared.base_classes.end();
    const auto previous = shared.long_captions.find(handle);
    const bool changed = overflow ? previous == shared.long_captions.end() || previous->second != title
                                  : previous != shared.long_captions.end();
    if (const auto old = shared.published.find(handle); !changed && !base_changed && old != shared.published.end() && old->second == info)
        return;
    // 只在欄位有變化時發布；結構尾端 padding 不屬於視窗狀態。
    std::lock_guard lock(shared);
#if !defined(_WIN32)
    if(base_changed){
        const auto path=shared.class_path(handle);
        if(distinct){
            const auto staging=path+".new";
            SharedFd file{open_window_file(staging)};
            const auto bytes=base.size()*sizeof(char16_t);
            ssize_t written;
            do { written=::pwrite(file.fd,base.data(),bytes,0); } while(written<0 && errno==EINTR);
            if(written!=static_cast<ssize_t>(bytes) || ::ftruncate(file.fd,static_cast<off_t>(bytes)) ||
               ::rename(staging.c_str(),path.c_str())){
                (void)::unlink(staging.c_str());
                throw HostCallFault("無法發布視窗基底類別");
            }
            shared.base_classes[handle]=base;
        }else{
            if(::unlink(path.c_str()) && errno!=ENOENT)throw HostCallFault("無法移除視窗基底類別");
            shared.base_classes.erase(old_base);
        }
    }
    if (changed) {
        const auto path = shared.caption_path(handle);
        if (overflow) {
            const auto staging = path + ".new";
            SharedFd file{open_window_file(staging)};
            const auto bytes = title.size() * sizeof(char16_t);
            ssize_t written;
            do { written = ::pwrite(file.fd, title.data(), bytes, 0); } while (written < 0 && errno == EINTR);
            if (written != static_cast<ssize_t>(bytes) || ::ftruncate(file.fd, static_cast<off_t>(bytes)) ||
                ::rename(staging.c_str(), path.c_str())) {
                (void)::unlink(staging.c_str());
                throw HostCallFault("無法發布跨程序視窗標題");
            }
            shared.long_captions[handle] = title;
        } else {
            if (::unlink(path.c_str()) && errno != ENOENT) throw HostCallFault("無法移除跨程序視窗標題");
            shared.long_captions.erase(previous);
        }
    }
#endif
    shared.published[handle] = info;
    auto &slot = shared.table->slots[handle % window_slots];
    std::array<std::uint32_t, sizeof(info) / 4> words{};
    std::memcpy(words.data(), &info, sizeof(info));
    const auto sequence = __atomic_load_n(&slot.sequence, __ATOMIC_RELAXED);
    (void)__atomic_add_fetch(&slot.sequence, 1U, __ATOMIC_ACQ_REL);
    for (unsigned i = 0; i < words.size(); ++i)
        __atomic_store_n(&slot.info[i], words[i], __ATOMIC_RELAXED);
    __atomic_store_n(&slot.sequence, sequence + 2, __ATOMIC_RELEASE);
}
void WindowsServices::unpublish_gui_window(std::uint64_t handle) {
    if (!shared_windows_)
        return;
    auto &shared = *shared_windows_;
    std::lock_guard lock(shared);
    auto &slot = shared.table->slots[handle % window_slots];
    if (__atomic_load_n(&slot.handle, __ATOMIC_ACQUIRE) == handle &&
        __atomic_load_n(&slot.owner, __ATOMIC_ACQUIRE) == process_id())
        __atomic_store_n(&slot.handle, 0U, __ATOMIC_RELEASE);
#if !defined(_WIN32)
    if(shared.base_classes.erase(handle))(void)::unlink(shared.class_path(handle).c_str());
    if (shared.long_captions.erase(handle)) (void)::unlink(shared.caption_path(handle).c_str());
#endif
    shared.published.erase(handle);
    shared.allocated.erase(handle);
    shared.frames.erase(handle);
    shared.surfaces.erase(handle);
    shared.surface_views.erase(handle);
    shared.surface_glass.erase(handle);
    shared.input_transparent.erase(handle);
    shared.transparent_since.erase(handle);
#if !defined(_WIN32)
    shared.mappings.erase(handle);
#endif
}
std::optional<SharedWindowInfo> WindowsServices::shared_gui_window(std::uint64_t handle, bool enforce_job) {
    initialize_shared_windows();
    if (!shared_windows_ || handle < 0x10000 || handle >= 0x0fffffff)
        return std::nullopt;
    if (gui_.windows.contains(handle))
        publish_gui_window(handle);
    auto &slot = shared_windows_->table->slots[handle % window_slots];
    if (__atomic_load_n(&slot.handle, __ATOMIC_ACQUIRE) != handle)
        return std::nullopt;
    const auto owner = __atomic_load_n(&slot.owner, __ATOMIC_ACQUIRE);
    if (owner != process_id() && !remote_process_available(owner))
        return std::nullopt;
    const auto snapshot = [&]() -> std::optional<SharedWindowInfo> {
        const auto before = __atomic_load_n(&slot.sequence, __ATOMIC_ACQUIRE);
        if (!before || (before & 1))
            return std::nullopt;
        std::array<std::uint32_t, sizeof(SharedWindowInfo) / 4> words{};
        for (unsigned i = 0; i < words.size(); ++i)
            words[i] = __atomic_load_n(&slot.info[i], __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (before != __atomic_load_n(&slot.sequence, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&slot.handle, __ATOMIC_ACQUIRE) != handle ||
            __atomic_load_n(&slot.owner, __ATOMIC_ACQUIRE) != owner)
            return std::nullopt;
        SharedWindowInfo info{};
        std::memcpy(&info, words.data(), sizeof(info));
        if (info.handle != handle || info.process != owner)
            return std::nullopt;
        return info;
    };
    std::optional<SharedWindowInfo> info;
    for (unsigned retry = 0; retry < 4 && !info; ++retry)
        info = snapshot();
    if (!info) {
        // 暫時的 seqlock 衝突不是無效 HWND。與發布者同步後只讀一次完整狀態；
        // 發布者異常退出時作業系統會釋放 flock，不會無限忙等奇數版本。
        std::lock_guard lock(*shared_windows_);
        info = snapshot();
        if (std::getenv("X64ARM_TRACE_GRAPHICS_ERRORS"))
            std::fprintf(stderr, "[shared_window_retry] pid=%u hwnd=%llu valid=%u\n", process_id(),
                         static_cast<unsigned long long>(handle), info ? 1U : 0U);
    }
    if (info && enforce_job && !allowed_gui_window(*info)) {
        last_error_ = 5;
        return std::nullopt;
    }
    return info;
}
std::uint64_t WindowsServices::query_shell_window() {
    const auto desktop = thread_desktop_object(threads_.current);
    for (const auto &[handle, w] : gui_.windows)
        if (w.shell_desktop && w.desktop_object == desktop)
            return handle;
    initialize_shared_windows();
    if (!shared_windows_)
        return 0;
    // Shell 身分由註冊狀態決定；不把一般桌面或同名視窗冒充 Shell。
    for (auto &slot : shared_windows_->table->slots) {
        const auto handle = __atomic_load_n(&slot.handle, __ATOMIC_ACQUIRE);
        if (!handle)
            continue;
        const auto info = shared_gui_window(handle);
        if (info && info->shell_desktop && info->desktop == desktop)
            return handle;
    }
    return 0;
}
std::uint64_t WindowsServices::desktop_manager_window(bool taskman,std::optional<std::uint64_t> replacement){
    initialize_user_objects();
    initialize_shared_windows();
    const auto desktop=thread_desktop_object(threads_.current);
    const unsigned role=taskman?1U:0U;
    const auto valid=[&](std::uint64_t h){
        if(!h)return false;
        if(const auto w=gui_.windows.find(h);w!=gui_.windows.end())return w->second.desktop_object==desktop;
        if(desktop!=2)return false;
        const auto remote=shared_gui_window(h,false);return remote && remote->desktop==desktop;
    };
    const auto apply=[&](std::uint64_t& slot){
        if(!valid(slot))slot=0;
        if(!replacement)return slot;
        if(*replacement && slot){last_error_=5;return UINT64_C(0);}
        if(*replacement && !valid(*replacement)){last_error_=1400;return UINT64_C(0);}
        slot=*replacement;return slot;
    };
#if !defined(_WIN32)
    // 初始桌面跨程序共用；其他桌面的物件身分由本程序的視窗站表管理。
    if(shared_windows_ && desktop==2){
        std::lock_guard lock(*shared_windows_);
        SharedFd file{open_window_file(shared_windows_->directory+"/desktop-managers-v1")};
        const auto offset=static_cast<off_t>(role*sizeof(std::uint64_t));
        std::uint64_t slot=0;
        ssize_t n;
        do{n=::pread(file.fd,&slot,sizeof(slot),offset);}while(n<0 && errno==EINTR);
        if(n!=0 && n!=sizeof(slot))throw HostCallFault("讀取桌面管理視窗失敗");
        const auto old=slot,value=apply(slot);
        if(slot!=old){
            do{n=::pwrite(file.fd,&slot,sizeof(slot),offset);}while(n<0 && errno==EINTR);
            if(n!=sizeof(slot))throw HostCallFault("寫入桌面管理視窗失敗");
        }
        return value;
    }
#endif
    return apply(gui_.user_objects.at(desktop).manager_windows[role]);
}
bool WindowsServices::register_shell_window(std::uint64_t handle, std::uint64_t view) {
    const auto found = gui_.windows.find(handle);
    const auto desktop = thread_desktop_object(threads_.current);
    if (found == gui_.windows.end()) {
        last_error_ = 1400;
        return false;
    }
    auto &window = found->second;
    if (window.parent || (window.style & 0x40000000U) || (window.ex_style & 8U) || window.desktop_object != desktop) {
        last_error_ = 87;
        return false;
    }
    if (view) {
        const auto child = gui_.windows.find(view);
        if (child == gui_.windows.end() || child->second.desktop_object != desktop) {
            last_error_ = 1400;
            return false;
        }
        if(child->second.ex_style&8U){last_error_=87;return false;}
    }
    initialize_shared_windows();
    const auto install = [&] {
        if (query_shell_window()) {
            last_error_ = 5;
            return false;
        }
        window.shell_desktop = true;
        publish_gui_window(handle);
        std::erase(gui_.z_order, handle);
        gui_.z_order.push_back(handle);
        publish_window_order(shared_windows_.get(),handle,1);
        if (native_gui_)
            native_gui_->reorder(handle, 1);
        return true;
    };
#if !defined(_WIN32)
    if (shared_windows_) {
        std::lock_guard lock(*shared_windows_);
        return install();
    }
#endif
    return install();
}
namespace {
// 呼叫端讀取溢出的標題前持有 shared 鎖並重新取得槽位快照。
std::u16string shared_caption(const SharedWindowState& shared,const SharedWindowInfo& w,std::size_t limit) {
    std::u16string value(w.title.data());
#if !defined(_WIN32)
        if (value.size() == w.title.size() - 1) {
            SharedFd file{::open(shared.caption_path(w.handle).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
            if (file.fd >= 0) {
                struct stat st{};
                if (::fstat(file.fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() ||
                    (st.st_mode & 077) || st.st_nlink != 1 || st.st_size < 0 ||
                    st.st_size % static_cast<off_t>(sizeof(char16_t)) || static_cast<std::uint64_t>(st.st_size) > limit)
                    throw HostCallFault("跨程序視窗標題檔案無效");
                const auto bytes = static_cast<std::size_t>(st.st_size);
                value.resize(bytes / sizeof(char16_t));
                ssize_t count;
                do { count = ::pread(file.fd, value.data(), bytes, 0); } while (count < 0 && errno == EINTR);
                if (count != st.st_size) throw HostCallFault("跨程序視窗標題不完整");
            } else if (errno != ENOENT) throw HostCallFault("無法讀取跨程序視窗標題");
        }
#endif
    return value;
}
}

std::uint64_t WindowsServices::find_gui_window(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol) {
    const bool native=symbol=="NtUserFindWindowEx", extended=native || symbol.starts_with("FindWindowEx");
    const auto requested=extended?arg(0):0, after=extended?arg(1):0;
    const auto message_parent=x86_?UINT64_C(0xfffffffd):gui_message_parent;
    const auto parent=requested==gui_.desktop?0:requested==message_parent?gui_message_parent:requested;
    std::optional<std::u16string> cls, caption;
    std::uint64_t atom=0;
    const auto read_key=[&](std::uint64_t p,bool is_class,std::optional<std::u16string>& key){
        if(!p)return true;
        if(native){
            const auto length=memory.load(GuestAddress{p},16);
            const auto buffer=memory.load(GuestAddress{p+(x86_?4:8)},x86_?32:64);
            if(is_class && buffer<=65535){atom=buffer;return true;}
            if((length&1) || (length && !buffer))return false;
            key=read_utf16_counted(memory,GuestAddress{buffer},length/2);
        }else if(is_class && p<=65535)atom=p;
        else key=symbol.ends_with('W')?read_utf16(memory,GuestAddress{p}):ansi_to_utf16_lossy(read_ansi(memory,p));
        return true;
    };
    if(!read_key(arg(extended?2:0),true,cls) || !read_key(arg(extended?3:1),false,caption) || (cls && cls->empty()))return 0;
    const auto equal=[](std::u16string_view a,std::u16string_view b){
        return std::equal(a.begin(),a.end(),b.begin(),b.end(),[](char16_t x,char16_t y){
            return x==y || unicode_case_unit(x,true)==unicode_case_unit(y,true);
        });
    };
    const auto desktop=thread_desktop_object(threads_.current);
    const auto parent_of=[&](std::uint64_t handle)->std::optional<std::uint64_t>{
        if(const auto w=gui_.windows.find(handle);w!=gui_.windows.end())return w->second.parent;
        if(const auto w=shared_gui_window(handle))return w->parent;
        return {};
    };
    if((parent && parent!=gui_message_parent && !parent_of(parent)) || (after && parent_of(after)!=parent)){
        last_error_=1400;return 0;
    }
    initialize_shared_windows();
    const auto handles=snapshot_window_order(shared_windows_.get(),gui_.z_order);
    const auto find_child=[&](std::uint64_t target)->std::uint64_t{
        bool seen=!after;
        for(const auto h:handles){
            if(!seen){seen=h==after;continue;}
            if(h==gui_.desktop)continue;
            if(const auto iter=gui_.windows.find(h);iter!=gui_.windows.end()){
                const auto& w=iter->second;
                if(w.desktop_object==desktop && w.parent==target && (!atom || w.class_atom==atom) &&
                   (!cls || equal(w.class_name,*cls)) && (!caption || equal(w.title,*caption)))return h;
                continue;
            }
            auto w=shared_gui_window(h,false);
            if(!w || !allowed_gui_window(*w) || w->desktop!=desktop || w->parent!=target ||
               (atom && w->class_atom!=atom) || (cls && !equal(w->class_name.data(),*cls)))continue;
            if(!caption)return h;
            if(w->title[w->title.size()-2]){
                std::lock_guard lock(*shared_windows_);
                w=shared_gui_window(h,false);
                if(w && allowed_gui_window(*w) && w->parent==target && w->desktop==desktop &&
                   equal(shared_caption(*shared_windows_,*w,output_limit),*caption))return h;
            }else if(equal(w->title.data(),*caption))return h;
        }
        return 0;
    };
    if(const auto h=find_child(parent))return h;
    return !requested && !after?find_child(gui_message_parent):0;
}

bool WindowsServices::invoke_remote_gui(GuestState &state, GuestMemory &memory,
                                        const WindowsServiceDescriptor &service) {
    if (!processes_ || (service.family != "user32.dll" && service.symbol!="NtUserGetClassName") || !service.arguments)
        return false;
    const auto symbol = service.symbol;
    const bool class_query=symbol.starts_with("GetClassName") || symbol.starts_with("RealGetWindowClass") || symbol=="NtUserGetClassName";
    const bool caption = symbol.starts_with("GetWindowText") || symbol == "InternalGetWindowText";
    const bool supported =
        symbol == "IsWindow" || symbol == "IsWindowUnicode" || symbol == "IsWindowVisible" || symbol == "IsWindowEnabled" || symbol == "IsIconic" ||
        symbol == "IsZoomed" || symbol == "GetWindowThreadProcessId" || symbol == "GetClientRect" ||
        symbol == "GetWindowRect" || symbol == "GetDC" ||
        symbol == "GetWindowDC" || symbol == "ReleaseDC" || symbol == "WindowFromDC" || symbol == "ClientToScreen" ||
        symbol == "ScreenToClient" || symbol.starts_with("GetWindowLong") || symbol.starts_with("SetWindowLong") ||
        class_query || caption ||
        symbol=="GetWindowDisplayAffinity" || symbol=="SetWindowDisplayAffinity";
    if (!supported)
        return false;
    const WindowsCall arg(state, memory, service);
    const auto handle = symbol == "WindowFromDC" ? (arg(0) >= 0x10000000 ? arg(0) - 0x10000000 : 0) : arg(0);
    if (gui_.windows.contains(handle))
        return false;
    auto found = shared_gui_window(handle, false);
    std::unique_lock<SharedWindowState> caption_lock;
    const bool real=class_query && (symbol.starts_with("Real") || (symbol=="NtUserGetClassName" && arg(1)));
    if (found && (real || (caption && found->title[found->title.size() - 2]))) {
        caption_lock = std::unique_lock(*shared_windows_);
        found = shared_gui_window(handle, false);
    }
    if (!found)
        return false;
    const auto &w = *found;
    std::uint64_t result = 0;
    if (!allowed_gui_window(w)) {
        last_error_ = 5;
        arg.return_to(state, 0);
        return true;
    }
    if(class_query){
        std::u16string name=w.class_name.data();
#if !defined(_WIN32)
        if(real){
            SharedFd file{::open(shared_windows_->class_path(handle).c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};
            if(file.fd>=0){
                struct stat st{};
                if(::fstat(file.fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=::geteuid() || (st.st_mode&077) ||
                   st.st_nlink!=1 || st.st_size<=0 || st.st_size>510 || st.st_size%2)
                    throw HostCallFault("基底類別資料無效");
                name.resize(static_cast<std::size_t>(st.st_size)/2);
                ssize_t count;
                do { count=::pread(file.fd,name.data(),name.size()*2,0); } while(count<0 && errno==EINTR);
                if(count!=st.st_size)throw HostCallFault("基底類別資料不完整");
            }else if(errno!=ENOENT)throw HostCallFault("無法讀取基底類別");
        }
#endif
        if(caption_lock.owns_lock())caption_lock.unlock();
        result=write_window_class_name(memory,arg,symbol,x86_,name,last_error_);
    }else if(symbol=="SetWindowDisplayAffinity")last_error_=5;
    else if(symbol=="GetWindowDisplayAffinity"){
        if(!arg(1) || (w.style&0x40000000U))last_error_=87;
        else {const auto affinity=w.display_affinity==1 || w.display_affinity==0x11?w.display_affinity:0U;memory.store(GuestAddress{arg(1)},32,affinity);result=1;}
    }
    else if (symbol == "IsWindow")
        result = 1;
    else if (symbol == "IsWindowVisible")
        result = w.visible;
    else if (symbol == "IsWindowUnicode") {
        const auto tagged=__atomic_load_n(&(*shared_windows_->encodings)[handle%window_slots],__ATOMIC_ACQUIRE);
        result=(tagged>>1)==handle && (tagged&1);
    }
    else if (symbol == "IsWindowEnabled")
        result = w.enabled;
    else if (symbol == "IsIconic")
        result = w.show == 2;
    else if (symbol == "IsZoomed")
        result = w.show == 3;
    else if (symbol == "GetWindowThreadProcessId") {
        result = w.thread;
        if (arg(1))
            memory.store(GuestAddress{arg(1)}, 32, w.process);
    } else if (symbol == "GetClientRect" || symbol == "GetWindowRect") {
        const bool client = symbol == "GetClientRect";
        const auto x = client ? 0 : w.x, y = client ? 0 : w.y;
        memory.store(GuestAddress{arg(1)}, 32, static_cast<std::uint32_t>(x));
        memory.store(GuestAddress{arg(1) + 4}, 32, static_cast<std::uint32_t>(y));
        memory.store(GuestAddress{arg(1) + 8}, 32, static_cast<std::uint32_t>(x) + (client ? w.width : w.outer_width));
        memory.store(GuestAddress{arg(1) + 12}, 32,
                     static_cast<std::uint32_t>(y) + (client ? w.height : w.outer_height));
        result = 1;
    } else if (symbol == "ClientToScreen" || symbol == "ScreenToClient") {
        const auto sign = symbol == "ClientToScreen" ? 1 : -1;
        for (unsigned i = 0; i < 2; ++i) {
            const auto at = arg(1) + 4 * i;
            const auto value = memory.load(GuestAddress{at}, 32);
            memory.store(GuestAddress{at}, 32,
                         static_cast<std::uint32_t>(value) +
                             static_cast<std::uint32_t>(std::int64_t{sign} * (i ? w.client_y : w.client_x)));
        }
        result = 1;
    } else if (symbol == "GetDC" || symbol == "GetWindowDC") {
        result = 0x10000000 + handle;
        gui_.dc_objects.try_emplace(result, 0xb011);
        auto& dc=gui_.contexts[result];dc=GuiDc{};dc.display=true;
        if(symbol=="GetWindowDC"){
            dc.window_frame=true;dc.viewport_origin={w.x-w.client_x,w.y-w.client_y};
        }
    } else if (symbol == "ReleaseDC") {
        if (arg(1) == 0x10000000 + handle){
            result = gui_.dc_objects.erase(arg(1)) ? 1 : 0;
            if(result){gui_.contexts.erase(arg(1));gui_.saved_contexts.erase(arg(1));}
        }
    } else if (symbol == "WindowFromDC") {
        if (gui_.dc_objects.contains(arg(0)))
            result = handle;
    } else if (symbol.starts_with("SetWindowLong"))
        last_error_ = 5;
    else if (symbol.starts_with("GetWindowLong")) {
        switch (static_cast<std::int32_t>(arg(1))) {
        case -16:
            result = w.style;
            break;
        case -20:
            result = w.ex_style;
            break;
        case -6:
            result = w.instance;
            break;
        case -12:
            result = w.id;
            break;
        case -8:
            result = w.parent ? w.parent : w.owner;
            break;
        case -4:
            last_error_ = 5;
            break;
        default:
            last_error_ = 1413;
        }
    } else {
        const bool wide = symbol.ends_with('W') || symbol == "InternalGetWindowText",
                   length = symbol.starts_with("GetWindowTextLength");
        auto value = shared_caption(*shared_windows_,w,output_limit);
        if (caption_lock.owns_lock()) caption_lock.unlock();
        if (length)
            result = wide ? value.size() : utf16_to_utf8(value).size();
        else if (arg(1) && static_cast<std::int32_t>(arg(2)) > 0) {
            const auto capacity = static_cast<std::uint32_t>(arg(2));
            if (wide) {
                result = std::min<std::size_t>(value.size(), capacity - 1);
                memory.write(GuestAddress{arg(1)}, utf16_bytes(value.substr(0, result)));
            } else {
                const auto text = utf16_to_utf8(value);
                std::vector<std::uint8_t> bytes(text.begin(), text.end());
                result = std::min<std::size_t>(bytes.size(), capacity - 1);
                while (result && result < bytes.size() && (bytes[result] & 0xc0) == 0x80) --result;
                bytes.resize(result);
                bytes.push_back(0);
                memory.write(GuestAddress{arg(1)}, bytes);
            }
        }
    }
    arg.return_to(state, result);
    return true;
}

void WindowsServices::publish_remote_frame(std::uint64_t handle, NativeGraphics& graphics, const GraphicsView& view) {
    const auto& desc=view.resource->desc;
    publish_shared_frame(handle,desc.width,desc.height,desc.format,[&](void* data,std::size_t size,std::uint32_t stride,const std::shared_ptr<void>& cache){
        graphics.graphics_export_frame(view,data,size,stride,cache);});
}
// 其他程序視窗上的 ANGLE（EGL）表面：CPU 端讀回的緊密 RGBA8 列（由上而下）寫入同一份共用影格，由視窗擁有者匯入呈現。
void WindowsServices::publish_remote_pixels(std::uint64_t handle,std::uint32_t width,std::uint32_t height,std::span<const std::uint8_t> rgba) {
    const auto row=std::size_t{width}*4;
    if(rgba.size()<row*height)throw HostCallFault("遠端呈現像素不足");
    publish_shared_frame(handle,width,height,28,[&](void* data,std::size_t size,std::uint32_t stride,const std::shared_ptr<void>&){
        auto* out=static_cast<std::uint8_t*>(data);
        for(std::uint32_t y=0;y<height && std::size_t{y}*stride+row<=size;++y)std::memcpy(out+std::size_t{y}*stride,rgba.data()+std::size_t{y}*row,row);});
}
void WindowsServices::publish_shared_frame(std::uint64_t handle,std::uint32_t width,std::uint32_t height,std::uint32_t format,
                                           const std::function<void(void*,std::size_t,std::uint32_t,const std::shared_ptr<void>&)>& write) {
    const auto window = shared_gui_window(handle);
    if (!window || window->process == process_id())
        throw WindowPresentationFault("遠端呈現 HWND 已關閉或無效");
    const auto size = std::uint64_t{width} * height * 4;
    if (!width || !height || width > 16384 || height > 16384 || size > 128 * 1024 * 1024 ||
        (format != 28 && format != 29 && format != 87 && format != 91))
        throw HostCallFault("遠端呈現像素格式或範圍無效");
#if !defined(_WIN32)
    auto &shared = *shared_windows_;
    SharedFd file{open_window_file(shared.directory + "/frame-" + std::to_string(handle))};
    FileLock lock(file.fd);
    // 256-byte row stride 同時滿足 Metal buffer blit 與 linear texture 對齊。
    const auto stride=(width*4+255U)&~255U;
    FrameHeader header{window_magic,width,height,format,2,stride,frame_bytes(stride,height)};
    if(header.bytes>128*1024*1024)throw HostCallFault("共用影格對齊後大小超限");
    struct stat status{};
    const auto file_size=static_cast<off_t>(static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE))+header.bytes);
    if(::fstat(file.fd,&status) || (status.st_size<file_size && ::ftruncate(file.fd,file_size)))
        throw HostCallFault("共用影格配置失敗");
    const auto mapping=shared.frame_mapping(handle,file.fd,static_cast<std::size_t>(header.bytes),status);
    const auto export_begin=trace_shared_frames()?trace_unix_us():0;
    write(mapping->data,mapping->size,stride,shared.cache_frames?mapping:std::shared_ptr<void>{});
    if (::pwrite(file.fd, &header, sizeof(header), 0) != sizeof(header))
        throw HostCallFault("共用影格標頭寫入失敗");
    auto &slot = shared.table->slots[handle % window_slots];
    if (__atomic_load_n(&slot.handle, __ATOMIC_ACQUIRE) != handle ||
        __atomic_load_n(&slot.owner, __ATOMIC_ACQUIRE) != window->process)
        throw WindowPresentationFault("遠端呈現期間 HWND 已關閉");
    const auto sequence=__atomic_add_fetch(&slot.frame, 1U, __ATOMIC_RELEASE);
    record_diagnostic("remote_frame_published");
    {static std::int64_t last=0,gap=0;record_frame_timing("remote_frame_first_published","remote_frame_max_publish_gap",last,gap);}
    if(trace_shared_frames()){const auto now=trace_unix_us();
        std::fprintf(stderr,"shared_frame kind=publish pid=%d handle=%llu seq=%u unix_us=%lld export_us=%lld\n",static_cast<int>(::getpid()),
            static_cast<unsigned long long>(handle),sequence,now,now-export_begin);}
#else
    throw HostCallFault("此平台缺少共用視窗呈現");
#endif
}
// 依格點抽樣判斷影格是否完全透明（RGBA／BGRA 的 alpha 皆在第 4 byte）；遇到不透明像素立即返回。
static bool frame_fully_transparent(const void *data, std::size_t size, std::uint32_t width, std::uint32_t height, std::uint32_t stride) {
    const auto *pixels = static_cast<const std::uint8_t *>(data);
    const auto step_x = std::max<std::uint32_t>(1, width / 48), step_y = std::max<std::uint32_t>(1, height / 48);
    for (auto y = step_y / 2; y < height; y += step_y)
        for (auto x = step_x / 2; x < width; x += step_x)
            if (const auto at = std::size_t{y} * stride + std::size_t{x} * 4 + 3; at < size && pixels[at])
                return false;
    return true;
}
// Steam 等透明選單淡出後仍維持顯示；看不見的視窗不能攔下對下方視窗的點擊。影格須持續完全透明
// 一段時間才讓滑鼠穿透：淡入初期也有全透明影格，此時穿透會讓滑鼠事件落到下方視窗，客體判定離開而關閉選單。
constexpr auto input_transparent_delay = std::chrono::milliseconds(400);
static void apply_input_transparent(SharedWindowState &shared, NativeGui *native, std::uint64_t top, bool transparent) {
    if (transparent == shared.input_transparent.contains(top))
        return;
    if (transparent)
        shared.input_transparent.insert(top);
    else
        shared.input_transparent.erase(top);
    if (native)
        native->set_input_transparent(top, transparent);
    record_diagnostic(transparent ? "remote_input_transparent" : "remote_input_opaque");
    if (trace_shared_frames())
        std::fprintf(stderr, "shared_frame kind=input pid=%d handle=%llu transparent=%d unix_us=%lld\n", static_cast<int>(::getpid()),
                     static_cast<unsigned long long>(top), transparent ? 1 : 0, trace_unix_us());
}
// 內容由其他程序送影格的頂層視窗（例如 Chromium 選單）隱藏後，子視窗常被縮成 0×0，上一張影格也已過時。
// 原生視窗若立即顯示，會先露出未繪製的底色與舊影格；改為透明顯示，等顯示後發布的影格匯入再揭露。
// 只看顯示前已發布過影格的子孫視窗；自身 GDI 繪製、同程序呈現與首次尚無影格的視窗照常立即顯示。
void WindowsServices::hold_remote_reveal(std::uint64_t handle, bool visible) {
#if !defined(_WIN32)
    if (!shared_windows_ || !native_gui_)
        return;
    auto &shared = *shared_windows_;
    if (!visible) {
        if (shared.reveals.erase(handle))
            native_gui_->hold_reveal(handle, false);
        return;
    }
    // 重新顯示即恢復接收滑鼠，避免沿用淡出後的穿透狀態。
    shared.transparent_since.erase(handle);
    apply_input_transparent(shared, native_gui_.get(), handle, false);
    const auto found = gui_.windows.find(handle);
    if (found == gui_.windows.end() || found->second.parent || shared.reveals.contains(handle))
        return;
    SharedWindowState::RevealHold hold;
    for (const auto &[child, window] : gui_.windows) {
        auto ancestor = window.parent;
        for (unsigned depth = 0; ancestor && ancestor != handle && depth < 32; ++depth) {
            const auto up = gui_.windows.find(ancestor);
            ancestor = up == gui_.windows.end() ? 0 : up->second.parent;
        }
        if (child == handle || ancestor != handle)
            continue;
        const auto &slot = shared.table->slots[child % window_slots];
        if (__atomic_load_n(&slot.handle, __ATOMIC_ACQUIRE) != child)
            continue;
        if (const auto sequence = __atomic_load_n(&slot.frame, __ATOMIC_ACQUIRE))
            hold.sequences[child] = sequence;
    }
    if (hold.sequences.empty() || !native_gui_->hold_reveal(handle, true))
        return;
    hold.start = std::chrono::steady_clock::now();
    hold.deadline = hold.start + std::chrono::milliseconds(250);
    shared.reveals.emplace(handle, std::move(hold));
    record_diagnostic("remote_reveal_held");
#else
    (void)handle;
    (void)visible;
#endif
}
void WindowsServices::poll_remote_frames() {
    if (!shared_windows_)
        return;
#if !defined(_WIN32)
    auto &shared = *shared_windows_;
    const auto reveal = [&](auto it, bool timeout) {
        const auto reason = timeout ? "timeout" : "frame";
        if (native_gui_)
            native_gui_->hold_reveal(it->first, false);
        record_diagnostic(timeout ? "remote_reveal_timeout" : "remote_reveal_frame");
        if (trace_shared_frames())
            std::fprintf(stderr, "shared_frame kind=reveal pid=%d handle=%llu reason=%s held_us=%lld\n", static_cast<int>(::getpid()),
                         static_cast<unsigned long long>(it->first), reason,
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - it->second.start).count()));
        return shared.reveals.erase(it);
    };
    std::vector<std::uint64_t> alpha_tops;
    if (!shared.reveals.empty()) {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = shared.reveals.begin(); it != shared.reveals.end();)
            it = now >= it->second.deadline || !gui_.windows.contains(it->first) ? reveal(it, true) : std::next(it);
    }
    if (!shared.transparent_since.empty()) {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = shared.transparent_since.begin(); it != shared.transparent_since.end();) {
            if (!gui_.windows.contains(it->first)) {
                shared.input_transparent.erase(it->first);
                it = shared.transparent_since.erase(it);
                continue;
            }
            if (now - it->second >= input_transparent_delay)
                apply_input_transparent(shared, native_gui_.get(), it->first, true);
            ++it;
        }
    }
    for (const auto &[handle, window] : gui_.windows) {
        publish_gui_window(handle);
        auto &slot = shared.table->slots[handle % window_slots];
        const auto sequence = __atomic_load_n(&slot.frame, __ATOMIC_ACQUIRE);
        if (!sequence)
            continue;
        // 影格表面只在建立時綁定原生 view 的呈現圖層。view 晚於第一張影格建立或之後被重建時，
        // 改綁目前的 view 並立即重新匯入最新影格；否則後續影格只寫入沒有呈現圖層的材質，視窗停在空白，
        // 而客體只在內容改變時才送新影格。
        const bool fresh = shared.frames[handle] != sequence;
        const auto existing = shared.surfaces.find(handle);
        if (!fresh && existing == shared.surfaces.end())
            continue;
        void *view = native_gui_ ? native_gui_->graphics_view(handle) : nullptr;
        const bool rebind = existing != shared.surfaces.end() && existing->second && shared.surface_views[handle] != view;
        if (!fresh && !rebind)
            continue;
        const auto name = shared.directory + "/frame-" + std::to_string(handle);
        SharedFd file{::open(name.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW)};
        if (file.fd < 0 || ::flock(file.fd, LOCK_SH | LOCK_NB))
            continue;
        struct stat status{};
        if (::fstat(file.fd, &status) || !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
            (status.st_mode & 077) || status.st_nlink != 1)
            continue;
        const auto published = __atomic_load_n(&slot.frame, __ATOMIC_ACQUIRE);
        FrameHeader header{};
        if (::pread(file.fd, &header, sizeof(header), 0) != sizeof(header) || header.magic != window_magic || header.version!=2 ||
            !header.width || !header.height || header.width > 16384 || header.height > 16384 ||
            header.stride!=((header.width*4+255U)&~255U) ||
            header.bytes!=frame_bytes(header.stride,header.height) || header.bytes>128*1024*1024 ||
            status.st_size<static_cast<off_t>(static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE))+header.bytes) ||
            (header.format!=28 && header.format!=29 && header.format!=87 && header.format!=91))
            continue;
        const auto mapping=shared.frame_mapping(handle,file.fd,static_cast<std::size_t>(header.bytes),status);
        auto &surface = shared.surfaces[handle];
        const bool created = !surface || surface->width() != header.width || surface->height() != header.height || rebind;
        if (created) {
            surface = make_hardware_raster(header.width, header.height, {}, view);
            shared.surface_views[handle] = view;
            if (rebind) record_diagnostic("remote_frame_surface_rebound");
            rasters_[0x10000000 + handle] = surface;
            if (native_gui_)
                native_gui_->graphics_surface(handle, surface);
        }
        // 頂層視窗為 DWM 整片玻璃，或帶 WS_EX_COMPOSITED（Chromium 透明視窗）時依影格 alpha 合成：
        // 選單淡出後留在畫面上的全透明影格、陰影與圓角不再呈現為黑色。其他視窗維持不透明呈現。
        bool glass = false;
        std::uint64_t top = 0;
        for (auto ancestor = handle; ancestor;) {
            const auto up = gui_.windows.find(ancestor);
            if (up == gui_.windows.end())
                break;
            if (!up->second.parent) {
                top = ancestor;
                if ((up->second.ex_style & 0x02000000U) && !up->second.remote_alpha) {
                    up->second.remote_alpha = true;
                    alpha_tops.push_back(ancestor);
                }
                glass = up->second.dwm_glass || up->second.remote_alpha;
                break;
            }
            ancestor = up->second.parent;
        }
        if (auto &applied = shared.surface_glass[handle]; created || applied != glass) {
            surface->set_transparent(glass);
            applied = glass;
        }
        const auto import_begin=trace_shared_frames()?trace_unix_us():0;
        surface->import_frame(mapping->data,mapping->size,header.stride,header.format,shared.cache_frames?mapping:std::shared_ptr<void>{});
        record_diagnostic("remote_frame_imported");
        if (top) {
            if (glass && mapping->data && frame_fully_transparent(mapping->data, mapping->size, header.width, header.height, header.stride))
                shared.transparent_since.try_emplace(top, std::chrono::steady_clock::now());
            else {
                shared.transparent_since.erase(top);
                apply_input_transparent(shared, native_gui_.get(), top, false);
            }
        }
        {static std::int64_t last=0,gap=0;record_frame_timing("remote_frame_first_imported","remote_frame_max_import_gap",last,gap);}
        // 每 60 幀抽樣一次匯入影格的平均亮度（8×8 格點），判斷 GPU 程序產出的影格本身是否全黑。
        if(static std::uint64_t imported=0;(imported++%60)==0 && mapping->data && header.width && header.height){
            const auto* pixels=static_cast<const std::uint8_t*>(mapping->data);double sum=0;unsigned count=0;
            for(unsigned gy=0;gy<8;++gy)for(unsigned gx=0;gx<8;++gx){
                const auto x=(header.width*(2*gx+1))/16,y=(header.height*(2*gy+1))/16;const auto at=std::size_t{y}*header.stride+std::size_t{x}*4;
                if(at+3<mapping->size){sum+=0.299*pixels[at+2]+0.587*pixels[at+1]+0.114*pixels[at];++count;}}
            if(count)set_diagnostic("remote_frame_last",std::to_string(header.width)+"x"+std::to_string(header.height)+" fmt="+std::to_string(header.format)+
                " luma="+std::to_string(static_cast<int>(sum/count))+" handle="+std::to_string(handle));
        }
        if(trace_shared_frames()){const auto now=trace_unix_us();
            // 8×8 格點的平均亮度與 alpha 範圍，判斷閃白／閃黑是否來自客體送出的影格本身。
            double luma=0;unsigned samples=0,alpha_min=255,alpha_max=0;
            if(const auto* pixels=static_cast<const std::uint8_t*>(mapping->data))
                for(unsigned gy=0;gy<8;++gy)for(unsigned gx=0;gx<8;++gx){
                    const auto at=std::size_t{(header.height*(2*gy+1))/16}*header.stride+std::size_t{(header.width*(2*gx+1))/16}*4;
                    if(at+3>=mapping->size)continue;
                    luma+=0.299*pixels[at+2]+0.587*pixels[at+1]+0.114*pixels[at];alpha_min=std::min<unsigned>(alpha_min,pixels[at+3]);alpha_max=std::max<unsigned>(alpha_max,pixels[at+3]);++samples;
                }
            std::fprintf(stderr,"shared_frame kind=import pid=%d handle=%llu seq=%u previous=%u unix_us=%lld import_us=%lld size=%ux%u luma=%d alpha=%u-%u\n",static_cast<int>(::getpid()),
                static_cast<unsigned long long>(handle),published,shared.frames[handle],now,now-import_begin,header.width,header.height,
                samples?static_cast<int>(luma/samples):-1,alpha_min,alpha_max);}
        shared.frames[handle] = published;
        for (auto it = shared.reveals.begin(); it != shared.reveals.end();) {
            const auto held = it->second.sequences.find(handle);
            it = held != it->second.sequences.end() && held->second != published ? reveal(it, false) : std::next(it);
        }
        (void)window;
    }
    // 迭代 gui_.windows 時不重新呈現；新改為依 alpha 合成的頂層視窗在此同步原生透明狀態。
    for (const auto top : alpha_tops)
        if (gui_.windows.contains(top))
            present_gui(top);
#endif
}
} // namespace x64arm
