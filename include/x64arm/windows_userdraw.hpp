#pragma once
#include "x64arm/windows_gui.hpp"
namespace x64arm {
std::uint32_t gui_system_color(unsigned index);
std::uint32_t gui_frame_control(GuiRect& rect,unsigned type,unsigned flags,std::vector<GuiDraw>& draws);
std::uint32_t gui_draw_edge(GuiRect& rect,unsigned edge,unsigned flags,std::vector<GuiDraw>& draws);
}
