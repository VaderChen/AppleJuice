#pragma once
#include "x64arm/windows_runtime.hpp"
#include "x64arm/windows_exceptions.hpp"
#include "x64arm/host_instruction_budget.hpp"
#include <algorithm>
namespace x64arm {
struct MultimediaThreadTask {
    std::uint64_t handle{};
    int base{},minimum{},maximum{},relative{};
    bool operator==(const MultimediaThreadTask&) const = default;
};
enum class ThreadStatus { Runnable, Waiting, Exited };
enum class ThreadWaitKind { HeapRetry, Http, Resolver, SocketSelect, Objects, Sleep, Critical, CriticalRetry, Completion, CrtLock, StandardInput, ConsoleInput, GuiSend, GuiInput, Slim, Condition, Once, LoaderRetry, ProcessInputIdle, FileIo, RegisteredWait, RegisteredWaitCompletion, TimerQueueCompletion, RegistryNotify, ThreadpoolCompletion, PowerNotificationCompletion, EtwTrace, EtwEnable, DebugEvent, MediaFoundation, ServiceControl, CrtFileRetry, CrtProcess, CrtLockRetry, NativeDevice, Perflib, HandleTransfer, ProcessCreation };
struct ThreadWait {
    ThreadWaitKind kind{ThreadWaitKind::Objects};
    std::vector<std::uint64_t> handles;
    std::vector<std::uint32_t> socket_events;
    std::uint64_t deadline{UINT64_MAX};
    bool all{}, alertable{};
    bool shared{}, critical{}, signaled{}, timed_out{};
    std::uint64_t gui_window{},gui_drag_detection{},gui_move_size{};
    std::uint32_t gui_min{},gui_max{},gui_mask{UINT32_MAX};bool gui_new_only{},gui_callbacks{true};
    bool operator==(const ThreadWait&) const = default;
};
struct ThreadStackGuard {
    std::uint32_t guarantee{};
    bool armed{},overflow{};
    std::uint64_t limit(std::uint64_t base)const{return armed?base+guarantee+4096:base;}
    bool operator==(const ThreadStackGuard&) const = default;
};
struct GuestThread {
    struct SystemCallFrame {
        std::uint64_t stack{},caller{},resume{},flags{};
        std::uint64_t expected_stack{},resume_stack{},resume_rcx{},resume_rdx{},resume_r11{};
        std::vector<std::uint8_t> saved_memory;
        bool operator==(const SystemCallFrame&) const = default;
    };
    GuestState context;
    std::vector<SystemCallFrame> system_calls;
    std::vector<CallbackFrame> callbacks;
    std::vector<GuestCallback> apcs;
    std::vector<std::uint64_t> console_handlers; // 控制事件接收時的 handler 快照。
    std::vector<SortState> sorts;
    std::vector<ExceptionDispatch> exceptions;
    std::array<std::uint64_t,128> fls_values{};
    std::array<std::uint64_t,1088> tls_values{};
    std::uint64_t teb{}, exception_area{}, stack_base{}, stack_size{}, errno_cell{}, entry{};
    ThreadStackGuard stack_guard;
    std::uint64_t fiber{},fiber_stack_allocation{},desktop_handle{};
    std::uint64_t msvc_exception_cells{},msvc_exception_area{};
    std::uint64_t impersonation_token{};
    std::vector<std::uint8_t> security_descriptor;
    std::uint32_t pseudo_access{0x1fffffU};
    std::uint32_t public_id{}; // Windows 對外身分；排程、IPC 與 CPU context 仍使用本機索引。
    std::uint64_t cycles{}; // 客體執行成本；等待不累計，與宿主牆鐘獨立。
    std::array<std::uint8_t,16> activity_id{};
    std::uint64_t affinity{UINT64_MAX}, strtok_cursor{}, invalid_parameter_handler{};
    std::uint32_t suspended{}, exit_code{}, ideal_processor{};
    std::int32_t priority{};
    MultimediaThreadTask multimedia;
    int effective_priority(std::uint32_t process_class,bool background) const {
        if(multimedia.handle)return std::clamp(multimedia.base+multimedia.relative,multimedia.minimum,multimedia.maximum);
        const int base=background || process_class==0x40?4:process_class==0x4000?6:process_class==0x8000?10:process_class==0x80?13:8;
        return priority==-15?1:priority==15?15:std::clamp(base+priority+static_cast<int>(dynamic_priority_boost),1,15);
    }
    // 等待剛完成而尚未 dispatch；與 Windows 可停用的動態優先序提升分開。
    bool wake_pending{};
    std::uint32_t dynamic_priority_boost{};
    bool priority_boost_disabled{};
    std::uint32_t power_control{}, power_state{};
    bool dynamic_code_opt_out{};
    bool hidden_from_debugger{};
    std::uint32_t last_native_status{};
    std::optional<std::uint32_t> error_mode; // 未覆寫時沿用程序錯誤模式。
    std::u16string description;
    std::uint64_t gui_pump_time{},dpi_context{};
    std::uint64_t message_extra{};std::uint32_t message_time{},message_position{};
    bool terminating{},forced_stop{};
    ThreadStatus status{ThreadStatus::Runnable};
    ThreadWait wait;
    bool operator==(const GuestThread&) const = default;
};
struct GuestFiber {
    GuestState context;
    std::vector<CallbackFrame> callbacks;
    std::vector<SortState> sorts;
    std::vector<ExceptionDispatch> exceptions;
    std::array<std::uint64_t,128> fls_values{};
    std::uint64_t stack_base{},stack_size{},stack_allocation{},exception_list{UINT32_MAX},active_thread{},last_thread{};
    ThreadStackGuard stack_guard;
    std::uint64_t msvc_exception_cells{},msvc_exception_area{};
    bool float_switch{},deleting{};
    bool operator==(const GuestFiber&) const = default;
};
struct ThreadRuntime {
    bool context_saved{};
    std::vector<std::uint64_t> retained_wait_handles; // 已關閉客體 handle 的既有等待仍持有核心物件。
    std::map<std::uint64_t,GuestThread> items;
    std::map<std::uint64_t,GuestFiber> fibers;
    std::map<std::u16string,std::uint32_t> multimedia_task_indices;
    std::uint64_t current{1}, next_id{2}, remaining{4096};
    std::uint64_t slice_deadline{UINT64_MAX}; // 宿主時鐘模式的時間片上限，單位 100 ns。
    HostInstructionBudget host_instruction_budget;
    bool yield{};
    // 常駐中斷返回的排程：完整輪詢外部事件與逾時，但時間片未到期時續跑目前執行緒，不強制輪替。
    bool interrupt_poll{};
    bool operator==(const ThreadRuntime&) const = default;
};
}
