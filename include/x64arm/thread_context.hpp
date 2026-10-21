#pragma once
#include "x64arm/memory.hpp"
#include <span>
#include <vector>

namespace x64arm::thread_context {
constexpr std::size_t size(ExecutionMode mode) {return mode==ExecutionMode::X86?716:1232;}
constexpr std::size_t flags_offset(ExecutionMode mode) {return mode==ExecutionMode::X86?0:48;}
std::uint32_t validate_flags(ExecutionMode,std::uint32_t);
std::vector<std::uint8_t> capture(const GuestState&,ExecutionMode,std::uint32_t);
GuestState restore(const GuestState&,ExecutionMode,std::span<const std::uint8_t>);
void write_selected(GuestMemory&,std::uint64_t,ExecutionMode,std::span<const std::uint8_t>);
}
