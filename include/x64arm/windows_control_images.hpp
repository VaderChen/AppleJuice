#pragma once
#include "x64arm/windows_gui.hpp"

namespace x64arm {
// 使用 presenter 的相同繪製流程產生完整控制項或單欄影像；不建立宿主視窗。
struct GuiHeaderRenderStyle {
    std::optional<std::uint32_t> text_color,background_color;
    bool frame_only{};
};
std::optional<GuiBitmap> render_gui_header_image(const GuiWindow&,std::optional<std::size_t> item = {},const GuiHeaderRenderStyle& = {});
}
