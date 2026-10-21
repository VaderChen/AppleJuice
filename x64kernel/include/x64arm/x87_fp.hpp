#pragma once
#include "x64arm/decoder.hpp"

namespace x64arm {
// 只接受已驗證的 2-byte x87 control、4-byte MXCSR 或 14／28-byte 環境區間。
// 回傳 0 表示待派送 #MF 或 MXCSR 保留位元錯誤，交回共用 CPU 故障路徑。
extern "C" std::uint64_t jit_x87_environment(void* bytes, GuestState* state, unsigned size, unsigned load) noexcept;
unsigned x87_tag(ExtendedFloat value) noexcept;
void check_x87_pending(const GuestState& state);
void execute_x87(const Instruction& instruction, const GuestState& state, GuestState& next, GuestMemory& memory);
// FBLD、FBSTP、FXTRACT、FNSAVE、FRSTOR；只由直譯器呼叫。
void execute_x87_legacy(const Instruction& instruction, const GuestState& state, GuestState& next, GuestMemory& memory);
}
