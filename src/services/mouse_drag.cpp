#include "x64arm/mouse_drag.hpp"
namespace x64arm::mouse_drag {
Detection::Detection(std::array<std::int32_t,2> start,std::int32_t width,std::int32_t height)
    :rectangle{std::int64_t{start[0]}-width,std::int64_t{start[1]}-height,
               std::int64_t{start[0]}+width,std::int64_t{start[1]}+height} {}
void Detection::move(std::array<std::int32_t,2> point,bool left_down){
    if(result!=Result::Tracking)return;
    if(!left_down){cancel();return;}
    if(point[0]<rectangle[0] || point[1]<rectangle[1] || point[0]>=rectangle[2] || point[1]>=rectangle[3])result=Result::Dragged;
}
void Detection::cancel(){if(result==Result::Tracking)result=Result::Cancelled;}
}
