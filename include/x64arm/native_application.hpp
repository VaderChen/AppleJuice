#pragma once
#include <atomic>

namespace x64arm {
enum class NativeApplicationRole { Main, Child };
namespace detail {
inline std::atomic<NativeApplicationRole> native_application_role{NativeApplicationRole::Main};
}
// 由啟動端在建立原生視窗前設定；共用命名空間或宿主父 PID 不代表客體子程序。
inline void configure_native_application_role(NativeApplicationRole role) noexcept {
    detail::native_application_role.store(role,std::memory_order_relaxed);
}
inline NativeApplicationRole native_application_role() noexcept {
    return detail::native_application_role.load(std::memory_order_relaxed);
}
}
