#pragma once
#include "x64arm/interpreter.hpp"
#include "x64arm/ir.hpp"
#include "x64arm/smap.hpp"
#include <chrono>
#include "x64arm/platform.hpp"
#include "x64arm/native_chain.hpp"
#include "x64arm/native_chain_profile.hpp"
#include "x64arm/rep_memory.hpp"
#include "x64arm/rep_compare.hpp"
#include <array>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace x64arm {
struct JitStats {
    std::uint64_t shared_regions{},shared_region_batches{},shared_region_instructions{},shared_region_invalidations{};
    std::uint64_t llvm_regions{},llvm_shared_entries{},llvm_instructions{},llvm_batches{},llvm_guard_exits{},llvm_compile_nanoseconds{};
    std::uint64_t llvm_compile_failures{},llvm_compiler_peak_workers{},llvm_hot_samples{};
    std::uint64_t aot_llvm_hits{},aot_llvm_regions{};
    // 首次有界 CFG：入口各自持有恢復／撤銷資料，同批發布並直接串接。
    std::uint64_t first_regions{},first_region_entries{},first_region_links{},first_region_used{},first_region_unused{},first_region_word_stops{};
    std::uint64_t region_allocations{},region_linked_branches{};
    std::uint64_t chain_batches{},chain_blocks{},chain_instructions{},chain_links{},chain_inline_links{};
    // 條件出口的宿主直接連結（X64ARM_JIT_DIRECT_LINK）：建立、略過旗標物化、無法修補、撤銷次數。
    std::uint64_t code_links{},code_links_elided{},code_link_failures{},code_links_revoked{};
    std::uint64_t parallel_batches{}, parallel_instructions{}, parallel_peak_workers{};
    std::uint64_t parallel_service_calls{},parallel_service_exits{};
    std::uint64_t parallel_attempts{},parallel_unavailable{},parallel_short_slices{},parallel_assigned_workers{},parallel_capacity_instructions{};
    std::uint64_t parallel_missing_entries{},parallel_entry_budget_exits{},parallel_observation_exits{},parallel_guard_exits{},parallel_budget_exits{},parallel_zero_workers{};
    std::uint64_t parallel_pool_nanoseconds{},parallel_worker_nanoseconds{},parallel_idle_nanoseconds{};
    std::uint64_t rep_memory_batches{},rep_memory_elements{};
    std::uint64_t rep_compare_batches{},rep_compare_elements{};
    std::uint64_t compiled_prefixes{}, prefix_instructions{}, translated_blocks{}, cache_hits{}, native_instructions{}, fallback_instructions{}, cache_resets{}, fast_scalar_instructions{}, linked_hits{}, compiled_fragments{}, fragment_instructions{}, fragment_exits{}, return_hits{}, return_misses{}, return_overflows{}, smap_hits{}, smap_fragment_hits{}, aot_hits{}, aot_fragment_hits{};
    // 原因計數可以重疊：同一次清除可能同時觀察到世代及模式變動。
    std::uint64_t cache_reset_generation{},cache_reset_ordering{},cache_reset_context{},cache_reset_capacity{},peak_cache_entries{};
    std::uint64_t cache_retained_entries{},cache_retained_bytes{},cache_evicted_entries{};
    std::uint64_t cache_recycle_nanoseconds{},cache_recycle_max_nanoseconds{};
    // 首次翻譯包含 decode／emit／publish／索引與容量回收；其餘三項為獨立的可選升級。
    // decode／emit／publish 為首次翻譯的子項，不可再加到總時間；SMAP 還原可能內含 chain 升級。
    std::uint64_t admission_blocks{},admission_instructions{},admission_promotions{};
    std::uint64_t compile_base_nanoseconds{},compile_upgrade_nanoseconds{},compile_prefix_nanoseconds{},compile_chain_nanoseconds{};
    std::uint64_t compile_decode_nanoseconds{},compile_emit_nanoseconds{},compile_publish_nanoseconds{};
    // 背景編譯壁鐘耗時不加進主執行緒 compile_*；發布仍由協調執行緒執行。
    std::uint64_t compiler_submitted{},compiler_completed{},compiler_stale{},compiler_cancelled{},compiler_queue_full{};
    std::uint64_t compiler_nanoseconds{},compiler_publish_nanoseconds{},compiler_publish_max_nanoseconds{},compiler_peak_workers{};
    // 離線編譯工作數、同時執行 worker 峰值與各份工作耗時總和（不是整段壁鐘時間）。
    std::uint64_t precompile_jobs{},precompile_peak_workers{},precompile_nanoseconds{};
    std::uint64_t cache_cleared_entries{},cache_cleared_bytes{};
    std::uint64_t code_mapping_ranges{};
    std::uint64_t translation_hits{},translation_misses{},translation_stale{},translation_admissions{},translation_evictions{};
    std::uint64_t translation_protected_hits{},translation_restored_fragments{},translation_restored_prefixes{};
    std::uint64_t translation_reused_upgrades{};
    std::uint64_t translation_bytes{},translation_peak_bytes{},translation_restore_nanoseconds{};
    // 可執行寫入分類。每筆寫入恰屬一類：同值、所在頁無任何翻譯、同頁但位元組不重疊、位元組重疊，
    // 或無法分類（身分改變／紀錄溢出）。範圍刷新另計 code_flush_ranges，不計入這五類。
    std::uint64_t code_write_unchanged{},code_write_uncovered{},code_write_page_only{},code_write_overlap{},code_write_unclassified{},code_flush_ranges{};
    // code_write_resets：結構變動清除時恰有待處理寫入的次數。cleared_entries 為寫入實際丟棄的條目
    // （全域清除時為整個快取，範圍撤銷時為重疊條目）；page／overlap 為頁粒度與位元組粒度會丟棄的條目。
    std::uint64_t code_write_resets{},code_write_cleared_entries{},code_write_page_entries{},code_write_overlap_entries{};
    // code_write_events：範圍撤銷處理的紀錄批次；code_write_invalidations：其中實際撤銷了條目的批次。
    std::uint64_t code_write_events{},code_write_invalidations{};
    // 依形狀重新解碼失敗而放棄 fragment／prefix 升級的次數；應恆為 0，非零代表有寫入路徑繞過範圍撤銷。
    std::uint64_t relift_failures{};
    // 定期追加保存持久快取的次數與耗時；被終止的程序也能累積翻譯。
    std::uint64_t smap_autosaves{},smap_autosave_nanoseconds{};
    // 背景預譯：送出的快照入口、worker 完成數、主線採用／等待／比對失敗與捨棄數。
    std::uint64_t speculation_submitted{},speculation_compiled{},speculation_hits{},speculation_waits{},speculation_rejected{},speculation_dropped{};
};
class JitEngine {
public:
    // 預設依宿主記憶體 1/32 預留 64–1024 MiB；僅發布的程式碼會提交實體頁。
    // 容量回收保留最近工作集，避免大型通用 runtime 反覆驅逐仍會使用的冷熱交界碼。
    // 呼叫端仍可指定較小的容量；不依 EXE、模組或客體指令位址決定策略。
    JitEngine();
    explicit JitEngine(std::size_t capacity);
    // 第二層只保存編譯產物，不占可執行 arena；0 可停用，供對照量測。
    JitEngine(std::size_t capacity,std::size_t translation_capacity,DecodeCache* shared_cache=nullptr);
    DecodeCache& decode_cache() const noexcept {return fallback_.decode_cache();}
    ~JitEngine();
    // 0 為同步；1–8 個背景執行緒只處理持有自己 IR 的可選升級，不讀寫客體記憶體。
    // precompile 也採用此 worker 數，僅協調者探索來源／更新 SMAP。
    // 僅可在首次 run 前設定；一般函式庫呼叫預設同步供受控驗證，CLI 預設 2 個 worker。
    void set_compiler_workers(unsigned count);
    // 0 關閉；2 從直接跳躍／回邊探索，4 也探索前向條件分支。數值同時為入口上限。
    // 只能在首次 run 前設定，供同引擎對照。
    void set_first_region(unsigned entries);
    // 首次基本翻譯前先直譯的入口次數；0 保留立即翻譯，與熱區升級門檻分開。
    void set_admission_threshold(unsigned visits);
    ExecutionResult run(GuestState& state, GuestMemory& memory, RunOptions options = {});
    void clear();
    // 僅查詢仍發布的不可變恢復表；signal scope 另外核對故障位址與客體 MMU 視窗。
    std::optional<JitRecoveryLocation> recovery_at(std::uintptr_t pc) const noexcept {return arena_.recovery_at(pc);}
    bool open_smap(const std::filesystem::path& source, bool primary=true, bool load_existing=true) { return smap_.open(source,primary,load_existing); }
    bool save_smap() { return smap_.save(); }
    std::size_t precompile(const GuestMemory&, std::span<const GuestAddress> seeds, GuestAddress begin, std::uint64_t size, const std::function<bool()>& cancelled = {}, bool follow_branches = true, std::uint32_t ordering_mask = 0x3fU);
    const JitStats& stats() const noexcept { return stats_; }
    // 條目只保存形狀；dump 需要同內容的客體記憶體重新解碼 IR，無法解碼時改寫出形狀。
    void dump(const std::filesystem::path& directory, const GuestMemory& memory) const;
    // 在編譯前選用；工作快照傳遞給 worker，執行中不讀共享可變設定。
    void retain_llvm_ir(bool enabled=true) { retain_llvm_ir_=enabled; }
    void enable_profiling(bool native=true) { profiling_ = native;fallback_profiling_=true; }
    void dump_profile(const std::filesystem::path& path) const;
    // 取樣前保存序號，取樣後只解析仍存活且較早發布的區塊，不把重用的 host PC 誤認為舊碼。
    std::uint64_t native_publication_serial() const noexcept {return arena_.publication_serial_;}
    void dump_native_samples(const std::filesystem::path&,const GuestMemory&,std::span<const std::uintptr_t>,std::uint64_t serial_limit) const;
private:
    struct SharedRegions;
    std::unique_ptr<SharedRegions> shared_regions_;
    std::uint64_t run_shared_region(GuestState&,GuestMemory&,const RunOptions&,std::uint64_t budget);
    struct ParallelPool;
    std::unique_ptr<ParallelPool> parallel_;
    struct CompileJob;
    struct CompilerPool;
    std::unique_ptr<CompilerPool> compiler_;
    std::unique_ptr<CompilerPool> llvm_compiler_;
    unsigned compiler_workers_{};
    // 程序設定已固定；熱派送只讀引擎的模式，不重複查詢 LLVM 控制函式。
    enum class UpgradeMode : std::uint8_t {Legacy,Combined,LlvmTiered};
    UpgradeMode upgrade_mode_{UpgradeMode::Legacy};
    bool retain_llvm_ir_{};
    unsigned first_region_entries_{};
    unsigned admission_threshold_{};
    struct ColdEntry {std::uint64_t end{};unsigned visits{},instructions{};};
    // 只保留入口熱度與界線，不保存每條 IR；總數有界，程式碼改寫依範圍撤銷。
    std::map<std::uint64_t,ColdEntry> cold_;
    bool immediate_chain_{};
    std::uint64_t compiler_serial_{};
    // 背景預譯：worker 由程式碼頁快照沿靜態後繼預先 lift／emit／prepare；主線比對客體位元組後發布。
    struct Speculation;
    struct SpeculativeBlock { IrBlock block; std::vector<std::uint32_t> words; std::shared_ptr<Arm64Recovery> recovery;
        std::shared_ptr<const JitMemory::PreparedBlock> prepared; MemoryOrdering ordering{}; };
    std::unique_ptr<Speculation> speculation_;
    unsigned speculation_workers_{};
    std::optional<SpeculativeBlock> take_speculation(std::uint64_t rip,const GuestMemory&,const RunOptions&);
    void speculate_successors(const IrBlock&,const GuestMemory&);
    static void compile_upgrade(CompileJob&);
    void publish_upgrade(CompileJob&,const GuestMemory&);
    void poll_compiler(const GuestMemory&);
    std::pair<std::uint64_t,std::uint64_t> run_parallel_batch(GuestState&,GuestMemory&,const RunOptions&,std::uint64_t budget);
    SmapCache smap_;
    // X64ARM_SMAP_AUTOSAVE 秒數（預設 15，0 停用）：有新翻譯且超過間隔時以追加段寫出，
    // 不依賴正常退出；只在協調執行緒於發布／派送邊界呼叫。
    unsigned smap_autosave_seconds_{};
    std::chrono::steady_clock::time_point smap_autosave_last_{};
    std::uint32_t smap_autosave_tick_{};
    void autosave_smap();
    // 常駐條目不保存完整 IR（每條約 300 bytes 的 Instruction）。範圍撤銷保證條目存活期間來源位元組不變，
    // 需要 IR 時依形狀以 relift 重新解碼即可；常駐只留線性片段與派送熱路徑需要的終結指令資訊。
    struct CodeShape {
        std::uint64_t entry{};std::uint32_t count{};std::vector<IrSpan> spans;
        // 編譯當下的精確來源位元組，第二層（translation_cache）命中時驗證；不從已變動的客體碼建立舊翻譯的指紋。
        std::vector<std::uint8_t> source;
        // 終結指令：CALL 的返回位址與推入大小、RET 的額外清理位元組。
        Opcode terminal{Opcode::Nop};std::uint64_t next_rip{};std::uint16_t call_bytes{},ret_extra{};bool breakpoint{};
        RepMemoryForm rep_memory{};
        StringCompareForm string_compare{};
        bool atomic_entry{};
        static CodeShape of(const IrBlock&);
    };
    IrBlock materialize(const GuestMemory&,const CodeShape&,std::size_t limit=SIZE_MAX) const;
    // 發布時解析一次作用域；單次派送不再讀取與解碼原始機器碼描述字。
    static unsigned native_scopes(std::span<const std::uint32_t>) noexcept;
    struct Fragment { CodeShape shape; std::vector<std::uint32_t> words; JitMemory::ChainBody invoke; std::shared_ptr<const Arm64Recovery> recovery;std::size_t offset;unsigned scopes;std::uint64_t exit_hint{};
        Fragment(const IrBlock& block,std::vector<std::uint32_t> code,JitMemory::ChainBody function,std::shared_ptr<const Arm64Recovery> table,std::size_t at):shape(CodeShape::of(block)),words(std::move(code)),invoke(function),recovery(std::move(table)),offset(at),scopes(native_scopes(words)){}
        Fragment(CodeShape code_shape,std::vector<std::uint32_t> code,JitMemory::ChainBody function,std::shared_ptr<const Arm64Recovery> table,std::size_t at):shape(std::move(code_shape)),words(std::move(code)),invoke(function),recovery(std::move(table)),offset(at),scopes(native_scopes(words)){}
    };
    struct Entry { CodeShape shape; std::vector<std::uint32_t> words; std::size_t offset; JitMemory::ChainBody invoke; std::array<Entry*,2> successors{}; unsigned replace{}, executions{}; Entry* return_entry{}; std::optional<Fragment> fragment{}; NativeChainNode chain{}; bool chain_checked{}; std::shared_ptr<const Arm64Recovery> recovery{};
        unsigned scopes{native_scopes(words)};
        // 共用入口持有同一發布權杖，最後一個擁有者才撤銷機器碼與恢復側表。
        struct LlvmCode {CodeShape shape;std::shared_ptr<const LlvmRegion> artifact;LlvmRegion::Function invoke;std::size_t offset;std::shared_ptr<const void> publication;};
        std::optional<LlvmCode> llvm{};
        bool llvm_checked{};
        bool llvm_ready{};unsigned llvm_samples{};
        bool first_region{},first_region_used{};
        // 切片尾端僅快取 1、2、4、8、16 指令前綴，限制附加程式碼量。
        std::array<std::unique_ptr<Fragment>,5> prefixes{};
        // 升級後的鏈結快取會記錄整段 fragment 出口，不能拿來覆寫基本區塊的訓練邊。
        std::array<Entry*,2> region_successors{};std::array<unsigned,2> successor_hits{};
        // 多態控制流只保存候選客體 RIP；不持有可失效的 Entry／機器碼指標。
        // 工作快照會重新解碼來源，執行時仍驗證真正的間接目標。
        struct ObservedTarget {std::uint64_t rip;unsigned hits;};
        std::vector<ObservedTarget> indirect_targets{};
        // 每條出邊嵌入反向串列，不另配節點；撤銷只走實際入邊，避免掃描全部快取。
        struct Link {Entry* source{};Link* next{};Link** previous{};};
        std::array<Link,5> links{};Link* incoming{};
        bool translation_reused{},hot_base{},atomic_guarded{};
        bool upgrade_checked{};std::uint64_t upgrade_serial{};
        // 回收取樣環只在協調安全點維護；Entry 位址不受 unordered_map rehash 影響。
        Entry* recycle_next{};Entry* recycle_previous{};
        // 入口在任何讀取、離開或陷阱之前即重新定義六個算術旗標；直接連結可略過來源出口的旗標物化。
        bool flags_dead{};
        // 指向本入口節點的直接連結 id；入口撤銷時逐一還原連結點。
        std::vector<std::uint32_t> code_in{};
    };
    struct TranslationCode {CodeShape shape;std::vector<std::uint32_t> words;std::shared_ptr<const Arm64Recovery> recovery;};
    IrBlock llvm_control_flow_source(const Entry&,const HostServices*,const GuestMemory&,std::vector<LlvmRegionEdge>&,
                                    bool offline=false,GuestAddress begin=GuestAddress{},std::uint64_t size=UINT64_MAX,
                                    bool* reused_llvm=nullptr) const;
    void install_llvm(Entry&,const std::shared_ptr<const LlvmRegion>&,const std::shared_ptr<const JitMemory::PreparedBlock>&,bool offline);
    void sample_llvm(Entry&,const HostServices*,const GuestMemory&);
    bool base_upgrade_pending(const Entry&) const noexcept;
    struct TranslationEntry {
        TranslationCode base;std::optional<TranslationCode> fragment;
        std::array<std::unique_ptr<TranslationCode>,5> prefixes{};
        std::size_t bytes{};unsigned executions{};bool frequent{},hot_base{};
        std::list<std::uint64_t>::iterator position;
    };
    std::unordered_map<std::uint64_t,TranslationEntry> translations_;
    std::list<std::uint64_t> translation_recent_,translation_frequent_;
    std::size_t translation_capacity_{},translation_bytes_{},translation_frequent_bytes_{};
    void clear_active(bool preserve_translations);
    void remember_translation(Entry&);
    void erase_translation(std::unordered_map<std::uint64_t,TranslationEntry>::iterator);
    void trim_translations();
    void touch_translation(TranslationEntry&);
    void consume_translation(std::unordered_map<std::uint64_t,TranslationEntry>::iterator,std::size_t bytes);
    bool valid_translation(const TranslationCode&,const GuestMemory&)const;
    Entry* restore_translation(std::uint64_t,const GuestMemory&);
    bool restore_fragment(Entry&,const GuestMemory&);
    bool restore_prefix(Entry&,std::size_t,const GuestMemory&);
    static void set_link(Entry&,unsigned slot,Entry* target);
    static void observe_region_target(Entry&,std::uint64_t target);
    void unlink_entry(Entry&);
    // 直接連結：來源區塊（已發布 offset）的條件出口樁改跳 thunk，直接進入目標節點。
    struct CodeLink {std::size_t block{};Entry* source{};Entry* target{};std::uint32_t index{};JitMemory::ExitLink link{};bool live{};};
    std::vector<CodeLink> code_links_;std::vector<std::uint32_t> free_code_links_;
    std::unordered_map<std::size_t,std::vector<std::uint32_t>> block_code_links_;
    bool direct_links_allowed() const noexcept;
    void link_entry_exits(Entry&);
    void create_code_link(Entry& source,std::size_t block,std::uint32_t index,Entry& target);
    void drop_block_links(std::size_t block);
    void revoke_entry_links(Entry&);
    void reset_code_links() noexcept;
    void link_recycle_entry(Entry&) noexcept;
    struct ChainResult {std::uint64_t completed;Entry* last;bool unchanged;};
    std::optional<ChainResult> run_chain(Entry&,const Fragment*,GuestState&,GuestMemory&,const RunOptions&,std::uint64_t,bool memory_recovery);
    struct ReturnFrame { std::uint64_t rip{}, sp{}; Entry* caller{}; };
    std::array<ReturnFrame,256> returns_{};
    std::size_t return_depth_{};
    Entry* resolve_entry(std::uint64_t rip, GuestMemory& memory, const RunOptions& options,const GuestState* state=nullptr);
    Entry* compile_first_region(IrBlock&,GuestMemory&,const RunOptions&);
    void observe_chain(Entry&,std::uint64_t next,bool complete,const HostServices*,const GuestMemory&);
    void remember_atomic_forms(const IrBlock&);
    void recycle_capacity(std::size_t required);
    void discard_entry_code(const Entry&);
    // 翻譯來源頁索引：每個客體頁一張已翻譯位元組點陣圖與擁有者入口（cache_ 的鍵）。
    // 可執行寫入先測點陣圖，未命中即不需任何撤銷；命中才逐一精確比對擁有者的 IR 範圍。
    // 撤銷後只重建被測過的頁；其他頁的殘留位元／擁有者只會多做一次精確比對，不會漏撤銷。
    struct CodePage {std::array<std::uint64_t,64> bytes{};std::vector<std::uint64_t> owners;std::size_t retired_owners{};};
    std::unordered_map<std::uint64_t,CodePage> code_pages_;
    void index_code(std::uint64_t owner,const CodeShape&);
    void rebuild_code_page(std::uint64_t page);
    static bool covers(const Entry&,std::uint64_t begin,std::uint64_t end);
    enum class CodeWrites : std::uint8_t {Invalidate,BeforeReset,Incomparable};
    bool process_code_writes(GuestMemory&,CodeWrites);
    const Fragment* compile_prefix(Entry&,std::uint64_t budget,const GuestMemory&);
    void compile_chain(Entry&,const Fragment*);
    void compile_fragment(Entry& entry, const HostServices* services, const GuestMemory& memory);
    enum class DispatchKind : std::uint8_t { Empty, Interpreted, Cold, Native, GuardedAtomic };
    struct DispatchEntry {
        std::uint64_t rip{};
        Entry* entry{};
        DispatchKind kind{DispatchKind::Empty};
        const Instruction* scalar{};
    };
    // 完整 RIP tag 區分碰撞；Entry 指標在 unordered_map rehash 後仍有效。
    static constexpr std::size_t dispatch_capacity = native_dispatch_capacity;
    // 入口表的記憶體獨立配置，避免擴表佔用客體協調執行緒的宿主堆疊。
    std::vector<DispatchEntry> dispatch_cache_ = std::vector<DispatchEntry>(dispatch_capacity);
    std::vector<NativeChainNode*> chain_dispatch_ = std::vector<NativeChainNode*>(native_chain_dispatch_capacity);
    void clear_dispatch_cache() noexcept;
    JitMemory arena_;
    Interpreter fallback_{true};
    std::unordered_map<std::uint64_t, Entry> cache_;
    // 每次只從環上擷取有界冷碼候選；重新使用過的條目跳過，不掃描全部快取。
    std::vector<std::pair<std::uint64_t,std::uint64_t>> recycle_candidates_;
    Entry* recycle_cursor_{};
    std::unordered_set<std::uint64_t> interpreted_;
    std::unordered_map<std::uint64_t,Instruction> scalar_;
    // generation_ 比對的是 GuestMemory::code_structure_generation()；可執行寫入改走範圍撤銷。
    std::uint64_t identity_{}, generation_{};
    MemoryOrdering ordering_{MemoryOrdering::Tso};
    ExecutionMode translation_mode_{ExecutionMode::X64};
    GuestAddress sentinel_{};
    HostServices* services_{};
    JitStats stats_;
    std::uint64_t use_clock_{};
    struct ProfileEntry {
        std::uint64_t calls{}, instructions{}, fallback_calls{}, native_instructions{}, fallback_instructions{}, fragment_calls{}, parallel_calls{}, native_bailouts{}, host_calls{};
        std::uint64_t llvm_calls{}, llvm_instructions{};
        bool native{}; std::string text, bytes;
    };
    void profile_native(const CodeShape&,std::uint64_t executed,bool fragment,bool parallel,bool side_exit,const GuestMemory&,std::uint32_t execution_limit=0,bool llvm=false,std::size_t publication_offset=SIZE_MAX);
    bool profiling_{},fallback_profiling_{};
    std::unordered_map<std::uint64_t, ProfileEntry> profile_;
    // 僅入口 profiling 啟用時累計，依發布序號及客體入口區分替換版本與共用入口。
    struct PublicationProfile {std::uint64_t calls{},instructions{};};
    using PublicationKey=std::tuple<std::uint64_t,std::uint64_t,bool>;
    std::map<PublicationKey,PublicationProfile> publication_profile_;
    NativeChainProfile chain_profile_;
    void profile_chain(const NativeChainContext&,NativeChainProfile&) const;
};
std::string format_stats(const JitStats& stats);
}
