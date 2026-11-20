#pragma once

#include "x64arm/windows_gui.hpp"

namespace x64arm {

// GWLP_ID 在一般子視窗保存控制項 ID；允許選單的視窗以同一欄位保存 HMENU。
inline bool gui_uses_control_id(const GuiWindow& window) {
    return (window.style & 0xc0000000U) == 0x40000000U;
}

template<typename Window>
auto& gui_menu_or_control_id(Window& window) {
    return gui_uses_control_id(window) ? window.id : window.menu;
}

}
