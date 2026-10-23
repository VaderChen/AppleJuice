#include "x64arm/windows_services.hpp"
#include "x64arm/resource_usage.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include "window_presentation_fault.hpp"
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
struct SharedWindowState {
    std::string directory;
    SharedWindowTable *table{};
    std::map<std::uint64_t, SharedWindowInfo> published;
    std::map<std::uint64_t, std::uint32_t> frames;
    std::map<std::uint64_t, std::shared_ptr<RasterSurface>> surfaces;
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
            for (auto h : allocated) {
                auto &slot = table->slots[h % window_slots];
                if (__atomic_load_n(&slot.handle, __ATOMIC_ACQUIRE) == h)
                    __atomic_store_n(&slot.handle, 0U, __ATOMIC_RELEASE);
            }
            ::munmap(table, sizeof(SharedWindowTable));
        }
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
    shared_windows_ = std::move(value);
#endif
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
    info.id = w.id;
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
    std::copy_n(w.class_name.begin(), std::min(w.class_name.size(), info.class_name.size() - 1),
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
    if (const auto old = shared.published.find(handle); old != shared.published.end() && old->second == info)
        return;
    // 只在欄位有變化時發布；結構尾端 padding 不屬於視窗狀態。
    std::lock_guard lock(shared);
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
    shared.published.erase(handle);
    shared.allocated.erase(handle);
    shared.frames.erase(handle);
    shared.surfaces.erase(handle);
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
bool WindowsServices::invoke_remote_gui(GuestState &state, GuestMemory &memory,
                                        const WindowsServiceDescriptor &service) {
    if (!processes_ || service.family != "user32.dll" || !service.arguments)
        return false;
    const auto symbol = service.symbol;
    const bool supported =
        symbol == "IsWindow" || symbol == "IsWindowVisible" || symbol == "IsWindowEnabled" || symbol == "IsIconic" ||
        symbol == "IsZoomed" || symbol == "GetWindowThreadProcessId" || symbol == "GetClientRect" ||
        symbol == "GetWindowRect" || symbol == "GetParent" || symbol == "GetAncestor" || symbol == "GetDC" ||
        symbol == "GetWindowDC" || symbol == "ReleaseDC" || symbol == "WindowFromDC" || symbol == "ClientToScreen" ||
        symbol == "ScreenToClient" || symbol.starts_with("GetWindowLong") || symbol.starts_with("SetWindowLong") ||
        symbol.starts_with("GetClassName") || symbol.starts_with("GetWindowText") ||
        symbol=="GetWindowDisplayAffinity" || symbol=="SetWindowDisplayAffinity";
    if (!supported)
        return false;
    const WindowsCall arg(state, memory, service);
    const auto handle = symbol == "WindowFromDC" ? (arg(0) >= 0x10000000 ? arg(0) - 0x10000000 : 0) : arg(0);
    if (gui_.windows.contains(handle))
        return false;
    const auto found = shared_gui_window(handle, false);
    if (!found)
        return false;
    const auto &w = *found;
    std::uint64_t result = 0;
    if (!allowed_gui_window(w)) {
        last_error_ = 5;
        arg.return_to(state, 0);
        return true;
    }
    if(symbol=="SetWindowDisplayAffinity")last_error_=5;
    else if(symbol=="GetWindowDisplayAffinity"){
        if(!arg(1) || (w.style&0x40000000U))last_error_=87;
        else {const auto affinity=w.display_affinity==1 || w.display_affinity==0x11?w.display_affinity:0U;memory.store(GuestAddress{arg(1)},32,affinity);result=1;}
    }
    else if (symbol == "IsWindow")
        result = 1;
    else if (symbol == "IsWindowVisible")
        result = w.visible;
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
    } else if (symbol == "GetParent")
        result = w.parent ? w.parent : w.owner;
    else if (symbol == "GetAncestor") {
        const auto flags = static_cast<std::uint32_t>(arg(1));
        if (flags < 1 || flags > 3)
            last_error_ = 87;
        else if (flags == 1)
            result = w.parent;
        else {
            result = handle;
            auto current = w;
            for (unsigned i = 0; i < 64; ++i) {
                const auto parent = current.parent ? current.parent : flags == 3 ? current.owner : 0;
                if (!parent)
                    break;
                const auto next = shared_gui_window(parent);
                if (!next)
                    break;
                result = parent;
                current = *next;
            }
        }
    } else if (symbol == "GetDC" || symbol == "GetWindowDC") {
        result = 0x10000000 + handle;
        gui_.dc_objects.try_emplace(result, 0xb011);
    } else if (symbol == "ReleaseDC") {
        if (arg(1) == 0x10000000 + handle)
            result = gui_.dc_objects.erase(arg(1)) ? 1 : 0;
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
        const bool wide = symbol.ends_with('W'), cls = symbol.starts_with("GetClassName"),
                   length = symbol.starts_with("GetWindowTextLength");
        const auto value = cls ? std::u16string(w.class_name.data()) : std::u16string(w.title.data());
        if (length)
            result = wide ? value.size() : utf16_to_utf8(value).size();
        else if (arg(2)) {
            const auto capacity = static_cast<std::uint32_t>(arg(2));
            if (wide) {
                result = std::min<std::size_t>(value.size(), capacity - 1);
                memory.write(GuestAddress{arg(1)}, utf16_bytes(value.substr(0, result)));
            } else {
                const auto text = utf16_to_utf8(value);
                std::vector<std::uint8_t> bytes(text.begin(), text.end());
                result = std::min<std::size_t>(bytes.size(), capacity - 1);
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
    const auto width=desc.width,height=desc.height,format=desc.format;
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
    graphics.graphics_export_frame(view,mapping->data,mapping->size,stride,shared.cache_frames?mapping:std::shared_ptr<void>{});
    if (::pwrite(file.fd, &header, sizeof(header), 0) != sizeof(header))
        throw HostCallFault("共用影格標頭寫入失敗");
    auto &slot = shared.table->slots[handle % window_slots];
    if (__atomic_load_n(&slot.handle, __ATOMIC_ACQUIRE) != handle ||
        __atomic_load_n(&slot.owner, __ATOMIC_ACQUIRE) != window->process)
        throw WindowPresentationFault("遠端呈現期間 HWND 已關閉");
    const auto sequence=__atomic_add_fetch(&slot.frame, 1U, __ATOMIC_RELEASE);
    record_diagnostic("remote_frame_published");
    if(trace_shared_frames()){const auto now=trace_unix_us();
        std::fprintf(stderr,"shared_frame kind=publish pid=%d handle=%llu seq=%u unix_us=%lld export_us=%lld\n",static_cast<int>(::getpid()),
            static_cast<unsigned long long>(handle),sequence,now,now-export_begin);}
#else
    throw HostCallFault("此平台缺少共用視窗呈現");
#endif
}
void WindowsServices::poll_remote_frames() {
    if (!shared_windows_)
        return;
#if !defined(_WIN32)
    auto &shared = *shared_windows_;
    for (const auto &[handle, window] : gui_.windows) {
        publish_gui_window(handle);
        auto &slot = shared.table->slots[handle % window_slots];
        const auto sequence = __atomic_load_n(&slot.frame, __ATOMIC_ACQUIRE);
        if (!sequence || shared.frames[handle] == sequence)
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
        if (!surface || surface->width() != header.width || surface->height() != header.height) {
            surface = make_hardware_raster(header.width, header.height, {},
                                           native_gui_ ? native_gui_->graphics_view(handle) : nullptr);
            rasters_[0x10000000 + handle] = surface;
            if (native_gui_)
                native_gui_->graphics_surface(handle, surface);
        }
        const auto import_begin=trace_shared_frames()?trace_unix_us():0;
        surface->import_frame(mapping->data,mapping->size,header.stride,header.format,shared.cache_frames?mapping:std::shared_ptr<void>{});
        record_diagnostic("remote_frame_imported");
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
            std::fprintf(stderr,"shared_frame kind=import pid=%d handle=%llu seq=%u previous=%u unix_us=%lld import_us=%lld\n",static_cast<int>(::getpid()),
                static_cast<unsigned long long>(handle),published,shared.frames[handle],now,now-import_begin);}
        shared.frames[handle] = published;
        (void)window;
    }
#endif
}
} // namespace x64arm
