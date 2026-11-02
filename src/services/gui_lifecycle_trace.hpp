#pragma once
#include "x64arm/windows_gui.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace x64arm::detail {
// 僅診斷焦點／擷取生命週期；不啟用完整服務追蹤，保留 resident API 快速路徑。
inline void trace_gui_lifecycle(std::string_view action,const GuiState& gui,std::uint64_t pid,
                               std::uint64_t thread,std::uint64_t window,std::uint64_t value=0) {
    static const bool enabled=std::getenv("X64ARM_TRACE_GUI_LIFECYCLE")!=nullptr;
    if(!enabled)return;
    const auto time=std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(stderr,"[gui-lifecycle] unix=%.6f pid=%llu thread=%llu action=%.*s hwnd=%llx value=%llx focus=%llx active=%llx capture=%llx generation=%llu\n",
        time,static_cast<unsigned long long>(pid),static_cast<unsigned long long>(thread),static_cast<int>(action.size()),action.data(),
        static_cast<unsigned long long>(window),static_cast<unsigned long long>(value),static_cast<unsigned long long>(gui.focus),
        static_cast<unsigned long long>(gui.active),static_cast<unsigned long long>(gui.capture),static_cast<unsigned long long>(gui.capture_generation));
}
}
