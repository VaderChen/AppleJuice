#pragma once
#include "x64arm/interpreter.hpp"
#include <memory>
#include <filesystem>

namespace x64arm {
// 新模組 AOT 的獨立執行器；共用 CPU／服務語義，不經舊 JIT 的逐區塊模板或 LLVM tier。
class AotEngine final {
public:
    struct Stats {
        // 一般入口表與精確續行表分開計數；format_stats 將實際編譯總量合併輸出。
        std::uint64_t modules{},compiled_instructions{},compiled_words{},decode_nanoseconds{},compile_nanoseconds{},publish_nanoseconds{};
        std::uint64_t native_instructions{},fallback_instructions{},native_entries{},guard_exits{},hardware_faults{},invalidations{},capacity_resets{};
        // 原生續發次數／指令額度及真正回到宿主的配額出口；不把續發算成完整入口。
        std::uint64_t budget_renewals{},budget_reissued_instructions{},native_budget_exits{},native_precise_exits{};
        // 真正無配額的 resident 框架與必要控制邊界；不冒充退休指令數。
        std::uint64_t resident_runs{},resident_callbacks{},resident_precise_callbacks{};
        std::uint64_t resident_timestamps{};
        // 無配額平行批次；CPU 時間以每個宿主 worker 的執行緒時鐘累計。
        std::uint64_t parallel_batches{},parallel_workers{},parallel_peak_workers{},parallel_interrupts{};
        // 協調者在信箱閒置等待時因外部世代改變（跨程序管道／物件通知）提前輪詢的次數。
        std::uint64_t parallel_external_wakes{};
        std::uint64_t parallel_wall_ns{},parallel_cpu_ns{},parallel_sites{};
        // 不經 quiesce（worker 執行中）完成的純新增發布次數。
        std::uint64_t parallel_lockfree_publications{};
        // 不停其他 worker 完成的協調服務（虛擬記憶體家族）次數。
        std::uint64_t parallel_local_services{};
        // 利用率退避：第二核沒有運算而離開平行批次的次數、累計退避時間與對應的 quiesce 次數。
        std::uint64_t parallel_backoffs{},parallel_backoff_ns{},parallel_quiesce_backoff{};
        // 各 quiesce 呼叫點的次數（診斷哪一類出口在中斷 worker）。
        std::uint64_t parallel_quiesce_external_wake{},parallel_quiesce_timeout{},parallel_quiesce_service_bounce{},parallel_quiesce_mapping_refused{},parallel_quiesce_mapping_consistent{},parallel_quiesce_local_fallback{},parallel_quiesce_full_exit{},parallel_quiesce_publish_fallback{},parallel_quiesce_batch_end{};
        std::uint64_t parallel_messages{},parallel_resumes{},parallel_publications{},parallel_service_retries{};
        std::uint64_t parallel_coordinated_services{},parallel_coordinated_semantics{};
        std::uint64_t parallel_mapping_batches{},parallel_mapping_requests{};
        std::uint64_t parallel_rebindings{},parallel_rebound_contexts{},parallel_local_runs{};
        std::uint64_t parallel_preemptions{},parallel_preempt_idle{},parallel_preempt_fallbacks{};
        std::uint64_t capacity_growths{},cache_capacity_bytes{},cache_reservation_bytes{},peak_cache_bytes{};
        std::uint64_t cache_validation_nanoseconds{},code_invalidations{},unaffected_code_events{};
        // 全清原因分開計數：紀錄遺失（落後環狀紀錄容量）、結構世代、MMU 存取模式、其他情境（身分／服務／模式／排序）。
        std::uint64_t log_lost_invalidations{},context_resets_structure{},context_resets_access{},context_resets_context{};
        // 原生碼因改碼返回的批次數、觀察間隔最多筆數及真正取不回的紀錄總數；不是被撤銷模組數。
        std::uint64_t native_invalidated_exits{},code_write_max_pending{},log_lost_entries{};
        // 已發布來源／依賴涵蓋，以及局部撤銷、保留與安全邊界回收的累計數；不是動態分支次數。
        std::uint64_t fragment_tracked_modules{},fragment_dependencies{},fragment_source_invalidations{},fragment_invalidated_modules{};
        std::uint64_t fragment_preserved_modules{},fragment_reclaims{},fragment_deferred_modules{},fragment_deferred_arenas{};
        // 常駐碼直接完成的宿主葉節點服務；仍計入 fallback_instructions，與宿主迴圈服務口徑一致。
        std::uint64_t resident_services{};
        std::uint64_t scalar_native_services{},scalar_native_retries{};
        // 已登錄為核心 ARM64 配方本體的純量服務入口數；配方每次呼叫不進 C++，不逐次計數。
        std::uint64_t scalar_recipe_bodies{};
        // 已登錄的唯讀時鐘 ARM64 入口；每次呼叫不進 provider，也不逐次計數。
        std::uint64_t clock_recipe_bodies{};
        // 只在明確開啟探索診斷時逐段計時；正式計時須維持關閉。
        std::uint64_t profiled_discoveries{},decode_fetch_nanoseconds{},decode_ir_nanoseconds{},decode_index_nanoseconds{},decode_copy_nanoseconds{};
        std::uint64_t profiled_decoder_calls{},decode_decoder_nanoseconds{},decode_validation_nanoseconds{},decode_store_nanoseconds{};
        std::uint64_t decode_index_tombstones{},decode_index_compactions{},decode_evictions{},decode_hits{};
        std::uint64_t decode_admission_deferred{},decode_admissions{};
        // 只在明確開啟產碼診斷時累計；正式執行不啟用各階段時鐘。
        std::uint64_t profiled_modules{},compile_index_ns{},compile_cfg_ns{},compile_liveness_ns{};
        std::uint64_t compile_emission_ns{},compile_relocation_ns{},compile_metadata_ns{};
        std::uint64_t indirect_discoveries{},indirect_candidates{};
        // 探索分類的靜態計數，不表示執行期目標集合完整或節省的耗時。
        std::uint64_t cfg_classified_regions{},cfg_bound_tables{},cfg_table_bytes{},cfg_table_truncated{},cfg_data_skips{};
        // precise_native_entries 只計宿主直接進入精確本體；原生查表續行不另算一次進入。
        std::uint64_t precise_modules{},precise_compiled_instructions{},precise_compiled_words{},precise_native_entries{};
        // 已發布的來源片段數；定位涵蓋範圍，不表示任意 PC 可以恢復架構狀態。
        std::uint64_t pc_fragments{};
        std::uint64_t input_flag_recoveries{};
        // 靜態暖迴圈本體數，包含快取還原；不代表實際執行輪次或節省耗時。
        std::uint64_t counted_memory_loops{};
        // 發布時固定出口綁定數；不是動態執行次數，far 是其中使用原生 literal 尾跳者。
        std::uint64_t direct_edges{},direct_far_edges{},late_edges{},continuous_modules{};
        std::uint64_t persistent_hits{},persistent_misses{},restored_words{},persistent_load_ns{},persistent_save_ns{},persistent_saved{},persistent_save_failures{};
        std::uint64_t prepared_modules{},prepared_words{},prepared_rejections{},prepared_cache_loads{};
        std::uint64_t image_cache_opens{},image_cache_loads{},image_cache_rejections{},image_persistent_hits{},image_persistent_saved{};
        // 記憶體快取（X64ARM_AOT_MEMORY_CACHE）：命中、未命中、存入、核對失敗與無法開啟區段的次數。
        std::uint64_t memory_cache_hits{},memory_cache_misses{},memory_cache_stores{},memory_cache_rejects{},memory_cache_unavailable{};
        // 記憶體快取命中時核對位元組、解壓與反序列化的累計時間（不含發布）。
        std::uint64_t memory_cache_restore_ns{};
        // 背景預編譯（X64ARM_AOT_BACKGROUND_COMPILE）：排入工作、背景存入、命中背景記錄、
        // 前景等待背景完成同一入口，以及佇列滿或範圍過大而捨棄的目標數。
        std::uint64_t background_jobs{},background_stores{},background_hits{},background_waits{},background_drops{};
        // 依預測類別（跳轉／落下出口、直接 call、IAT call）分列的排入與命中數。
        std::array<std::uint64_t,3> background_jobs_by_kind{},background_hits_by_kind{};
        // 發布時計數；calls 是靜態修補點，不是動態呼叫次數。compiled_words 仍代表自足模板。
        // published_words 不含池及尾端對齊，兩者另列，避免把壓縮字數當成配置容量收益。
        std::uint64_t runtime_pools{},runtime_linked_modules{},runtime_calls{},runtime_saved_words{},runtime_pool_bytes{},runtime_padding_words{},runtime_alignment_bytes{},published_words{};
        // resident REP 的動態批次／退休元素／NEON 搬移或填入 bytes；precise_calls 是單元素回退嘗試數。
        std::uint64_t rep_memory_batches{},rep_memory_elements{},rep_memory_neon_bytes{},rep_memory_precise_calls{};
        // 缺少函式範圍時延後探索的直接 CALL 目標；是編譯期邊數，不是省下的指令或耗時。
        std::uint64_t deferred_call_targets{};
        // 覆蓋命中為探索資格檢查次數；私有指令為累積發布數，皆非唯一位址數。
        std::uint64_t private_coverage_hits{},demand_suffix_modules{},private_entry_instructions{};
    };
    struct CacheLimits {
        std::size_t initial=64*1024*1024,maximum=1024*1024*1024;
    };
    // 預設預留有上限的虛擬空間，實際產碼需求增加時才擴充可用容量，保留原生位址。
    AotEngine();
    // 顯式 capacity 仍是固定上限；需要成長時明確傳入 CacheLimits。
    // 區域最多探索 region_instructions 條。有合法服務提示時保留其範圍內控制流，
    // 否則預設限制於入口所在客體頁。範圍外控制流由已發布入口表續行。
    explicit AotEngine(std::size_t capacity,std::size_t region_instructions=4096,DecodeCache* shared_cache=nullptr);
    explicit AotEngine(CacheLimits,std::size_t region_instructions=4096,DecodeCache* shared_cache=nullptr);
    DecodeCache& decode_cache() const noexcept;
    ~AotEngine();
    AotEngine(const AotEngine&)=delete;
    AotEngine& operator=(const AotEngine&)=delete;
    ExecutionResult run(GuestState&,GuestMemory&,RunOptions={});
    const Stats& stats() const noexcept;
    void dump(const std::filesystem::path&) const;
    void clear();
    bool open_smap(const std::filesystem::path&,bool primary=true,bool load_existing=true);
    // 離線產物獨立保存；run 只讀取準備層，缺漏補譯沿用另一個暖快取。
    bool open_prepared_smap(const std::filesystem::path&,bool load_existing=true);
    bool save_smap();
    // 只準備可持久化模組，不發布或執行客體碼；缺少入口仍由 run 共用流程補譯。
    // regions 是已驗證、依 begin 排序且不重疊的可選函式提示。
    std::size_t precompile(GuestMemory&,std::span<const GuestAddress> seeds,GuestAddress begin,std::uint64_t size,
                          std::span<const TranslationRegion> regions={},const std::function<bool()>& cancelled={},
                          bool follow_branches=true);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::string format_stats(const AotEngine::Stats&);
}
