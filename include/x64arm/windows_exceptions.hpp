#pragma once
#include "x64arm/windows_unwind.hpp"

namespace x64arm {
struct ExceptionWalkFrame {
    GuestState context;
    std::uint64_t pc{};
    UnwindResult unwind;
    std::size_t callback_depth{}, exception_depth{};
    std::size_t scope_index{};
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
    // Win32 由 registration 的 state 與 SavedESP 復原；不套用 Win64 UNWIND_INFO。
    std::uint32_t registration{},saved_stack{};
    std::int32_t catch_high{-1},resume_state{-1};
    std::size_t callback_depth{},exception_depth{};
    bool local_unwound{};
    bool operator==(const MsvcCatch&) const=default;
};
struct ExceptionDispatch {
    GuestState original, walk, target_context;
    std::uint32_t code{}, flags{};
    std::uint64_t record{}, context{}, pointers{}, dispatcher{}, walk_context{};
    // x64 核心至使用者模式的例外框架；非零時先經 ntdll 客體入口，再接共用 VEH／SEH。
    std::uint64_t native_stack{}, native_entry{};
    std::vector<std::uint64_t> vectors;
    std::vector<ExceptionWalkFrame> frames;
    std::vector<SehScope> scopes;
    std::size_t index{}, callback_depth{}, target_frame{}, scope_index{};
    std::size_t walk_callback_depth{}, walk_exception_depth{};
    std::size_t walk_scope_index{};
    std::uint64_t unwind_context{};
    std::uint64_t function_query_pc{},function_query_entry{},unwind_origin_pc{},unwind_origin_sp{};
    bool function_query_ready{};
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
    // 已呼叫 _set_se_translator 轉譯器的 C++ 框架（establisher／registration）：轉譯器正常返回後重新進入處理常式時改找 catch(...)，
    // 轉譯器內再次發生的 SEH 也不再轉譯同一框架。
    std::uint64_t se_translated_frame{};
    ExceptionStage stage{ExceptionStage::Vectored};
    // API 自己的例外邊界在 VEH 之後、客體呼叫者的 SEH 之前承接。
    // 非零續行入口只在處理常式未改變 RIP 時使用，保留合法的 CONTEXT 轉向。
    bool unwound{}, unhandled_called{}, restore_jump{}, api_exception_boundary{};
    GuestAddress api_exception_resume{};
    // 續行時一併彈出的堆疊位元組（RaiseException 自身框架內引發時，回到呼叫者要彈出返回位址）。
    std::uint64_t api_exception_pop{};
    MsvcCatch msvc;
    // exception_ptr 重拋所建立的獨立堆積物件；catch 解構完成後由共用 EH 回收。
    std::uint64_t msvc_owned_object{};
    bool operator==(const ExceptionDispatch&) const = default;
};
inline void resume_api_exception_boundary(GuestState& state,const ExceptionDispatch& dispatch){
    const bool after_breakpoint=dispatch.code==0x80000003U && state.rip.value==dispatch.original.rip.value+1;
    if(dispatch.api_exception_resume.value && (state.rip==dispatch.original.rip || after_breakpoint)){
        state.rip=dispatch.api_exception_resume;state.reg(Gpr::Rsp)+=dispatch.api_exception_pop;}
}
struct ExceptionState {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> vectors;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> continue_vectors;
    std::vector<ExceptionDispatch> stack;
    std::uint64_t next_handle{0x8000};
    bool operator==(const ExceptionState&) const = default;
};
}
