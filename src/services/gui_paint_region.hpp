#pragma once
#include "x64arm/windows_gui.hpp"
#include "x64arm/guest_callback.hpp"

namespace x64arm {
inline GuiRegion gui_client_region(const GuiWindow& window) {
    return window.width && window.height ? GuiRegion{{0,0,static_cast<std::int32_t>(window.width),
        static_cast<std::int32_t>(window.height)}} : GuiRegion{};
}
inline GuiRegion gui_update_region(const GuiWindow& window) {
    if(!window.invalid)return {};
    const auto client=gui_client_region(window);
    return window.update_region?gui_region_combine(*window.update_region,client,1):client;
}
inline std::optional<GuiRegion> gui_dc_clip(const GuiDc& dc,bool system=true) {
    auto region=dc.application_clip;
    if(dc.meta_clip)region=region?gui_region_combine(*region,*dc.meta_clip,1):dc.meta_clip;
    if(system && dc.paint_clip)region=region?gui_region_combine(*region,*dc.paint_clip,1):dc.paint_clip;
    return region;
}
inline void gui_refresh_dc_clip(GuiDc& dc) {
    const auto region=gui_dc_clip(dc);dc.clipped=region.has_value();dc.excluded.clear();
    if(region){dc.clip=gui_region_bounds(*region);dc.excluded=gui_region_combine(GuiRegion{dc.clip},*region,4);}
}
inline GuestCallback gui_erase_callback(std::uint64_t procedure,std::uint64_t window) {
    GuestCallback callback{procedure,{window,0x14,0x10000000+window,0}};
    callback.gui_erase_window=window;return callback;
}
}
