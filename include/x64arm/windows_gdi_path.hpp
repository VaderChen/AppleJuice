#pragma once
#include "x64arm/windows_gui.hpp"
namespace x64arm {
std::vector<GuiPathPoint> gui_flatten_path(const std::vector<GuiPathPoint>&);
// CoreGraphics 產生實際幾何；回傳 device 座標，與 GDI region / Metal 共用。
std::array<GuiRegion,2> gui_cosmetic_dashes(const std::vector<GuiPathPoint>&,std::span<const double> dash,bool opaque);
GuiRegion native_path_region(const std::vector<GuiPathPoint>&,unsigned fill_mode);
std::vector<GuiPathPoint> native_stroke_path(const std::vector<GuiPathPoint>&,const GuiTransform&,double width,unsigned style,double miter);
std::vector<GuiPathPoint> native_stroke_path(const std::vector<GuiPathPoint>&,const GuiTransform&,double width,unsigned style,double miter,std::span<const double> dash);
}
