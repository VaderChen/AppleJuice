#pragma once
#include "x64arm/windows_region.hpp"
#include <cstdlib>

namespace x64arm {
inline GuiRegion rounded_region(GuiRect rect,std::int64_t ellipse_width,std::int64_t ellipse_height){
    if(rect[0]>rect[2])std::swap(rect[0],rect[2]);if(rect[1]>rect[3])std::swap(rect[1],rect[3]);
    if(!gui_region_coordinates({rect}))throw std::invalid_argument("區域座標超出 27 位範圍");
    // 與 GDI 曲線圖元一致，右／下邊界位於指定矩形的內側。
    if(gui_rect_empty(rect))return {};--rect[2];--rect[3];if(gui_rect_empty(rect))return {};
    const auto ew=std::min<std::int64_t>(rect[2]-rect[0],std::abs(ellipse_width));
    const auto eh=std::min<std::int64_t>(rect[3]-rect[1],std::abs(ellipse_height));
    if(ew<2 || eh<2)return {rect};
    if(eh>65536 || ew>1048576)throw std::length_error("橢圓區域超出資源限制");
    // 整數中點追蹤橢圓外緣；曲線僅建立邊角列，中間長方形合併成同一水平帶。
    const auto horizontal=ew-1,vertical=eh-1;
    const auto horizontal_square=horizontal*horizontal,vertical_square=vertical*vertical;
    auto derivative_x=4*vertical_square*(1-horizontal),derivative_y=4*horizontal_square*(1+(vertical&1));
    auto error=derivative_x+derivative_y+horizontal_square*(vertical&1);
    std::int64_t inset=0,row=eh/2;std::vector<std::int32_t> profile(static_cast<std::size_t>(eh));
    while(inset<=ew/2){const auto decision=error*2;
        if(decision>=derivative_x){++inset;derivative_x+=8*vertical_square;error+=derivative_x;}
        if(decision<=derivative_y){++row;derivative_y+=8*horizontal_square;error+=derivative_y;if(row<eh)profile[static_cast<std::size_t>(row)]=static_cast<std::int32_t>(inset);}
    }
    for(std::int64_t i=0;i<eh/2;++i)profile[static_cast<std::size_t>(i)]=profile[static_cast<std::size_t>(eh-1-i)];
    GuiRegion region;
    const auto append=[&](std::int32_t top,std::int32_t bottom,std::int32_t indent){
        GuiRect band{rect[0]+indent,top,rect[2]-indent,bottom};if(gui_rect_empty(band))return;
        if(!region.empty() && region.back()[0]==band[0] && region.back()[2]==band[2] && region.back()[3]==top)region.back()[3]=bottom;
        else {if(region.size()>=4096)throw std::length_error("橢圓區域水平帶過多");region.push_back(band);}
    };
    const auto split=static_cast<std::int32_t>(eh/2);
    for(std::int32_t i=0;i<eh;++i){const auto top=i<=split?rect[1]+i:rect[3]-static_cast<std::int32_t>(eh)+i;
        const auto bottom=i==split?rect[3]-static_cast<std::int32_t>(eh)+i+1:top+1;
        append(top,bottom,profile[static_cast<std::size_t>(i)]);}
    return region;
}
}
