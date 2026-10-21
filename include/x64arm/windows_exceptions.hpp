#pragma once
#include "x64arm/windows_unwind.hpp"

namespace x64arm {
struct ExceptionWalkFrame {
    GuestState context;
    std::uint64_t pc{};
    UnwindResult unwind;
    std::size_t callback_depth{}, exception_depth{};
    bool operator==(const ExceptionWalkFrame&) const = default;
};
struct SehScope {
    std::uint32_t begin{}, end{}, filter{}, target{};
    bool operator==(const SehScope&) const = default;
};
enum class ExceptionStage { Vectored, Search, Unwind, Unhandled };
struct MsvcCatch {
    std::uint64_t handler{},parent{},unwind_help{},function_info{},destructor{},continuation{},copy_source{},copy_target{},copy_constructor{};
    std::int32_t target_state{-1},catch_state{-1},rethrow_state{-2};
    bool active{},catching{},copy_virtual{},entered_catch{};
    bool compressed{};std::array<std::uint64_t,2> continuations{};unsigned continuation_count{};
    bool operator==(const MsvcCatch&) const=default;
};
struct ExceptionDispatch {
    GuestState original, walk, target_context;
    std::uint32_t code{}, flags{};
    std::uint64_t record{}, context{}, pointers{}, dispatcher{}, walk_context{};
    std::vector<std::uint64_t> vectors;
    std::vector<ExceptionWalkFrame> frames;
    std::vector<SehScope> scopes;
    std::size_t index{}, callback_depth{}, target_frame{}, scope_index{};
    std::size_t walk_callback_depth{}, walk_exception_depth{};
    std::uint64_t target_ip{}, return_value{};
    std::uint64_t consolidate_record{}, consolidate_context{};
    GuestState consolidate_origin;
    std::uint32_t x86_frame{UINT32_MAX}, x86_next{UINT32_MAX}, x86_target{}, x86_guard{}, x86_previous{UINT32_MAX}, x86_nested{};
    unsigned x86_steps{};
    std::uint64_t x86_jump_buffer{};
    // 載入器 SEH 邊界位於 DllMain／TLS 外；先展開 guest finally，再回復載入器流程。
    std::uint64_t module_operation{};
    bool module_continue{};
    std::size_t module_callback_depth{}, module_exception_depth{};
    GuestState module_context;
    std::uint64_t etw_thread{};std::size_t etw_callback_depth{},etw_exception_depth{};std::uint32_t etw_seh_boundary{UINT32_MAX};
    std::size_t user_callback_depth{SIZE_MAX};
    std::size_t protected_callback_depth{SIZE_MAX};
    ExceptionStage stage{ExceptionStage::Vectored};
    bool unwound{}, unhandled_called{}, restore_jump{}, winrt_origination{};
    MsvcCatch msvc;
    // exception_ptr 重拋所建立的獨立堆積物件；catch 解構完成後由共用 EH 回收。
    std::uint64_t msvc_owned_object{};
    bool operator==(const ExceptionDispatch&) const = default;
};
struct ExceptionState {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> vectors;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> continue_vectors;
    std::vector<ExceptionDispatch> stack;
    std::uint64_t next_handle{0x8000};
    bool operator==(const ExceptionState&) const = default;
};
}
