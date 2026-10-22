#pragma once
// 已廢棄：LLVM 區域執行後端保留但不再維護；不影響 LLVM 靜態分析工具。
#include "x64arm/ir.hpp"
#include "x64arm/native_chain.hpp"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace x64arm {
// LLVM 配置完成後產生的不可變位置側表；x86／x64 共用同一客體狀態與精確恢復契約。
struct LlvmFaultState {
    struct Location {std::uint8_t kind{},size{};std::uint16_t reg{};std::int32_t offset{};std::uint64_t constant{};};
    // GuestState、選用的初始預算框架位置、區塊起始計數、客體位址、16 GPR、7 延後旗標、其餘 RFLAGS。
    // 旗標保留原始寬度；FaultSite::completed 提供故障位置在區塊內的固定退休量。
    struct FlagRecipe {
        enum class Kind : std::uint8_t { Raw, Parity, Zero, Sign, Add, Sub, Imul };
        Kind kind{};
        std::uint8_t width{};
        // >=0x8000 指向尚未提交的原始 GuestState GPR；其他值是位置側表索引。
        std::array<std::uint16_t,2> operands{};
    };
    std::vector<Location> locations;
    std::array<FlagRecipe,7> flags{};
    std::int32_t exit_delta{};
    std::uint32_t address_displacement{};
    std::uint32_t changed{};
    std::uint8_t width{},data_register{};
    bool writing{},remaining_budget{},shared_flags{};
};
// 只提供候選客體邊；間接跳躍仍在執行時比較實際目標，不把觀測當成證明。
struct LlvmRegionEdge {std::uint64_t source{},target{};};
// LLVM 區域使用獨立 C ABI；每個出口提交精確狀態，故障邊由編譯器配置暫存器。
// 編譯器及通過認證的 SMAP 可建立不可變產物；發布器不接受外部機器碼或 trusted 旗標。
class LlvmRegion final {
public:
    using Function = std::uint64_t (*)(GuestState*, GuestMemory*, std::uint64_t budget);
    static constexpr std::uint64_t guard_exit = UINT64_C(1) << 63;
    static constexpr std::uint64_t count_mask = ~guard_exit;
    static constexpr std::uint64_t dispatch_budget = native_dispatch_budget;
    static constexpr std::size_t instruction_limit = 1024;
    static_assert(dispatch_budget+instruction_limit<=UINT32_MAX);
    static constexpr std::size_t word_limit = 65536;
    struct FaultSite {
        std::uint32_t word_offset{}, bailout_word{}, instruction{}, completed{};
        GuestAddress rip;
        std::shared_ptr<const LlvmFaultState> recovery;
    };
    // 編譯器明確產生的位置側表；不掃描數值猜測宿主指標。
    struct HelperRelocation {std::uint32_t word_offset{};std::uint8_t helper{};};
    std::span<const std::uint32_t> words() const noexcept { return words_; }
    std::span<const FaultSite> faults() const noexcept { return faults_; }
    const IrBlock& source() const noexcept { return source_; }
    std::span<const GuestAddress> entries() const noexcept { return entries_; }
    std::span<const GuestAddress> static_exits() const noexcept { return static_exits_; }
    // 僅明確要求產碼診斷時保存；一般編譯及 SMAP 還原可以是空字串。
    const std::string& optimized_ir() const noexcept { return optimized_ir_; }
    // 編譯器已在所有出口還原完整 FP 環境，或完整產碼符合共用 A64 無浮點副作用 leaf 證明。
    // 不由客體 opcode 猜測 LLVM 選碼；舊離線格式預設不提供此資格。
    bool preserves_floating_environment() const noexcept { return preserves_floating_environment_; }
    // 非零時，原生鏈可從此 word 進入共用本體；X3 指向鏈已保存的 FPCR／FPSR。
    // 一般 C ABI 仍從 word 0 的封閉 wrapper 呼叫相同本體，不複製區域產碼。
    std::uint32_t shared_floating_entry() const noexcept { return shared_floating_entry_; }
    Function native_chain_entry(Function published_entry) const noexcept {
        return reinterpret_cast<Function>(reinterpret_cast<std::uintptr_t>(published_entry)+shared_floating_entry_*sizeof(std::uint32_t));
    }
    LlvmRegion(const LlvmRegion&) = delete;
    LlvmRegion& operator=(const LlvmRegion&) = delete;
private:
    friend class SmapCache;
    friend std::shared_ptr<const LlvmRegion> compile_llvm_region(const IrBlock&, MemoryOrdering, bool,std::span<const LlvmRegionEdge>,bool,bool,std::size_t,bool);
    // 只有驗證 MAC／引擎身分／來源位元組後的 SMAP 可重建，不公開 raw-code 發布入口。
    std::vector<std::uint8_t> serialize() const;
    static std::shared_ptr<const LlvmRegion> deserialize(IrBlock,std::span<const std::uint8_t>);
    LlvmRegion(IrBlock source, std::vector<std::uint32_t> words, std::string ir, std::vector<FaultSite> faults, std::vector<GuestAddress> entries,
               std::vector<HelperRelocation> helpers={},bool persistent=false,std::vector<GuestAddress> exits={},bool preserves_floating_environment=false,
               std::uint32_t shared_floating_entry=0)
        : source_(std::move(source)), words_(std::move(words)), optimized_ir_(std::move(ir)), faults_(std::move(faults)), entries_(std::move(entries)),
          helpers_(std::move(helpers)),persistent_(persistent),static_exits_(std::move(exits)),preserves_floating_environment_(preserves_floating_environment),
          shared_floating_entry_(shared_floating_entry) {}
    const IrBlock source_;
    const std::vector<std::uint32_t> words_;
    const std::string optimized_ir_;
    const std::vector<FaultSite> faults_;
    const std::vector<GuestAddress> entries_;
    const std::vector<HelperRelocation> helpers_;
    const bool persistent_{};
    const std::vector<GuestAddress> static_exits_;
    const bool preserves_floating_environment_{};
    const std::uint32_t shared_floating_entry_{};
};
// 編譯預算與產物安全上限分開；未啟用實驗時仍使用原來的 256 指令／兩個目標。
inline std::size_t llvm_region_capacity(const char* name,std::size_t fallback,std::size_t maximum) noexcept {
    const auto* raw=std::getenv(name);if(!raw)return fallback;
    const std::string_view value(raw);std::size_t parsed{};
    const auto result=std::from_chars(value.data(),value.data()+value.size(),parsed);
    return result.ec==std::errc{} && result.ptr==value.data()+value.size() && parsed>0 && parsed<=maximum?parsed:fallback;
}
inline std::size_t llvm_region_instruction_limit() noexcept {
    return llvm_region_capacity("X64ARM_JIT_LLVM_REGION_LIMIT",256,LlvmRegion::instruction_limit);
}
inline std::size_t llvm_region_target_limit() noexcept {
    return llvm_region_capacity("X64ARM_JIT_LLVM_TARGETS",2,16);
}
inline std::size_t llvm_region_word_limit(std::size_t instruction_capacity=0) noexcept {
    return std::max<std::size_t>(16384,(instruction_capacity?instruction_capacity:llvm_region_instruction_limit())*64);
}
bool llvm_regions_available() noexcept;
bool llvm_regions_requested() noexcept;
bool llvm_control_flow_requested() noexcept;
// CFG 模式保留兩條分支與匯合點；不支援的語義是區域出口，不依程式名稱或位址選取。
// 已完成基本區塊的凍結來源可明列容量；仍不得超過 instruction_limit，不修改 worker 的環境設定。
// 重用既有 LLVM 後繼時，只編譯前綴自身的循環；短直線前綴保留原生鏈。
std::shared_ptr<const LlvmRegion> compile_llvm_region(const IrBlock&, MemoryOrdering, bool control_flow=false,std::span<const LlvmRegionEdge> hints={},bool persistent=false,bool retain_ir=false,std::size_t instruction_capacity=0,bool require_cycle=false);
}
