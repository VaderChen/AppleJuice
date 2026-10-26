#pragma once
#include "x64arm/memory.hpp"
#include "x64arm/execution_policy.hpp"
#include "x64arm/result.hpp"
#include "x64arm/resident_heap.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <filesystem>
#include <array>

// 服務層以此判斷 SDK 是否提供 HostServices::resident_service，舊 SDK 仍可編譯。
#define X64ARM_HOST_SERVICES_RESIDENT_SERVICE 1
#define X64ARM_HOST_SERVICES_RESIDENT_HOST_FP 1
#define X64ARM_HOST_SERVICES_MANAGED_FP 1
#define X64ARM_HOST_SERVICES_RETAINED_FP 1
#define X64ARM_HOST_SERVICES_COMMITTED_FP_STATUS 1
#define X64ARM_HOST_SERVICES_SCALAR_NATIVE 1
// 服務層以此判斷 SDK 是否支援純量原生配方：核心內的 ARM64 葉常式直接完成已知 CRT 數學入口。
#define X64ARM_HOST_SERVICES_SCALAR_RECIPE 1
#define X64ARM_HOST_SERVICES_CLOCK_RECIPE 1
#define X64ARM_HOST_SERVICES_SCALAR_ERRORS 1
#define X64ARM_HOST_SERVICES_PARALLEL_SERVICE 1
#define X64ARM_HOST_SERVICES_TRANSLATION_REGION 1
#define X64ARM_HOST_SERVICES_TRANSLATION_IMAGE 1
#define X64ARM_HOST_SERVICES_NATIVE_TIME_SLICE 1
#define X64ARM_HOST_SERVICES_RESIDENT_EXECUTION 1
#define X64ARM_HOST_SERVICES_RESIDENT_PARALLEL 1
#define X64ARM_HOST_SERVICES_RESIDENT_TIMESTAMP 1
#define X64ARM_HOST_SERVICES_RESIDENT_ACTIVE_CONTEXTS 1
#define X64ARM_HOST_SERVICES_COORDINATED_SERVICE 1
#define X64ARM_HOST_SERVICES_RESIDENT_SERVICE_REQUEST 1
#define X64ARM_HOST_SERVICES_RESIDENT_CONTROL_REQUEST 1
#define X64ARM_HOST_SERVICES_RESIDENT_CONTEXT_ERRORS 1
#define X64ARM_HOST_SERVICES_RESIDENT_CONTROL_DRAIN 1
#define X64ARM_HOST_SERVICES_RESIDENT_PREEMPT 1
#define X64ARM_HOST_SERVICES_RESIDENT_BLOCK 1
#define X64ARM_HOST_SERVICES_RESIDENT_CONTEXT_SCALAR 1
#define X64ARM_HOST_SERVICES_RESIDENT_IDLE_WAKE 1
#define X64ARM_HOST_SERVICES_RESIDENT_HEAP 1
namespace x64arm {
// 可選產碼範圍提示，end 不包含在範圍內。只影響探索分組，不授予執行權限。
struct TranslationRegion {GuestAddress begin,end;};
// 載入器已授權的不可變來源身分；宿主路徑不交給客體。載入差值不是原生碼重定位。
struct TranslationImage {
    std::filesystem::path source;
    std::array<std::uint8_t,32> identity{};
    GuestAddress begin{},preferred{};
    std::uint64_t size{};
    bool operator==(const TranslationImage&) const=default;
};
inline constexpr std::uint32_t cpu_system_call_trap = 0x10000000U;
inline constexpr std::uint32_t cpu_system_enter_trap = 0x10000001U;
inline constexpr std::uint32_t cpu_software_interrupt_trap = 0x10000100U;
enum class SystemCallEntry { Syscall, Sysenter, Interrupt };
enum class ResidentServiceProgress {Completed,Retry,Coordinator};
// 含睡眠的宿主連續時鐘，毫秒自宿主開機起算；DWORD 自然回繞。
// 核心依客體模式選擇 EAX、EDX:EAX 或 RAX，沒有參數、LastError 或共享時間頁副作用。
enum class ResidentClockRecipe : unsigned {None=0,Milliseconds32=1,Milliseconds64=2,PerformanceCounter=3};
class HostCallFault : public std::runtime_error { using std::runtime_error::runtime_error; };
struct CpuFault : std::runtime_error {
    std::uint32_t code;
    CpuFault(std::uint32_t value, const char* message) : std::runtime_error(message), code(value) {}
};
// 常駐 ARM64 邊界借出的原宿主環境；僅在本次 invoke_managed_service 期間有效。
// 服務可依自己的計算契約建立環境；核心在正常／例外返回後統一恢復客體環境。
struct ResidentHostEnvironment {
    std::uint64_t control{},status{};
    // 預設必須重建。只有本次 helper 的全部路徑沒有改動 FPCR/FPSR，才可明確保留；
    // SIMD 資料搬移可用，浮點計算必須證明不產生旗標。核心另核對完整 MXCSR。
    // 這是借出的回報欄位，不得保存到本次呼叫之外，也不能先標記再呼叫未知外部函式。
    mutable std::uint64_t retained{};
    void retain() const noexcept {retained=1;}
    // 只保留已提交的 FPSR，不承諾 FPCR 未變。呼叫者須先把所有例外位完整併入
    // MXCSR，之後不得再產生未提交的例外；核心僅在全遮罩惰性模式採用，仍重建 FPCR。
    // 此宣告會撤回完整環境保留；restore() 也會撤回本宣告。
    void retain_committed_status() const noexcept {retained=2;}
    void restore() const {
        retained=0;
#if defined(__aarch64__) && defined(__APPLE__)
        asm volatile("msr fpcr, %0\nmsr fpsr, %1"::"r"(control),"r"(status):"memory");
#else
        throw HostCallFault("此平台不支援常駐 ARM64 浮點邊界");
#endif
    }
};
static_assert(sizeof(ResidentHostEnvironment)==24 && offsetof(ResidentHostEnvironment,status)==8 &&
              offsetof(ResidentHostEnvironment,retained)==16);
// 純量葉節點的值契約；不借出完整 GuestState，服務不得變更 RC／遮罩、排程或映射。
// 輸入 first/second 為 XMM0/1 低 64 位。width=0/4/8 分別為整數／float／double 回傳；
// 成功才提交 integer、result、next、MXCSR sticky 與必要 errno。失敗必須沒有客體可見效果。
// environment 僅借用本次呼叫；宿主 FP 一律由核心恢復。堆疊與 ABI 的檢查由服務層負責。
// resident_scalar_recipe 的編號；核心依此選擇常駐 ARM64 配方本體。
enum class ScalarRecipe : unsigned { None=0, ExpF=1, Log2=2, LogF=3, SinF=4, CosF=5, LRintF=6, LRint=7, LRoundF=8, LRound=9, AcosF=10, PowF=11, Hypot=12, Cos=13, Sin=14, Log=15, Exp=16, AtanF=17, Atan=18, Atan2F=19, Atan2=20, Acos=21 };
// 每批原生執行借用的錯誤格位。cell 必須來自 native_pointer 的私人可寫 4-byte 範圍；
// 非空時，客體位址與宿主指標須對應同一儲存，生命期及執行緒身分維持到批次出口。
// 映射／執行緒變更必須沿既有世代或 concurrency_epoch 令批次退出；不支援時回傳空值。
struct ResidentScalarErrors {
    std::uint32_t* cell{};
    std::uint64_t guest_address{};
    std::uint32_t domain{},range{};
};
struct ResidentScalarCall {
    std::uint64_t entry{},stack{},first{},second{},integer{},result{};
    std::uint32_t mxcsr{},width{};
    std::uint64_t next{};
    ResidentHostEnvironment environment{};
};
static_assert(sizeof(ResidentScalarCall)==88 && offsetof(ResidentScalarCall,first)==16 &&
              offsetof(ResidentScalarCall,integer)==32 && offsetof(ResidentScalarCall,mxcsr)==48 &&
              offsetof(ResidentScalarCall,next)==56 && offsetof(ResidentScalarCall,environment)==64);
// 常駐執行期間的外部喚醒來源（跨程序 IPC、裝置完成等）。只由中斷通知者執行緒呼叫，
// 共享所有權保證等待期間有效；世代與 observed 不同或逾時才返回，回傳世代是否已改變。
class ResidentExternalWake {
public:
    virtual ~ResidentExternalWake()=default;
    virtual bool wait_changed(std::uint64_t observed,std::uint64_t nanoseconds) const noexcept=0;
    // 不等待，只核對世代是否已不同於 observed；協調者持信箱鎖時呼叫，不得阻塞。
    virtual bool changed(std::uint64_t observed) const noexcept {return wait_changed(observed,0);}
};
// CPU 只認識受控客體入口，Windows ABI 與 API 語義由服務層負責。
struct ResidentPreemptPlan {std::size_t victim{SIZE_MAX};GuestState* replacement{};};
class HostServices {
    std::uint64_t concurrency_epoch_{};
    std::uint64_t nested_instructions_{};
public:
    using NestedStop = std::function<bool()>;
    using NestedRunner = std::function<ExecutionResult(GuestState&,GuestMemory&,const NestedStop&)>;
private:
    NestedRunner nested_runner_;
public:
    // 原生模態迴圈借用目前 CPU 後端；作用域離開時還原外層執行器。
    NestedRunner exchange_nested_runner(NestedRunner runner) {auto previous=std::move(nested_runner_);nested_runner_=std::move(runner);return previous;}
    bool has_nested_runner() const noexcept {return bool(nested_runner_);}
    std::uint64_t nested_instruction_count() const noexcept {return nested_instructions_;}
    void account_nested_instructions(std::uint64_t count) noexcept {nested_instructions_+=count;}
    ExecutionResult run_nested(GuestState& state,GuestMemory& memory,const NestedStop& stop) {
        if(!nested_runner_)throw HostCallFault("目前 CPU 執行範圍不支援原生模態回呼");
        // 巢狀 run 會暫時交換 runner；複製後再呼叫，避免釋放執行中的 closure。
        const auto runner=nested_runner_;return runner(state,memory,stop);
    }
    // 排程／服務只能在協調執行緒變更；變更前通知，派送器於返回邊界重新取得排序模式。
    std::uint64_t concurrency_epoch() const noexcept { return concurrency_epoch_; }
    void notify_concurrency_changed() noexcept { ++concurrency_epoch_; }
    virtual ~HostServices() = default;
    virtual void retired(GuestMemory&, std::uint64_t) {}
    virtual void after_instructions(GuestState&, GuestMemory& memory, std::uint64_t count) { retired(memory, count); }
    // 原生區塊批次執行期間，服務層與記憶體映射保持不變；所有 worker 返回後才回呼。
    virtual bool concurrent_guest_threads() const noexcept { return false; }
    virtual unsigned parallel_workers() const noexcept { return 1; }
    virtual std::vector<GuestState*> parallel_contexts(GuestState&) { return {}; }
    virtual ExecutionPolicy execution_policy(const GuestState&) const { return ExecutionPolicy::Automatic; }
    // counts[0]==0 時，協調 context 尚待處理非原生指令／故障；保留它直到慢路徑有進度。
    // 其他 worker 的退休可另行核算，但不能僅因它們消耗宿主時間就搶占這個零進度 context。
    virtual void after_parallel_instructions(GuestState& state,GuestMemory& memory,std::span<const std::uint64_t> counts) {
        if(!counts.empty() && counts[0])after_instructions(state,memory,counts[0]);
    }
    virtual std::uint64_t timestamp_counter(const GuestState& state,GuestMemory&) {return state.tsc;}
    // 正常常駐模式的唯讀時間戳來源。可由多個 worker 同時呼叫，不更新共享時間頁、
    // 排程或映射；false 不產生副作用，沿用一般語義流程。固定指令時鐘不得啟用。
    virtual bool resident_timestamp_counter(const GuestState&,const GuestMemory&,std::uint64_t&) const noexcept {return false;}
    // 明確允許在此預算內延後 after_instructions；期間服務入口與映射不可變更。
    // 預設關閉，保留依賴每區塊回呼的自訂服務；仍受 instruction_slice 限制。
    virtual std::uint64_t native_chain_limit() const noexcept { return 0; }
    // 可選的原生續行時限（奈秒）。服務明確允許在此時間內跨越多個指令批次，
    // 延後 after_instructions；期間映射、入口及排程狀態不可變更。核心僅在
    // 無明確指令上限、無單步及無 instruction_slice 限制時採用，且不超過 250 us。
    // 時限只在既有完整指令邊界觀察；API、故障與失效出口仍立即交回宿主。
    virtual std::uint64_t native_chain_time_slice_ns() const noexcept { return 0; }
    // 正常 resident AOT 的邊界驅動契約：純迴圈無配額、無時間片、無週期回呼。
    // 服務的時鐘及生命期不得依賴退休指令數；API／例外仍有完整客體狀態。
    virtual bool resident_execution() const noexcept { return false; }
    // 作用域僅由無配額 AOT 入口持有；服務可據此停用計量排程，巢狀呼叫成對進出。
    virtual void enter_resident_execution() noexcept {}
    virtual void leave_resident_execution() noexcept {}
    // 原生 worker 的慢出口隨虛擬執行緒保存；排程交換 GuestState 後仍須可識別。
    // 僅在所有 worker 停止的協調邊界呼叫，不由 worker 查詢或修改服務狀態。
    virtual std::uint64_t resident_context_key(const GuestState& state) const noexcept {
        return reinterpret_cast<std::uintptr_t>(&state);
    }
    // 全體 worker 停止後傳遞實際未完成的服務；不能只依入口的靜態平行資格
    // 重派同一個請求。key 為 resident_context_key，入口隨請求保存以排除舊狀態。
    // provider 僅登記排程待辦，不在這個通知內執行服務或切換 context。
    virtual void resident_service_request(std::uint64_t,GuestAddress) {}
    // 完整控制待辦涵蓋 API、指令語義、精確故障與例外。只在所有 worker
    // 停止時通知排程器；pending=false 代表控制器已接手相同 key／RIP。
    // 這不是退休或成功回報，實際語義仍由原有處理流程完成。
    virtual void resident_control_request(std::uint64_t key,GuestAddress rip,bool pending) {
        if(pending)resident_service_request(key,rip);
    }
    // 無配額批次不虛構退休指令數。結束後由服務處理時間、等待及下一個 runnable。
    virtual void after_resident_parallel(GuestState& state,GuestMemory& memory) {
        after_instructions(state,memory,0);
    }
    // 單一常駐框架因中斷請求返回：請求只在服務明列待排程工作（其他 runnable、等待期限、
    // 外部事件）時武裝，服務應在此做完整排程。預設沿用批次收尾，與既有服務相容。
    virtual void after_resident_interrupt(GuestState& state,GuestMemory& memory) {
        after_resident_parallel(state,memory);
    }
    // 零表示沒有待排程事件；不得為正常運算任意加入週期性的整批停止。
    virtual std::uint64_t resident_parallel_wakeup_ns(std::span<GuestState* const>) const {return 0;}
    // 可中斷的常駐框架另監看外部事件：observed 為協調者最近一次完整輪詢前的世代，
    // 世代改變即要求中斷，讓協調者在完整邊界輪詢並排程。沒有來源時回傳空指標。
    virtual std::shared_ptr<const ResidentExternalWake> resident_external_wake(std::uint64_t& observed) const {
        (void)observed;return {};
    }
    // 常駐平行框架的協調者在信箱等待、且沒有 worker 正在執行客體碼時另監看外部事件（不受搶占設定影響）：
    // 等待中的客體可能在等其他程序的管道資料或跨程序物件，這些通知不會產生信箱請求，只依期限等待會延誤到
    // 下一個客體計時器。世代改變時協調者先呼叫 request_resident_poll，再在完整邊界輪詢並排程。
    virtual std::shared_ptr<const ResidentExternalWake> resident_idle_wake(std::uint64_t& observed) const {
        (void)observed;return {};
    }
    // 下一次排程邊界必須完整輪詢外部事件（等待物件、管道、時間），即使目前執行緒時間片尚未到期。
    virtual void request_resident_poll() {}
    virtual bool allows_multiblock() const noexcept { return false; }
    virtual std::uint64_t instruction_slice() const noexcept { return UINT64_MAX; }
    // 載入器可提供已驗證的函式／程式區域；核心仍核對映射、入口及指令上限。
    // 查詢不得執行客體碼或改變記憶體／服務狀態。沒有提示時沿用核心預設策略。
    virtual std::optional<TranslationRegion> translation_region(GuestAddress) const {return {};}
    // 僅於缺少已發布入口的冷邊界查詢；不得執行客體碼、讀取檔案或改變映射。
    virtual std::optional<TranslationImage> translation_image(GuestAddress) const {return {};}
    virtual bool dispatch_cpu_exception(GuestState&, GuestMemory&, std::uint32_t) { return false; }
    virtual bool dispatch_system_call(GuestState&, GuestMemory&) { return false; }
    // CPL3 的 IDT／MSR 入口由客體 OS 提供；CPU 在退休前檢查，服務在退休後派送。
    virtual bool software_interrupt_enabled(std::uint8_t vector) const noexcept { return vector==3 || vector==4; }
    virtual bool system_enter_enabled(ExecutionMode) const noexcept { return false; }
    virtual bool dispatch_system_call(GuestState& state,GuestMemory& memory,SystemCallEntry entry) {
        return entry==SystemCallEntry::Syscall && dispatch_system_call(state,memory);
    }
    virtual std::optional<ExecutionResult> dispatch_software_interrupt(GuestState& state,GuestMemory&,std::uint8_t,unsigned) {
        ExecutionResult result;result.reason=StopReason::UnsupportedHostCall;result.rip=state.rip;
        result.message="客體 OS 尚未提供此軟體中斷服務";return result;
    }
    virtual bool dispatch_cpu_trap(GuestState& state,GuestMemory& memory,std::uint32_t code,unsigned) { return dispatch_cpu_exception(state,memory,code); }
    virtual bool dispatch_fault(GuestState&, GuestMemory&, const MemoryFault&) { return false; }
    virtual bool handles(GuestAddress address) const noexcept = 0;
    // 純計算葉節點服務：invoke 不切換執行緒、不讀時間、不做 I/O／GUI，也不執行巢狀客體碼。
    // 回傳 true 時，常駐翻譯碼可在完整指令邊界直接呼叫同一個 invoke，計為一條退休指令；
    // 退休後的 after_instructions 與一般原生批次一起彙總。條件可隨時改變，每次呼叫前重新查詢。
    // 此時 invoke 在客體浮點環境下執行：不得依賴或改變 FPCR；產生的浮點旗標只會讓後續指令保守地精確退出。
    virtual bool resident_service(GuestAddress) const noexcept { return false; }
    // 需要宿主浮點環境的純計算服務仍可常駐，但經完整 call_host 邊界執行；
    // 回客體時依更新後的 MXCSR 重設 FPCR/FPSR。此分類在服務物件生命期內固定，
    // resident_service 的執行資格仍每次重新查詢；預設沿用無浮點相依的輕量邊界。
    virtual bool resident_service_host_fp(GuestAddress) const noexcept { return false; }
    // 明確擁有計算浮點環境的葉節點服務：入口仍是客體 FP，完整狀態已物化。
    // 執行浮點計算前由服務設定環境；不得巢狀執行客體、切換執行緒或借用此參照到返回後。
    // 無需自行恢復 FPCR/FPSR；核心無條件重建客體環境。分類同樣在物件生命期內固定。
    virtual bool resident_service_manages_fp(GuestAddress) const noexcept { return false; }
    // 分類於服務物件生命期內固定；每次 invoke 仍須重新核對動態資格。
    // false 保留一般服務入口；true 不得執行客體回呼、I/O 或存取未宣告的客體暫存器。
    virtual bool resident_scalar_service(GuestAddress) const noexcept { return false; }
    virtual bool invoke_resident_scalar(ResidentScalarCall&,GuestMemory&) noexcept { return false; }
    // 純量原生配方：只對 resident_scalar_service 為 true 的入口查詢；回傳 ScalarRecipe 編號，0 沿用純量值服務。
    // 配方在客體 FP 環境內以 NEON／FP 指令運算，不物化 GuestState、不切換 FPCR，只產生 inexact 黏著旗標；
    // 特殊值、errno 路徑、非惰性浮點環境或無 MMU 基址時由核心交回同一入口的純量值服務。分類於物件生命期內固定。
    virtual unsigned resident_scalar_recipe(GuestAddress) const noexcept { return 0; }
    // 唯讀時鐘入口的原生 lowering。分類與時鐘來源在一次原生執行期間固定；
    // 切換虛擬時鐘／追蹤政策須先撤銷原生執行。None 沿用原服務，不以近似時間替代。
    virtual ResidentClockRecipe resident_clock_recipe(GuestAddress) const noexcept { return ResidentClockRecipe::None; }
    // 有界 heap 配方只消耗既有配置區。借用表僅於全部使用者停止後更新，
    // 映射／heap 生命期改變須先建立一致邊界；空值沿原服務完成完整契約。
    virtual ResidentHeapOperation resident_heap_recipe(GuestAddress) const noexcept { return ResidentHeapOperation::None; }
    virtual const ResidentHeap* borrow_resident_heap(GuestMemory&) noexcept { return nullptr; }
    virtual ResidentScalarErrors resident_scalar_errors(GuestMemory&) noexcept { return {}; }
    virtual std::optional<std::uint32_t> invoke_managed_service(GuestState& state,GuestMemory& memory,
                                                              const ResidentHostEnvironment& environment) {
        environment.restore();return invoke(state,memory);
    }
    // 批次內的非阻塞服務。映射、服務表、排程與程式碼在所有 worker 返回前保持不變。
    // 實作只能變更呼叫者的狀態／已驗證的資料，或自行同步的服務物件；不可呼叫一般 invoke。
    // false 必須完全不產生客體可見效果，讓協調執行緒沿既有故障／等待／回呼路徑重試。
    // true 代表已完整執行一個服務並設定返回 RIP／RSP；退休及 TSC 由引擎統一計算。
    virtual bool parallel_service(GuestAddress) const noexcept { return false; }
    virtual bool invoke_parallel_service(GuestState&,GuestMemory&) { return false; }
    // Retry 保證尚無客體副作用，僅等待其他 worker 釋放資源；不要求全域退出。
    virtual ResidentServiceProgress resident_parallel_service(GuestState& state,GuestMemory& memory) {
        return invoke_parallel_service(state,memory)?ResidentServiceProgress::Completed:ResidentServiceProgress::Coordinator;
    }
    // active 是本 epoch 真正取得 worker 的 context，已排除尚待補譯／完整服務者。
    // 身分與集合到下次一致邊界前不變；只比較指標，不讀取同伴正在改變的 GuestState。
    virtual ResidentServiceProgress resident_parallel_service(GuestState& state,GuestMemory& memory,
                                                               std::span<GuestState* const>) {
        return resident_parallel_service(state,memory);
    }
    // 所有 worker 已交出完整狀態並解除映射借用；僅由協調者呼叫。
    // 可改映射／程序內資料，但不得排程、變更 context 身分或執行客體回呼。
    // true 已完成服務與返回，false 完全沒有客體副作用；例外隨原 context
    // 保存，交既有服務例外流程收尾，不重複執行可能已產生副作用的操作。
    virtual bool resident_coordinated_service(GuestState&,GuestMemory&) {return false;}
    virtual std::string_view name(GuestAddress address) const noexcept = 0;
    virtual std::optional<std::uint32_t> invoke(GuestState& state, GuestMemory& memory) = 0;
    // 協調者在所有 worker 停止的一致邊界，為指定客體借用私人錯誤格位。
    // 不可暫時切換 provider 的目前執行緒；映射或客體重綁後須重新取得。
    // 預設空值，不假設單客體 resident_scalar_errors 對其他客體也適用。
    virtual ResidentScalarErrors resident_context_errors(const GuestState&,GuestMemory&) noexcept { return {}; }
    // 全體 worker 已交出狀態且目前客體沒有待辦時，選入另一個 runnable
    // 的既有控制請求。provider 必須沿完整的 TLS／回呼／例外交換契約切換，
    // 不執行客體指令、不虛構請求，也不喚醒仍在等待的客體。true 表示已切換；
    // 控制器沿原服務／精確語義流程處理後再詢問，清空待辦才重派 worker。
    virtual bool resident_select_control_request(GuestState&,GuestMemory&) { return false; }
    // 協調者信箱等待逾時、全部 slot 都有客體在運算時，不停止全體 worker：provider 自行同步地解析逾時／就緒等待，
    // 若有 runnable 客體需要 vCPU，指定一個可被換下的 active slot（不含 slot 0，其 context 由協調者持有）與替換
    // context。replacement 不在 active 中、沒有待辦、不停在服務入口；victim 仍視為 Runnable。false 表示不搶占：
    // 此時 plan.replacement 非空代表仍有 runnable 候選（協調者須沿全停輪替排程），為空代表沒有客體需要 vCPU，
    // 協調者只重算期限。期間其他 worker 繼續運算，不得改映射、程式碼或 active 客體的狀態。
    virtual bool resident_preempt_plan(GuestMemory&,std::span<GuestState* const>,ResidentPreemptPlan&) { return false; }
    // 被換下的 worker 已交出完整狀態、替換者即將取得該 vCPU；provider 只更新排程簿記（處理器編號、游標）。
    virtual void resident_preempt_commit(GuestState&,GuestState&) {}
    // worker slot（非 slot 0）在服務入口要阻塞等待時（協調服務已回 false）：provider 在不切換目前客體、不停其他 worker
    // 的前提下，把該客體標成等待（沿用完整路徑的等待契約，之後由一般等待解析完成呼叫），並指定一個可接手該 vCPU 的
    // runnable 客體。只有找得到替換者時才可改狀態並回 true；false 必須沒有任何副作用（協調者沿全停路徑處理）。
    virtual bool resident_block_service(GuestState&,GuestMemory&,std::span<GuestState* const>,GuestState*&) { return false; }
    // slot 停駐（resident_block_parking() 為 true 的 provider）：resident_block_service 回 true 且 replacement 為 nullptr
    // 表示客體已標成等待、沒有替換者；核心保留該 slot 的 worker 不回覆、不停其他 worker。slot 0（協調者的目前客體）
    // 停駐時 provider 已把活體狀態存回執行緒表。等待由一般等待解析完成後，核心以 resident_unpark 取回狀態並原地續跑。
    virtual bool resident_block_parking() const noexcept { return false; }
    // 停駐 slot 的客體是否已回到 runnable（呼叫已由等待解析完成）。
    virtual bool resident_context_runnable(const GuestState&) const { return false; }
    // 停駐 slot 續跑前：slot 0 由執行緒表載回活體狀態；其他 slot 只記錄已派送。
    virtual void resident_unpark(GuestState&,GuestMemory&) {}
    // 不停 worker 地解析等待（逾時、已就緒物件）；回傳是否有客體狀態改變。
    virtual bool resident_resolve_waits(GuestMemory&) { return false; }
    // 是否有可執行、未暫停、卻不在任何 slot 的客體（需要全停輪替才能取得 vCPU）。保守預設為有。
    virtual bool resident_runnable_waiting(std::span<GuestState* const>) const { return true; }
    // 停駐 slot 換人：仍在等待的停駐客體讓出此 vCPU 給不在任何 slot 的 runnable 客體。回傳接手者的 context
    // （slot 0 為已載入新目前客體的原 GuestState）；沒有可接手者回 nullptr 且沒有副作用。
    virtual GuestState* resident_idle_replacement(GuestState&,GuestMemory&,std::span<GuestState* const>) { return nullptr; }
    // 純量值回退可在呼叫者的 worker 完成。分類不變；執行只讀固定服務表及
    // epoch 內的映射，以已驗證視窗讀堆疊、借用私人 errno，不觸碰目前 runtime、
    // 共用頁面快取、排程或回呼。false 必須沒有客體可見效果。
    virtual bool resident_context_scalar_service(GuestAddress) const noexcept { return false; }
    virtual bool invoke_resident_context_scalar(ResidentScalarCall&,GuestMemory&,
                                               const ResidentScalarErrors&) noexcept { return false; }
};
}
