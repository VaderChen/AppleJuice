#pragma once
#include "x64arm/windows_gui.hpp"
#include <algorithm>
#include <optional>

namespace x64arm {
// 虛擬桌面與任意 DPI 查詢共用同一組基準度量。
inline std::uint32_t gui_scale_metric(std::uint32_t value,std::uint32_t dpi) {
    return static_cast<std::uint32_t>(std::min<std::uint64_t>((std::uint64_t{value}*dpi+48)/96,INT32_MAX));
}
inline std::uint32_t gui_scale_signed_metric(std::int32_t value,std::uint32_t dpi) {
    const auto magnitude=(static_cast<std::uint64_t>(value<0?-static_cast<std::int64_t>(value):value)*dpi+48)/96;
    const auto signed_value=value<0?-static_cast<std::int64_t>(magnitude):static_cast<std::int64_t>(magnitude);
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(signed_value,INT32_MIN,INT32_MAX));
}
inline std::uint32_t gui_system_metric(std::uint32_t index,std::uint32_t dpi=96) {
    std::uint32_t value{};
    switch(index){
    case 0:case 16:case 78:return 1440;case 1:case 17:case 79:return 900;
    case 80:return 1;
    case 2:case 3:case 20:case 21:value=16;break;case 4:value=24;break;
    case 5:case 6:case 45:case 46:value=1;break;
    case 11:case 12:case 13:case 14:value=32;break;
    case 7:case 8:case 32:case 33:value=4;break;case 15:value=20;break;
    case 49:case 71:case 72:value=18;break;default:return 0;
    }return gui_scale_metric(value,dpi);
}
struct GuiInsets {std::uint32_t left{},top{},right{},bottom{};};
inline GuiInsets gui_frame_insets(std::uint32_t style,std::uint32_t extended,bool menu,bool scrollbars=true) {
    std::uint32_t border=style&0x40000U?4U:style&0x400000U?4U:style&0x800000U?1U:0U;
    if(extended&1U)border=std::max(border,4U);
    if(extended&0x20000U)++border;
    if(extended&0x200U)border+=2;
    if((extended&0x100U) && !border)border=1;
    GuiInsets result{border,border,border,border};
    if((style&0xc00000U)==0xc00000U)result.top+=(extended&0x80U)?18U:24U;
    if(menu && !(style&0x40000000U))result.top+=20;
    if(scrollbars){if(style&0x100000U){if(extended&0x4000U)result.left+=16;else result.right+=16;}if(style&0x200000U)result.bottom+=16;}
    return result;
}
inline GuiInsets gui_frame_insets_for_dpi(std::uint32_t style,std::uint32_t extended,bool menu,std::uint32_t dpi) {
    auto f=gui_frame_insets(style,extended,menu,false);
    f.left=gui_scale_metric(f.left,dpi);f.top=gui_scale_metric(f.top,dpi);
    f.right=gui_scale_metric(f.right,dpi);f.bottom=gui_scale_metric(f.bottom,dpi);return f;
}
inline GuiInsets gui_frame_insets(const GuiWindow& window) {if(window.frame_insets){const auto& a=*window.frame_insets;return {a[0],a[1],a[2],a[3]};}return gui_frame_insets(window.style,window.ex_style,window.menu!=0);}
// NCCALCSIZE 回傳的客戶區與外框共用尺寸／座標模型；不得再依 style 重複扣框。
inline void gui_calculated_frame(GuiWindow& window,const std::array<std::int32_t,4>& outer,const std::array<std::int32_t,4>& client) {
    const auto width=std::max<std::int64_t>(0,std::int64_t{outer[2]}-outer[0]),height=std::max<std::int64_t>(0,std::int64_t{outer[3]}-outer[1]);
    const auto left=std::clamp<std::int64_t>(std::int64_t{client[0]}-outer[0],0,width),top=std::clamp<std::int64_t>(std::int64_t{client[1]}-outer[1],0,height);
    const auto right=std::clamp<std::int64_t>(std::int64_t{outer[2]}-client[2],0,width-left),bottom=std::clamp<std::int64_t>(std::int64_t{outer[3]}-client[3],0,height-top);
    window.frame_insets=std::array{static_cast<std::uint32_t>(left),static_cast<std::uint32_t>(top),static_cast<std::uint32_t>(right),static_cast<std::uint32_t>(bottom)};
}
inline void gui_refresh_frame(GuiWindow& window) {const auto f=gui_frame_insets(window.style,window.ex_style,window.menu!=0);window.frame_insets=std::array{f.left,f.top,f.right,f.bottom};}
inline std::array<std::uint32_t,2> gui_client_size(std::uint32_t width,std::uint32_t height,const GuiInsets& inset) {
    return {width>inset.left+inset.right?width-inset.left-inset.right:0,height>inset.top+inset.bottom?height-inset.top-inset.bottom:0};
}
inline std::array<std::uint32_t,2> gui_outer_size(const GuiWindow& window) {
    const auto inset=gui_frame_insets(window);return {window.width+inset.left+inset.right,window.height+inset.top+inset.bottom};
}
inline std::uint64_t gui_move_lparam(const GuiWindow& window) {const auto f=gui_frame_insets(window);return ((static_cast<std::uint32_t>(window.x)+f.left)&65535U)|(((static_cast<std::uint32_t>(window.y)+f.top)&65535U)<<16);}
inline std::array<std::int64_t,2> gui_client_origin(const GuiState& gui,std::uint64_t handle) {
    std::array<std::int64_t,2> result{};
    for(unsigned depth=0;handle && depth<64;++depth){const auto found=gui.windows.find(handle);if(found==gui.windows.end())break;const auto& window=found->second;const auto inset=gui_frame_insets(window);result[0]+=static_cast<std::int64_t>(window.x)+inset.left;result[1]+=static_cast<std::int64_t>(window.y)+inset.top;handle=window.parent;}
    return result;
}
// Z-order 只比較同一 parent 的兄弟視窗；命中頂層後才向內搜尋其子樹。
// 保留子視窗預設位於兄弟底部的建立規則，父視窗不會遮住自己的控制項。
inline std::uint64_t gui_window_from_point(const GuiState& gui,std::int64_t x,std::int64_t y,std::optional<std::uint64_t> desktop=std::nullopt) {
    const auto search=[&](const auto& self,std::uint64_t parent,unsigned depth)->std::uint64_t {
        if(depth>=64)return 0;
        for(const auto handle:gui.z_order){const auto found=gui.windows.find(handle);if(found==gui.windows.end())continue;const auto& w=found->second;
            if(w.parent!=parent || !w.visible || !w.enabled || w.show_command==2 || (desktop && w.desktop_object!=*desktop))continue;
            const auto kind=w.style&31U;if(w.parent && gui_control_class(w)==u"static" && (kind<=2 || kind==11 || kind==12))continue;
            const auto origin=gui_client_origin(gui,handle);const auto f=gui_frame_insets(w);
            if(x<origin[0]-f.left || y<origin[1]-f.top || x>=origin[0]+w.width+f.right || y>=origin[1]+w.height+f.bottom)continue;
            if(x>=origin[0] && y>=origin[1] && x<origin[0]+w.width && y<origin[1]+w.height)
                if(const auto child=self(self,handle,depth+1))return child;
            return handle;
        }return 0;
    };
    return search(search,0,0);
}
// 標準非客戶區幾何；與 API 共用階層命中，再保留邊框／標題／捲軸分類。
inline unsigned gui_window_hit(const GuiState& gui,std::uint64_t handle,std::int64_t x,std::int64_t y) {
    if(gui.windows.contains(handle)){const auto& w=gui.windows.at(handle);
        const auto origin=gui_client_origin(gui,handle);const auto f=gui_frame_insets(w);const auto left=origin[0]-f.left,top=origin[1]-f.top,right=origin[0]+w.width+f.right,bottom=origin[1]+w.height+f.bottom;
        if(x<left || y<top || x>=right || y>=bottom)return 0;
        if(x>=origin[0] && y>=origin[1] && x<origin[0]+w.width && y<origin[1]+w.height)return 1;
        const auto border=(w.style&0x40000U)?4:1;
        if(w.style&0x40000U){const bool l=x<left+border,r=x>=right-border,t=y<top+border,b=y>=bottom-border;if(t)return l?13U:r?14U:12U;if(b)return l?16U:r?17U:15U;if(l)return 10;if(r)return 11;}
        if((w.style&0xc00000U)==0xc00000U && y<top+border+((w.ex_style&0x80U)?18:24)){
            if((w.style&0x80000U) && x<left+border+20)return 3;
            if((w.style&0x80000U) && x>=right-border-20)return 20;
            if((w.style&0x10000U) && x>=right-border-40)return 9;
            if((w.style&0x20000U) && x>=right-border-((w.style&0x10000U)?60:40))return 8;return 2;}
        if(w.menu && y<origin[1])return 5;if((w.style&0x100000U) && (x>=origin[0]+w.width || x<origin[0]))return 7;if((w.style&0x200000U) && y>=origin[1]+w.height)return 6;return 18;
    }return 0;
}
inline std::pair<std::uint64_t,unsigned> gui_mouse_hit(const GuiState& gui,std::int64_t x,std::int64_t y,std::uint64_t desktop) {
    const auto handle=gui_window_from_point(gui,x,y,desktop);return {handle,gui_window_hit(gui,handle,x,y)};
}

}
