#pragma once
#include "x64arm/memory.hpp"
#include "x64arm/execution_policy.hpp"
#include "x64arm/result.hpp"
#include <functional>
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
#define X64ARM_HOST_SERVICES_SCALAR_ERRORS 1
#define X64ARM_HOST_SERVICES_PARALLEL_SERVICE 1
#define X64ARM_HOST_SERVICES_TRANSLATION_REGION 1
#define X64ARM_HOST_SERVICES_TRANSLATION_IMAGE 1
#define X64ARM_HOST_SERVICES_NATIVE_TIME_SLICE 1
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
// CPU 只認識受控客體入口，Windows ABI 與 API 語義由服務層負責。
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
    // 明確允許在此預算內延後 after_instructions；期間服務入口與映射不可變更。
    // 預設關閉，保留依賴每區塊回呼的自訂服務；仍受 instruction_slice 限制。
    virtual std::uint64_t native_chain_limit() const noexcept { return 0; }
    // 可選的原生續行時限（奈秒）。服務明確允許在此時間內跨越多個指令批次，
    // 延後 after_instructions；期間映射、入口及排程狀態不可變更。核心僅在
    // 無明確指令上限、無單步及無 instruction_slice 限制時採用，且不超過 250 us。
    // 時限只在既有完整指令邊界觀察；API、故障與失效出口仍立即交回宿主。
    virtual std::uint64_t native_chain_time_slice_ns() const noexcept { return 0; }
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
    virtual std::string_view name(GuestAddress address) const noexcept = 0;
    virtual std::optional<std::uint32_t> invoke(GuestState& state, GuestMemory& memory) = 0;
};
}
