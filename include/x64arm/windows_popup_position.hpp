#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace x64arm {
struct GuiPopupPlacement {
    std::array<std::int32_t,4> bounds{}, exclude{};
    bool operator==(const GuiPopupPlacement&) const = default;

    std::array<std::int32_t,4> position(std::int32_t x, std::int32_t y, std::int32_t width,
                                        std::int32_t height, std::uint32_t flags) const {
        using Wide = std::int64_t;
        const Wide left=bounds[0], top=bounds[1];
        const Wide last_x=std::max(left,Wide(bounds[2])-width);
        const Wide last_y=std::max(top,Wide(bounds[3])-height);
        Wide px=std::clamp(Wide(x)-((flags&8)?width:(flags&4)?width/2:0),left,last_x);
        Wide py=std::clamp(Wide(y)-((flags&32)?height:(flags&16)?height/2:0),top,last_y);
        if(exclude[0]<exclude[2] && exclude[1]<exclude[3] &&
           px<exclude[2] && px+width>exclude[0] && py<exclude[3] && py+height>exclude[1]){
            // 先保留指定方向的對齊，再嘗試另一軸；只採用在螢幕內的候選。
            const auto move_axis=[](Wide& pos,Wide first,Wide last,Wide before,Wide after,bool prefer_before){
                const std::array choices=prefer_before?std::array{before,after}:std::array{after,before};
                for(const Wide candidate:choices)if(candidate>=first && candidate<=last){pos=candidate;return true;}
                return false;
            };
            const auto horizontal=[&]{return move_axis(px,left,last_x,Wide(exclude[0])-width,exclude[2],flags&8);};
            const auto vertical=[&]{return move_axis(py,top,last_y,Wide(exclude[1])-height,exclude[3],flags&32);};
            if(flags&64){if(!horizontal())vertical();}
            else if(!vertical())horizontal();
        }
        const auto limit=std::numeric_limits<std::int32_t>::max();
        return {static_cast<std::int32_t>(px),static_cast<std::int32_t>(py),
                static_cast<std::int32_t>(std::min<Wide>(limit,px+width)),
                static_cast<std::int32_t>(std::min<Wide>(limit,py+height))};
    }
};
}
