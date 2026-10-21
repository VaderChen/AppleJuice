#pragma once
#include "x64arm/decoder.hpp"
namespace x64arm {
// 只保存批次核心需要的解碼資訊；JIT／AOT／第二層翻譯還原都從同一份 IR 建立。
struct RepMemoryForm {
    Opcode opcode{Opcode::Nop};
    std::uint8_t width{},address_width{},stack_width{},length{};
    MemoryOperand::Segment segment{MemoryOperand::Segment::Flat};
    explicit operator bool() const noexcept {return opcode!=Opcode::Nop;}
    static RepMemoryForm of(const Instruction&);
};
// 呼叫者必須保證客體執行緒沒有並行、服務允許延後回呼；最多完成 budget 個元素。
// 不符合連續私有資料範圍時回傳 0，交由原有逐元素路徑產生精確故障。
// 回傳已完成元素數；不更新 TSC、不派送服務、不保存任何宿主記憶體指標。
std::uint64_t execute_rep_memory(const RepMemoryForm&,GuestState&,GuestMemory&,std::uint64_t budget);
}
