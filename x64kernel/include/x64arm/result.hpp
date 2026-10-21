#pragma once
#include "x64arm/memory.hpp"
#include <optional>
#include <string>

namespace x64arm {
enum class StopReason { Returned, Exited, BudgetExhausted, UnsupportedInstruction, InvalidInstruction,
                        MemoryFault, ArithmeticFault, UnsupportedHostCall, PlatformUnavailable, InternalError };
std::string_view stop_name(StopReason reason);
struct ExecutionResult {
    StopReason reason{StopReason::InternalError};
    std::uint64_t completed{};
    GuestAddress rip{};
    std::string message;
    std::vector<std::uint8_t> bytes;
    std::string instruction;
    std::uint8_t width{};
    std::optional<GuestAddress> fault_address;
    std::optional<Access> fault_access;
    std::size_t fault_size{};
    std::optional<std::uint32_t> exit_code;
    // 正常常駐執行不計算退休指令數；completed 不能充當精確計數或排程進度。
    bool completed_exact{true};
};
std::string format_result(const ExecutionResult& result, const GuestState& state);
}
