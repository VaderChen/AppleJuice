#pragma once
#include <cstdint>
namespace x64arm {
struct GuestState;
class GuestMemory;
namespace detail {
// 僅供例外返回／NtContinue 的有界唯讀診斷，不更改續行狀態。
void trace_handler_resume_instruction(const GuestState&,const GuestMemory&,std::uint32_t,
                                     std::uint64_t origin,std::uint64_t pid,std::uint64_t thread) noexcept;
}
}
