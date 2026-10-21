#pragma once
#include "x64arm/windows_gui.hpp"
namespace x64arm {
std::optional<GuiThemeMetrics> native_theme_metrics(std::u16string_view,std::int32_t,std::int32_t,std::uint32_t,std::array<std::int32_t,2>);
// 像素為 COLORREF 位元組順序、預乘 alpha，供共用 Metal AlphaBlend 使用。
std::optional<GuiBitmap> native_theme_background(std::u16string_view,std::int32_t,std::int32_t,std::uint32_t,std::array<std::int32_t,2>);
}
