#pragma once
#include "x64arm/windows_gui.hpp"
namespace x64arm {
std::uint32_t gui_system_color(unsigned index);
constexpr unsigned gui_system_color_count=31;
constexpr std::uint64_t gui_system_brush_base=0xa000,gui_system_pen_base=0xa100;
std::uint64_t gui_system_paint_object(GuiState& gui,unsigned index,bool pen);
bool gui_set_system_color(unsigned index,std::uint32_t color);
std::uint32_t gui_frame_control(GuiRect& rect,unsigned type,unsigned flags,std::vector<GuiDraw>& draws);
std::uint32_t gui_draw_edge(GuiRect& rect,unsigned edge,unsigned flags,std::vector<GuiDraw>& draws);
}
