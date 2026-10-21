#pragma once
#include "x64arm/windows_runtime.hpp"
#include "x64arm/memory.hpp"

namespace x64arm {
struct UnwindResult {
    GuestState caller;
    std::uint64_t establisher{}, handler{}, handler_data{}, function_entry{};
    unsigned handler_flags{};
    std::array<std::uint64_t,16> integer_sources{}, floating_sources{};
    bool operator==(const UnwindResult&) const = default;
};
const RuntimeFunction* lookup_function(const RuntimeImage& image, std::uint64_t pc, std::uint64_t function_entry = 0);
void validate_unwind(const GuestMemory& memory, const RuntimeImage& image);
// 載入階段借用區段資料，不建立客體映射或複製映像；仍檢查跨區段範圍及權限。
struct UnwindMappingView {
    GuestAddress base;
    std::span<const std::uint8_t> bytes;
    Permissions permissions;
};
void validate_unwind(std::span<const UnwindMappingView> mappings, const RuntimeImage& image);
UnwindResult virtual_unwind(const GuestMemory& memory, const RuntimeImage& image, std::uint64_t pc, const GuestState& context, unsigned handler_type = 1, std::uint64_t function_entry = 0);
void write_context(GuestMemory& memory, std::uint64_t address, const GuestState& state);
void write_context(GuestMemory& memory, std::uint64_t address, const GuestState& state, ExecutionMode mode);
GuestState read_context(const GuestMemory& memory, std::uint64_t address, const GuestState& previous);
}
