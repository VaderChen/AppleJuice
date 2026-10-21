#pragma once
#include <cstdlib>
#include <string_view>

namespace x64arm::detail {
// 同成品開關對照；只控制 I/O 等待的快速路徑，不改變完成／取消契約。
inline bool io_wait_fastpath_enabled() {
    static const bool enabled = [] {
        const auto* setting = std::getenv("X64ARM_IO_WAIT_FASTPATH");
        return !setting || std::string_view(setting) != "0";
    }();
    return enabled;
}
}
