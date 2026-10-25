#pragma once
#include "x64arm/state.hpp"
#include "x64arm/cpu_features.hpp"
#include "x64arm/address_cache.hpp"
#include <array>
#include <utility>
#include <atomic>
#include <span>
#include <optional>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace x64arm {
enum class Access : std::uint8_t { Read = 1, Write = 2, Execute = 4 };
using Permissions = std::uint8_t;
constexpr Permissions permission(Access access) { return static_cast<Permissions>(access); }
inline constexpr Permissions read_write = permission(Access::Read) | permission(Access::Write);
inline constexpr Permissions read_execute = permission(Access::Read) | permission(Access::Execute);
std::string_view access_name(Access access);
struct MemoryFault : std::runtime_error {
    GuestAddress address;
    Access access;
    std::size_t size;
    MemoryFault(GuestAddress address, Access access, std::size_t size, const char* message);
};
// 來源頁保護失敗不是客體 MemoryFault，也不能作為可忽略的快取 miss。
struct CodeSourceError : std::runtime_error {using std::runtime_error::runtime_error;};
struct CodeSourceOwnerRequired : CodeSourceError {using CodeSourceError::CodeSourceError;};
// 主機提供的資料區域；owner 保證 mmap / buffer 生命期，客體地址不洩漏主機指標。
class MemoryBacking {
public:
    // 私人 backing 的全部 bytes 初始為零；大型匿名映射由宿主按需提供零頁。
    // 此保證只適用剛建立的私人 backing，不涵蓋重用空洞或下方外部儲存建構式。
    explicit MemoryBacking(std::size_t size, std::uintptr_t alignment_offset=0);
    // StableShared 僅用於引擎持有生命週期且建立後不可縮小的共用區段；一般檔案 mmap 不適用。
    enum class ExternalAccess { Checked, StableShared };
    MemoryBacking(std::shared_ptr<void> owner, std::span<std::uint8_t> bytes, ExternalAccess access=ExternalAccess::Checked);
    std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool external() const noexcept { return external_; }
    bool stable_shared() const noexcept { return stable_shared_; }
private:
    friend class GuestMemory;
    void reuse_pages(std::size_t offset,std::size_t size);
    std::shared_ptr<void> owner_;
    std::uint8_t* data_{};
    std::size_t size_{};
    bool external_{},stable_shared_{};
    std::span<std::uint8_t> native_storage_;
    bool reusable_pages_{};
};
class GuestMemory {
public:
    // JIT 僅讀取不可變 Region 欄位；映射變動仍須等待所有 worker 返回。
    struct NativeLayout {std::uint32_t cache,direct,mmu,mmu_permissions;};
    // 資格表低三位沿用讀／寫／RMW；額外位只表示整個客體頁均有效，排除填補與空洞。
    static constexpr unsigned native_mmu_full_read=1U<<3,native_mmu_full_write=1U<<4;
    static constexpr unsigned native_mmu_code_source=1U<<5;
    static constexpr unsigned native_page_bits=9;
    static constexpr std::size_t native_page_slots=std::size_t{1}<<native_page_bits;
    static constexpr std::size_t native_page_index(std::uint64_t page) noexcept {return address_cache::index(page,native_page_slots-1);}
    static constexpr unsigned native_region_bits=6;
    static constexpr std::size_t native_region_slots=std::size_t{1}<<native_region_bits;
    static constexpr std::size_t native_region_index(std::uint64_t page) noexcept {return address_cache::index(page,native_region_slots-1);}
    // 平行更新必須以完整 16 bytes 發布；原生 LDP 僅在 LSE2 平台啟用。
    struct alignas(16) NativePageEntry {std::uint64_t tag{UINT64_MAX},addend{};};
    static bool native_flat_cache_available() noexcept;
    static NativeLayout native_layout() noexcept;
    struct NativeRegionLayout {std::uint32_t cache,base,length,permissions,data;};
    static NativeRegionLayout native_region_layout() noexcept;
    unsigned processor_count()const noexcept{return processor_count_;}
    void set_processor_count(unsigned count){if(!count || count>64)throw std::invalid_argument("客體處理器數須介於 1 與 64");processor_count_=count;}
    const VirtualProcessorIdentity& processor_identity()const noexcept{return processor_identity_;}
    // 與映射變更相同，只能在所有客體 worker 停止時設定。副本／快照保留各自身分。
    void set_processor_identity(VirtualProcessorIdentity identity){
        if(identity.base_mhz>65535 || identity.maximum_mhz>65535 || identity.bus_mhz>65535 ||
           (identity.base_mhz && identity.maximum_mhz && identity.base_mhz>identity.maximum_mhz) || identity.brand.back())
            throw std::invalid_argument("客體 CPU 品牌或頻率設定無效");
        processor_identity_=identity;
    }
    ExecutionMode execution_mode() const noexcept { return mode_; }
    void set_execution_mode(ExecutionMode mode) noexcept { if (mode_ != mode) { mode_ = mode; ++generation_; ++structure_generation_; ++generation_stats_.mode_changes; } }
    GuestMemory();
    explicit GuestMemory(std::size_t limit);
    GuestMemory(const GuestMemory& other);
    GuestMemory& operator=(const GuestMemory& other);
    GuestMemory(GuestMemory&& other) noexcept;
    GuestMemory& operator=(GuestMemory&& other) noexcept;
    // 候選政策在建構時固定；所有 Execute 消費者共用單調來源頁追蹤。
    // 未裝 scope 時，與映射 API 一樣要求呼叫者已停止所有客體 worker。
    // 平行執行須在 worker 啟動前裝 owner scope；回呼只能 quiesce，不得排程、
    // 執行客體回呼或改映射。巢狀 scope 僅在同一 owner 執行緒按 LIFO 使用。
    class CodeSourceScope {
    public:
        CodeSourceScope(const GuestMemory&,void(*quiesce)(void*),void* opaque);
        ~CodeSourceScope();
        CodeSourceScope(const CodeSourceScope&)=delete;
        CodeSourceScope& operator=(const CodeSourceScope&)=delete;
        void check() const;
    private:
        friend class GuestMemory;
        const GuestMemory* memory_{};void(*quiesce_)(void*){};void* opaque_{};
        CodeSourceScope* previous_{};bool owns_token_{};
        static thread_local CodeSourceScope* current_;
    };
    void ensure_code_source(GuestAddress,std::size_t) const;
    void throw_if_code_source_failed() const;
    static constexpr std::size_t capacity_limit = 256U * 1024U * 1024U;
    static constexpr std::size_t maximum_capacity = UINT64_C(8) * 1024 * 1024 * 1024;
    static constexpr std::size_t guest_page_size = 4096;
    void map(GuestAddress base, std::size_t size, Permissions permissions, std::span<const std::uint8_t> initial = {});
    void map_shared(GuestAddress base, std::size_t size, Permissions permissions,
                    const std::shared_ptr<MemoryBacking>& backing, std::size_t offset = 0);
    void check(GuestAddress address, std::size_t size, Access access) const;
    void read(GuestAddress address, std::span<std::uint8_t> destination, Access access = Access::Read) const;
    // 短期唯讀視圖：檢查第一個 byte，最多到目前映射尾端，不探查後方映射。
    // 映射／保護變更前須結束使用；呼叫者不得跨 API／worker 邊界保存此視圖。
    std::span<const std::uint8_t> readable_prefix(GuestAddress address, std::size_t maximum, Access access = Access::Read) const;
    void write(GuestAddress address, std::span<const std::uint8_t> source);
    // API 共用的批次操作；先完整驗證範圍，保留重疊搬移、故障與可執行寫入紀錄。
    // 私有連續映射使用宿主原生函式；跨映射／外部別名搬移保留完整來源快照。
    void move(GuestAddress destination,GuestAddress source,std::size_t size);
    void fill(GuestAddress destination,std::uint8_t value,std::size_t size);
    int compare(GuestAddress left,GuestAddress right,std::size_t size) const;
    // 載入器在所有客體 worker 停止時初始化穩定唯讀資料；不撤銷原生讀取資格。
    // 不得用來更新外部共享或已標為動態的資料；客體 store 不經過此介面。
    void initialize_readonly(GuestAddress address, std::span<const std::uint8_t> source);
    // 僅供主機維護唯讀資料頁；拒絕程式碼頁，客體 store 不經過此介面。
    void update_readonly(GuestAddress address, std::span<const std::uint8_t> source);
    std::uint64_t load(GuestAddress address, std::uint8_t width, Access access = Access::Read) const;
    void store(GuestAddress address, std::uint8_t width, std::uint64_t value);
    enum class AtomicRmw {Add,And,Or,Xor,Exchange,CompareExchange,Negate};
    // 對齊連續 backing 使用宿主原子操作；跨映射／非對齊由停止所有客體 worker 的協調路徑處理。
    std::uint64_t atomic_rmw(GuestAddress,std::uint8_t width,AtomicRmw,std::uint64_t source,std::uint64_t expected=0);
    // 回傳比較時觀察到的完整值；即使比較失敗仍驗證寫入權限。
    std::array<std::uint64_t,2> compare_exchange128(GuestAddress address,
        std::array<std::uint64_t,2> expected,std::array<std::uint64_t,2> desired);
    // JIT 使用的短期指標：完整檢查範圍及權限；跨映射、外部共享或宿主更新頁回退。
    void* native_pointer(GuestAddress address, unsigned size, unsigned permissions) noexcept;
    void* native_page_pointer(GuestAddress address, unsigned size, unsigned permissions) noexcept;
    // Apple ARM64 JIT 專用；bit 62 的 TBI 標記要求共享存取屏障，不傳給一般 API。
    static constexpr std::uint64_t native_shared_tag=UINT64_C(1)<<62;
    void* native_jit_pointer(GuestAddress address,unsigned size,unsigned permissions) noexcept;
    bool native_shared_access() const noexcept {return native_shared_access_;}
    // 在所有客體 worker 停止時同步完整宿主頁別名；軟體單核模式排除外部共享資料。
    bool prepare_native_mmu(bool allow_shared=true);
    // 兩種政策均要求完整範圍與同質權限；exact_layout 控制同質子頁合併政策。
    // 切換政策會撤銷並重建別名；一般呼叫仍依既有診斷設定選擇映射政策。
    bool prepare_native_mmu(bool allow_shared,bool exact_layout);
    // 只在映射重建後更新；獨立 backing 共宿主頁或寫入 COW／程式碼需要一般存取模式。
    bool native_mmu_direct_complete() const noexcept {return native_mmu_complete_;}
    // 只在重建／派送邊界核對；讀取別名不得額外涵蓋客體空洞或混合權限。
    // 未映射的宿主頁可由精確故障恢復處理，不要求所有合法讀取都有別名。
    bool native_mmu_precise_reads() const noexcept {
        return native_mmu_owner_ && !native_mmu_dirty_count_ && native_mmu_partial_pages_.empty();
    }
    struct NativeMmuGap {
        GuestAddress base{};std::size_t length{};unsigned required{},first{},last{};
        const char* reason{};bool host_updated{},external{},copy_on_write{};
    };
    // 映射重建與明確要求的診斷共用同一份資格判定；不在客體熱碼中盤點。
    std::optional<NativeMmuGap> native_mmu_gap() const noexcept;
    bool native_mmu_update_pending() const noexcept {return native_mmu_dirty_count_!=0;}
    // 常駐 ABI 在入口及宿主返回時讀取；欄位地址在物件生命期內固定，內容只能在 worker 停止時改變。
    // 不暴露或硬編碼 GuestMemory 的內部位移；呼叫者不得在客體執行途中搬移此物件。
    const std::uintptr_t* native_mmu_base_address() const noexcept {return native_mmu_bases_.data();}
    // 原生 MMU 視窗：宿主位址＝視窗基底＋客體位址。x64 預設配置（映像、模組、虛擬配置、堆積、
    // 堆疊、TEB／PEB）全部落在 256 GiB 內，PE32 的 4 GiB 是其子集；超出視窗的位址仍走受檢 helper。
    // 視窗 64 TiB：客體配置上限受 user_address_limit 與視窗夾住，所有合法 x64 位址都在視窗內。
    static constexpr unsigned native_mmu_window_bits=46;
    static constexpr std::uint64_t native_mmu_window=UINT64_C(1)<<native_mmu_window_bits;
    // 視窗前後至少各保留 8 GiB 不可存取區：已證明在視窗內的基底加上 disp32 與小幅堆疊位移，
    // 仍落在保留區內並以硬體故障進入精確恢復，不會觸及宿主其他映射。
    static constexpr std::uint64_t native_mmu_guard=UINT64_C(1)<<33;
    // 優先保留寬 guard（前 16 TiB、後 32 TiB）：任何 64 位元客體位址加基底後，不是落在保留區（硬體故障、
    // 精確恢復）就是超出宿主 47 位元 VA（同樣故障）；只剩 (-16 TiB-基底, -16 TiB) 的垃圾指標會回繞到宿主低位址。
    // 此時產碼不再逐次檢查視窗（implicit window），改由硬體 MMU 捕捉非法存取。
    static constexpr std::uint64_t native_mmu_low_guard=UINT64_C(1)<<44,native_mmu_high_guard=UINT64_C(1)<<45;
    static bool native_mmu_implicit_window() noexcept {return native_mmu_implicit_.load(std::memory_order_acquire);}
    bool native_mmu_contains(std::uintptr_t address) const noexcept;
    // 短期原生批次視窗，限制在同一客體頁與 Region；寫入排除可執行頁。
    // before 不含目前 byte，after 含目前 byte；排除外部 backing／宿主更新頁。
    struct NativeWindow {std::uint8_t* data{};std::size_t before{},after{};};
    NativeWindow native_window(GuestAddress address,unsigned permissions) noexcept;
    // 完整私人連續 backing 借用：可跨同一 backing 的相鄰 Region，不回填 JIT 頁面快取。
    // 呼叫及使用期間須禁止映射／保護／backing 變動；僅借資料，不借 Region 指標或所有權。
    // 不完整、COW、外部、宿主更新或無法證明無 guest alias 的範圍回傳空值。
    // 只在一致邊界核對身分及世代後重借。
    std::span<std::uint8_t> native_private_span(GuestAddress address,std::size_t size,unsigned permissions) noexcept;
    std::uint64_t native_borrow_generation() const noexcept {return native_borrow_generation_;}
    struct WindowStats {std::uint64_t pages{},mixed_permissions{},partial{},split_backing{},excluded{},eligible{};};
    // 量測 MMU 視窗的保守候選頁；呼叫者須在 worker 停止時讀取。
    WindowStats window_stats(std::size_t host_page_size)const;
    struct Mapping { GuestAddress base; std::size_t size; Permissions permissions; };
    std::optional<Mapping> query(GuestAddress address) const noexcept;
    std::optional<Mapping> next_mapping(GuestAddress address) const noexcept;
    bool range_available(GuestAddress address, std::uint64_t size) const noexcept;
    void discard(GuestAddress address, std::size_t size);
    // 保留 commit／映射／權限，先驗證整段私人 backing 才清除內容；完整宿主頁可交還 VM。
    struct DiscardResult {std::uint64_t cleared{},advised{};};
    DiscardResult discard_contents(GuestAddress address,std::size_t size,bool make_inaccessible=false);
    void protect(GuestAddress address, std::size_t size, Permissions permissions);
    // 完整驗證後將範圍改為私人 backing；其他 alias／程序的共享資料不受寫入影響。
    void privatize(GuestAddress address,std::size_t size,Permissions permissions);
    void make_copy_on_write(GuestAddress address,std::size_t size,Permissions permissions);
    bool is_copy_on_write(GuestAddress address) const noexcept;
    // 多段權限先完整驗證及配置描述元，再一次提交，供部分 reclaim 還原不同保護。
    void protect_ranges(std::span<const Mapping> ranges);
    bool mapped(GuestAddress address) const noexcept;
    std::uint64_t identity() const noexcept { return identity_; }
    std::uint64_t code_generation() const noexcept { return generation_; }
    // 不含已記錄範圍的可執行寫入／映射／權限／範圍刷新；JIT 依此判斷是否需全域清除。
    // 直譯器與 AOT 解碼快取仍使用 code_generation()；環狀紀錄遺失由各觀察者處理，不遞增此值。
    std::uint64_t code_structure_generation() const noexcept { return structure_generation_; }
    // 只量測既有世代失效來源，不改變快取撤銷語義。non_executable_protect 為 protect 的子集。
    struct GenerationStats {
        std::uint64_t executable_maps{},executable_unmaps{},protect{},non_executable_protect{},
            executable_writes{},executable_write_bytes{},restores{},flushes{},mode_changes{},
            last_write_address{},last_write_region_base{},last_write_region_size{};
        // 寫入前後位元組完全相同的可執行寫入；仍照常遞增世代，僅供評估寫前比對的收益。
        std::uint64_t executable_write_unchanged{},executable_write_unchanged_bytes{};
        std::uint64_t flush_ranges{};
    };
    const GenerationStats& generation_stats() const noexcept { return generation_stats_; }
    // 程式碼範圍變動的環狀紀錄；mapping 區分映射／權限與位元組寫入，避免混入寫入分類。
    // 以累計序號定位：每個觀察者各自記錄已處理序號，落後超過容量時只有該觀察者自行撤銷；
    // 紀錄滿載不再遞增結構世代，已逐筆核對過紀錄的觀察者不會因此丟掉有效翻譯。
    struct ExecutableWrite {std::uint64_t address{},size{};bool unchanged{},flush{},mapping{};};
    static constexpr std::size_t executable_write_log_capacity=64;
    struct ExecutableWriteWindow {
        std::array<ExecutableWrite,executable_write_log_capacity> entries{};std::size_t count{};
        std::uint64_t lost{},sequence{}; // lost：窗口起點之前已被覆寫而取不回的筆數；sequence：擷取時的累計序號。
        std::size_t size() const noexcept {return count;}
        bool empty() const noexcept {return count==0;}
        const ExecutableWrite* begin() const noexcept {return entries.data();}
        const ExecutableWrite* end() const noexcept {return entries.data()+count;}
        const ExecutableWrite& operator[](std::size_t index) const noexcept {return entries[index];}
        std::span<const ExecutableWrite> span() const noexcept {return {entries.data(),count};}
    };
    std::uint64_t executable_write_sequence() const noexcept {return executable_write_sequence_;}
    ExecutableWriteWindow executable_writes_since(std::uint64_t from) const noexcept;
    // 共用消費位置：舊 JIT 取走紀錄後清除；獨立觀察者改用自己的序號，不受此影響。
    ExecutableWriteWindow executable_write_log() const noexcept {return executable_writes_since(executable_write_consumed_);}
    std::uint64_t executable_write_overflow() const noexcept {
        const auto pending=executable_write_sequence_-executable_write_consumed_;
        return pending>executable_write_log_capacity?pending-executable_write_log_capacity:0;
    }
    bool executable_writes_pending() const noexcept {return executable_write_sequence_!=executable_write_consumed_;}
    void clear_executable_write_log() noexcept {executable_write_consumed_=executable_write_sequence_;}
    // 由 CPU／JIT 共用的快取世代失效；服務返回後 dispatcher 會清除舊翻譯。
    void flush_instruction_cache() noexcept { ++generation_; ++structure_generation_; ++generation_stats_.flushes; }
    // 範圍刷新：整體世代照常遞增；JIT 只撤銷來源位元組與範圍重疊的翻譯。
    void flush_instruction_cache(GuestAddress address,std::size_t size) noexcept;
    bool same_contents(const GuestMemory& other) const { return processor_identity_==other.processor_identity_ && processor_count_==other.processor_count_ && mode_ == other.mode_ && limit_ == other.limit_ && regions_ == other.regions_ && virtual_reservations_==other.virtual_reservations_; }
    void restore_writable(const GuestMemory& snapshot);
    void unmap(GuestAddress base);
    void unmap(GuestAddress base, std::size_t size);
    std::size_t limit() const noexcept { return limit_; }
    std::size_t remaining_capacity() const noexcept { return limit_ - allocated_; }
    // 客體 backing 的頁面計數；不等同宿主程序 RSS，外部共享 backing 不計入 private commit。
    struct Usage {std::uint64_t committed{},private_commit{},peak_committed{},peak_private_commit{},virtual_size{},peak_virtual_size{};};
    Usage usage() const noexcept;
    // Windows 保留區與 backing 共用地址聯集，統計不把 reserve 與 commit 重複計算。
    void reserve_virtual_range(GuestAddress,std::uint64_t);
    void release_virtual_range(GuestAddress,std::uint64_t);
    void set_private_commit_limit(std::uint64_t limit) noexcept { private_commit_limit_=limit; }
    std::uint64_t private_commit_limit() const noexcept { return private_commit_limit_; }
    struct TrimResult {std::uint64_t bytes{},ranges{};std::error_code error;};
    // 原生 residency 提示：保留 bytes、映射、權限及 JIT 世代，只處理完整的私人宿主頁。
    TrimResult trim_working_set() const;
    struct MemoryRange {GuestAddress address;std::size_t size;};
    // 完整預檢可讀／可執行範圍後，以原生 WILLNEED 預取 backing；不更改內容及 JIT 世代。
    TrimResult prefetch(std::span<const MemoryRange> ranges) const;
    struct WorkingSetLimits {
        std::uint64_t minimum{50 * guest_page_size},maximum{345 * guest_page_size};
        bool operator==(const WorkingSetLimits&) const = default;
    };
    WorkingSetLimits working_set_limits() const noexcept;
    std::error_code set_working_set_limits(std::uint64_t minimum,std::uint64_t maximum);
    struct PageLockResult {std::uint64_t pages{};bool complete{true};std::error_code error;};
    // 鎖定客體頁的實際 backing；重複鎖定不累計次數，宿主頁別名共用原生鎖。
    PageLockResult lock_pages(GuestAddress address,std::size_t size);
    PageLockResult unlock_pages(GuestAddress address,std::size_t size);
    bool page_locked(GuestAddress address) const noexcept;
    std::uint64_t locked_page_count() const noexcept;

private:
    // 保持原有 64-byte 熱描述；頁證明另外配置，不改變 Region 步長。
    struct Region {
        GuestAddress base; Permissions permissions, maximum_permissions;
        std::shared_ptr<MemoryBacking> backing;
        std::size_t offset{}, length{};
        bool host_updated{};
        bool copy_on_write{};
        Permissions native_permissions{}; // 排除外部 backing／宿主更新頁，隨映射重建。
        std::uint8_t* first{}; // 熱 load/store 不必解參照 backing。
        Region(GuestAddress base, Permissions permissions, std::shared_ptr<MemoryBacking> backing,
               std::size_t offset, std::size_t size, Permissions maximum = 7, bool host_updated = false,bool copy_on_write = false);
        std::uint8_t* data() const noexcept { return first; }
        std::uint8_t* begin() const noexcept { return first; }
        std::uint8_t* end() const noexcept { return first + length; }
        std::size_t size() const noexcept { return length; }
        bool operator==(const Region&) const;
    };
    std::vector<Region> regions_;
    struct BackingReclaim {
        std::shared_ptr<MemoryBacking> backing;
        std::vector<std::pair<std::uintptr_t,std::uintptr_t>> spans;
    };
    std::vector<BackingReclaim> plan_backing_reclaim(GuestAddress,std::size_t) const;
    static void reclaim_backing_pages(std::span<const BackingReclaim>) noexcept;
    std::size_t allocated_{};
    bool native_shared_access_{};
    bool copy_on_write_present_{};
    std::uint64_t peak_committed_{},peak_private_commit_{};
    void record_usage() noexcept;
    Usage range_usage(GuestAddress,std::uint64_t) const noexcept;
    void record_usage(GuestAddress,std::uint64_t,const Usage&) noexcept;
    std::size_t limit_{capacity_limit};
    std::uint64_t private_commit_limit_{UINT64_MAX};
    std::uint64_t identity_{};
    std::uint64_t generation_{};
    std::uint64_t structure_generation_{};
    // 與物件身分一起使用；純 RW 映射／權限／私人 backing 遷移也撤銷長借用。
    // 不隨普通資料寫入、原生 MMU alias 重建或 JIT 頁面快取命中遞增。
    std::uint64_t native_borrow_generation_{};
    bool code_source_watch_enabled_{};
    mutable std::set<std::uint64_t> code_source_pages_;
    mutable const std::set<std::uint64_t>* code_source_pending_{};
    mutable std::atomic<const CodeSourceScope*> code_source_owner_{};
    mutable std::atomic<bool> code_source_failed_{};
    mutable std::exception_ptr code_source_failure_;
    bool code_source_watched(std::uint64_t) const noexcept;
    ExecutionMode mode_{ExecutionMode::X64};
    unsigned processor_count_{1};
    VirtualProcessorIdentity processor_identity_;
    // 僅快取映射查找，不快取權限。任何映射結構變動都必須先失效。
    // relaxed atomic 保留多個主機讀者同時呼叫 const 存取的安全性；映射變動仍需外部同步。
    mutable std::array<std::atomic<const Region*>, native_region_slots> region_cache_{};
    // 低 52 位為頁號，52–53 位為權限；54 位代表需額外範圍檢查的 Region 指標。
    // 完整頁直接使用 addend；短映射冷分支才解參照 Region。
    // 分表避免同一頁的不同權限探查互相逐出；映射改變時所有 worker 已停止。
    using NativePageTable=std::array<std::atomic<NativePageEntry>,native_page_slots>;
    std::array<NativePageTable,3> native_page_cache_{};
    // 產碼只讀固定的三個表指標，表大小不再受 ADD immediate 的 4 KiB 範圍限制。
    std::array<NativePageTable*,3> native_page_tables_{&native_page_cache_[0],&native_page_cache_[1],&native_page_cache_[2]};
    // 低 4 GiB 的完整頁使用直接索引；零項目回退，單一 atomic pointer 不需 tag 配對。
    static constexpr std::size_t native_direct_slots=1U<<20;
    std::shared_ptr<void> native_direct_owner_;
    std::array<std::uintptr_t*,3> native_direct_tables_{};
    std::atomic<bool> native_direct_ready_{};
    std::array<std::uintptr_t,3> native_mmu_bases_{};
    std::uint8_t* native_mmu_permissions_{};
    std::shared_ptr<void> native_mmu_owner_,native_mmu_table_owner_;
    std::uint64_t native_mmu_low_guard_{},native_mmu_high_guard_{};
    inline static std::atomic<bool> native_mmu_implicit_{};
    // 待重建的視窗區段。映像、堆積與堆疊相距數十 GiB，各自重建，不把中間整段一起撤銷與重新映射。
    static constexpr std::size_t native_mmu_dirty_slots=8;
    std::array<std::pair<std::uint64_t,std::uint64_t>,native_mmu_dirty_slots> native_mmu_dirty_{};
    std::size_t native_mmu_dirty_count_{};
    std::vector<std::uint64_t> native_mmu_partial_pages_;
    void mark_native_mmu_dirty(std::uint64_t first,std::uint64_t last) noexcept;
    void rebuild_native_mmu(std::uint64_t first,std::uint64_t last,bool allow_shared,bool fresh);
    bool native_mmu_attempted_{},native_mmu_shared_{},native_mmu_complete_{},native_mmu_direct_{};
    void update_native_mmu_completeness() noexcept;
    void initialize_direct_cache();
    void invalidate_direct_pages(GuestAddress,std::uint64_t) noexcept;
    std::atomic<bool> native_pages_ready_{};
    std::atomic<bool> native_partial_pages_ready_{};
    GenerationStats generation_stats_{};
    std::array<ExecutableWrite,executable_write_log_capacity> executable_write_log_{};
    std::uint64_t executable_write_sequence_{},executable_write_consumed_{};
    void record_executable_write(const Region&,GuestAddress,std::size_t,bool unchanged) noexcept;
    void log_code_range(std::uint64_t address,std::uint64_t size,bool unchanged,bool flush,bool mapping=false) noexcept;
    void cache_native_page(const Region&,GuestAddress,unsigned) noexcept;
    void invalidate_region_cache(bool retain_full_pages=false,bool retain_direct=false) noexcept;
    const Region* locate(GuestAddress address) const noexcept;
    void insert_region(Region region);
    void prepare_cow_write(GuestAddress address,std::size_t size);
    struct ResidencyState;
    std::shared_ptr<ResidencyState> residency_;
    void copy_residency_limits(const GuestMemory&);
    void release_page_locks(GuestAddress,std::size_t) noexcept;
    std::shared_ptr<ResidencyState> refresh_page_locks(std::span<const Region>,GuestAddress,std::size_t) const;
    static std::vector<std::pair<std::uintptr_t,std::uintptr_t>> trim_unlocked_ranges(std::span<const std::pair<std::uintptr_t,std::uintptr_t>>);
    static TrimResult trim_native_spans(std::span<const std::pair<std::uintptr_t,std::uintptr_t>>);
    // 映射變更時更新；Job 輪詢只讀計數。附加在尾端，保留 JIT／SMAP 使用的既有欄位位移。
    std::uint64_t committed_{},private_commit_{};
    std::map<std::uint64_t,std::uint64_t> virtual_reservations_;
    std::uint64_t virtual_size_{},peak_virtual_size_{};
};
}
