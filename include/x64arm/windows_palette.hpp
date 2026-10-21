#pragma once
#include "x64arm/windows_gui.hpp"

namespace x64arm {
// DEFAULT_PALETTE 的 20 個保留色，數值為 PALETTEENTRY 的 R/G/B/flags 順序。
inline constexpr std::array<std::uint32_t,20> gui_default_palette{
    0x000000,0x000080,0x008000,0x008080,0x800000,0x800080,0x808000,0xc0c0c0,0xc0dcc0,0xf0caa6,
    0xf0fbff,0xa4a0a0,0x808080,0x0000ff,0x00ff00,0x00ffff,0xff0000,0xff00ff,0xffff00,0xffffff};
inline std::vector<std::uint32_t> gui_dib_palette(unsigned bits){
    if(bits==1)return {0,0xffffff};
    if(bits==4)return {0,0x80,0x8000,0x8080,0x800000,0x800080,0x808000,0xc0c0c0,0x808080,0xff,0xff00,0xffff,0xff0000,0xff00ff,0xffff00,0xffffff};
    if(bits!=8)return {};
    std::vector<std::uint32_t> result;result.reserve(256);
    result.insert(result.end(),gui_default_palette.begin(),gui_default_palette.begin()+10);
    for(unsigned blue=0;blue<6;++blue)for(unsigned green=0;green<6;++green)for(unsigned red=0;red<6;++red)
        result.push_back(red*51U|(green*51U<<8)|(blue*51U<<16));
    for(unsigned gray=1;gray<=20;++gray){const auto level=(gray*255U+10U)/21U;result.push_back(level*0x010101U);}
    result.insert(result.end(),gui_default_palette.begin()+10,gui_default_palette.end());return result;
}
inline std::span<const std::uint32_t> gui_palette_entries(const GuiState& gui,std::uint64_t palette){
    if(palette==0xb00f)return gui_default_palette;
    const auto found=gui.palettes.find(palette);return found==gui.palettes.end()?std::span<const std::uint32_t>{}:found->second.entries;
}
inline std::uint32_t gui_palette_color(std::uint32_t entry){
    if(entry&0x02000000U){const auto index=entry&0xffffU;return index<gui_default_palette.size()?gui_default_palette[index]:0;}
    return entry&0xffffffU;
}
inline std::uint32_t gui_resolve_color(const GuiState& gui,const GuiDc& dc,std::uint32_t color){
    if((color>>24)==1){const auto entries=gui_palette_entries(gui,dc.palette);const auto index=color&0xffffU;return entries.empty()?0:gui_palette_color(entries[index<entries.size()?index:0]);}
    // 原生輸出為 true color；PALETTERGB 不需量化為實體 8-bit 系統調色盤。
    return color&0xffffffU;
}
inline void gui_resolve_draw_colors(const GuiState& gui,const GuiDc& dc,GuiDraw& draw){
    draw.color=gui_resolve_color(gui,dc,draw.color);draw.fill_color=gui_resolve_color(gui,dc,draw.fill_color);
    draw.pattern_background=gui_resolve_color(gui,dc,draw.pattern_background);draw.pattern_color=gui_resolve_color(gui,dc,draw.pattern_color);
}
}
