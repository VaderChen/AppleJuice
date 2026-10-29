#pragma once
#include <cstdint>
#include <vector>

namespace x64arm {
struct GuestState;
std::uint64_t floating_callback_result(const GuestState&,unsigned width,bool x86);
enum class GuestExceptionPolicy { Propagate, CaptureCpp, CaptureAll, NoexceptCpp };
struct GuestCallback {
    std::uint64_t target{};
    std::vector<std::uint64_t> arguments;
    bool tls_initializer{};
    bool dll_initializer{};
    // UINT64 參數在 Win32 callback 中佔兩個堆疊 slot。
    std::uint32_t wide_arguments{};
    bool wide_result{};
    // Win32 fastcall 的前一／二個 DWORD 參數由 ECX／EDX 傳遞。
    unsigned x86_register_arguments{};
    // Windows x64 的浮點參數使用對應位置 XMM0–3；Win32 仍用堆疊。
    std::uint32_t floating_arguments{};
    unsigned floating_result{}; // 0、32 或 64；Win32 由 ST(0) 取得，x64 由 XMM0 取得。
    bool user_mode_callback{};
    // 只攔截逸出這個 callback 的例外；guest 內部仍可搜尋 handler 並完成 unwind。
    GuestExceptionPolicy exception_policy{GuestExceptionPolicy::Propagate};
    std::uint64_t waitable_timer{}; // 核心物件身分，用於取消或去除重複的 timer APC。
    // 非同步通知的資料必須在該次 callback 進入前發布，不能被後續通知覆蓋。
    struct Writeback32 {
        std::uint64_t address{};
        std::uint32_t clear_mask{UINT32_MAX},value{};
        bool operator==(const Writeback32&) const = default;
    };
    std::vector<Writeback32> writebacks{};
    bool operator==(const GuestCallback&) const = default;
};
}
