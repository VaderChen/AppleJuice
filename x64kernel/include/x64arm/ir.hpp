#pragma once
#include <span>
#include "x64arm/decoder.hpp"
#include "x64arm/jit_recovery.hpp"
#include <cstdint>
#include <vector>

namespace x64arm {
// 常數位移的未定義 OF／AF 是否視為覆寫（X64ARM_AOT_SHIFT_FLAGS，預設開啟）。
bool shift_undefined_flags_requested();
class Arm64CodeDescription;
class HostServices;
enum class MemoryOrdering : std::uint8_t { Relaxed, Tso, Shared, HardwareTso, NativeMmu, NativeMmuRelaxed };
constexpr bool hardware_ordering(MemoryOrdering ordering) {return ordering==MemoryOrdering::HardwareTso || ordering==MemoryOrdering::NativeMmu;}
constexpr bool relaxed_ordering(MemoryOrdering ordering) {return ordering==MemoryOrdering::Relaxed || ordering==MemoryOrdering::NativeMmuRelaxed;}
constexpr bool mmu_ordering(MemoryOrdering ordering) {return ordering==MemoryOrdering::NativeMmu || ordering==MemoryOrdering::NativeMmuRelaxed;}
struct IrEffects {
    std::uint16_t read_gpr{}, write_gpr{};
    std::uint16_t read_xmm{}, write_xmm{};
    std::uint64_t defined_flags{}, read_flags{};
    bool memory{}, branch{};
    // native_lowered_form 的結果：lift、效果計算與產碼共用，每條指令只判定一次。
    bool lowered{};
};
struct IrInstruction { GuestAddress rip; Instruction instruction; IrEffects effects; };
struct IrBlock { GuestAddress entry; std::vector<IrInstruction> instructions; };
// 執行模式決定 EIP／RIP 的回繞寬度；直接邊、CALL 返回點及後端出口共用此契約。
inline std::uint64_t ir_next_rip(const IrInstruction& node) {
    return (node.rip.value+node.instruction.length)&width_mask(node.instruction.stack_width);
}
inline std::uint64_t ir_relative_target(const IrInstruction& node) {
    return (ir_next_rip(node)+node.instruction.operands[0].immediate)&branch_target_mask(node.instruction);
}
// 線性片段：自 rip 起連續 count 條、共 bytes 位元組。快取條目以此取代完整 IR。
struct IrSpan { std::uint64_t rip{}; std::uint32_t count{}, bytes{}; };
IrEffects instruction_effects(const Instruction& instruction);
std::vector<IrSpan> ir_spans(const IrBlock& block);
IrBlock relift(const GuestMemory& memory, GuestAddress entry, std::span<const IrSpan> spans, std::size_t limit = SIZE_MAX,DecodeCache* cache=nullptr);
// 只描述真正消費的旗標；未覆寫的舊位元由 liveness 自然保留。
constexpr std::uint64_t condition_flags(Condition condition){
    constexpr std::array<std::uint64_t,8> masks{flags::OF,flags::CF,flags::ZF,flags::CF|flags::ZF,
        flags::SF,flags::PF,flags::SF|flags::OF,flags::ZF|flags::SF|flags::OF};
    return masks[static_cast<unsigned>(condition)/2];
}
std::optional<Condition> branch_condition(const Instruction&);
// LOOP／LOOPcc／JCXZ 以 rCX 計數決定後繼；conditional_branch 涵蓋旗標條件與計數條件，用於判定 fallthrough 邊。
bool counter_branch(Opcode op);
bool conditional_branch(const Instruction&);
// native_form：可進入原生區域（含直譯語義橋接）；native_lowered_form：具專屬 ARM64 lowering。
bool native_form(const Instruction& instruction);
bool native_lowered_form(const Instruction& instruction);
IrBlock lift_block(const GuestMemory& memory, GuestAddress entry, std::size_t limit = 32,
                   std::optional<GuestAddress> stop = {}, const HostServices* services = nullptr,DecodeCache* cache=nullptr);
// 由客體程式碼位元組快照 lift（不經 GuestMemory、不查服務入口）；規則與 lift_block 相同。
// 解碼需要快照範圍外的位元組時 truncated=true，呼叫端必須捨棄結果。使用前須逐條比對實際客體位元組與服務邊界。
IrBlock lift_snapshot(std::span<const std::uint8_t> bytes, std::uint64_t base, ExecutionMode mode, GuestAddress entry, std::size_t limit, bool& truncated);
std::string format_ir(const IrBlock& block);
std::vector<std::uint32_t> emit_arm64(const IrBlock& block, MemoryOrdering ordering = MemoryOrdering::Tso, Arm64Recovery* recovery = nullptr, bool inline_memory = true, bool scopes = true, std::shared_ptr<const Arm64CodeDescription>* description = nullptr);
}
