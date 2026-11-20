#pragma once
#include "x64arm/windows_gui.hpp"
#include <algorithm>

namespace x64arm {
inline std::array<std::int32_t,4> gui_virtual_screen_bounds(const std::vector<GuiMonitor>& monitors){
    auto bounds=monitors.front().rect;
    for(const auto& monitor:monitors){
        bounds[0]=std::min(bounds[0],monitor.rect[0]);bounds[1]=std::min(bounds[1],monitor.rect[1]);
        bounds[2]=std::max(bounds[2],monitor.rect[2]);bounds[3]=std::max(bounds[3],monitor.rect[3]);
    }
    return bounds;
}

inline std::array<std::int32_t,2> gui_clip_cursor_point(std::array<std::int32_t,2> point,const std::array<std::int32_t,4>& rect){
    for(unsigned axis=0;axis<2;++axis){
        // 零寬或零高時，該軸固定在起點。
        const auto end=rect[axis+2]>rect[axis]?rect[axis+2]-1:rect[axis];
        point[axis]=std::clamp(point[axis],rect[axis],end);
    }
    return point;
}
}
