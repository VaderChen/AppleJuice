#pragma once
#include "x64arm/windows_gui.hpp"
#include <cmath>
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace x64arm {
inline GuiTransform gui_multiply(const GuiTransform& a,const GuiTransform& b){
    return {a[0]*b[0]+a[1]*b[2],a[0]*b[1]+a[1]*b[3],a[2]*b[0]+a[3]*b[2],a[2]*b[1]+a[3]*b[3],a[4]*b[0]+a[5]*b[2]+b[4],a[4]*b[1]+a[5]*b[3]+b[5]};
}
inline bool gui_valid_transform(const GuiTransform& m){return std::all_of(m.begin(),m.end(),[](double v){return std::isfinite(v) && std::abs(v)<=std::numeric_limits<float>::max();});}
inline std::optional<GuiTransform> gui_inverse(const GuiTransform& m){
    const auto determinant=m[0]*m[3]-m[1]*m[2];if(!std::isfinite(determinant) || determinant==0)return {};
    GuiTransform out{m[3]/determinant,-m[1]/determinant,-m[2]/determinant,m[0]/determinant,0,0};out[4]=-m[4]*out[0]-m[5]*out[2];out[5]=-m[4]*out[1]-m[5]*out[3];if(!gui_valid_transform(out))return {};return out;
}
inline GuiTransform gui_device_transform(const GuiDc& dc){
    const double x=static_cast<double>(dc.viewport_extent[0])/dc.window_extent[0],y=static_cast<double>(dc.viewport_extent[1])/dc.window_extent[1];
    return gui_multiply(dc.world,{x,0,0,y,dc.viewport_origin[0]-dc.window_origin[0]*x,dc.viewport_origin[1]-dc.window_origin[1]*y});
}
inline std::array<double,2> gui_point(const GuiTransform& m,double x,double y){return {x*m[0]+y*m[2]+m[4],x*m[1]+y*m[3]+m[5]};}
inline std::optional<std::array<std::int32_t,2>> gui_integer_point(const GuiTransform& m,double x,double y){
    const auto p=gui_point(m,x,y);std::array<std::int32_t,2> result{};for(unsigned i=0;i<2;++i){const auto v=std::round(p[i]);if(!std::isfinite(v) || v<INT32_MIN || v>INT32_MAX)return {};result[i]=static_cast<std::int32_t>(v);}return result;
}
inline GuiRect gui_transform_bounds(const GuiTransform& m,const GuiRect& r){
    const std::array points{gui_point(m,r[0],r[1]),gui_point(m,r[2],r[1]),gui_point(m,r[2],r[3]),gui_point(m,r[0],r[3])};
    std::array<double,4> out{points[0][0],points[0][1],points[0][0],points[0][1]};for(const auto& p:points){out[0]=std::min(out[0],p[0]);out[1]=std::min(out[1],p[1]);out[2]=std::max(out[2],p[0]);out[3]=std::max(out[3],p[1]);}
    GuiRect result{};for(unsigned i=0;i<4;++i){const auto v=i<2?std::floor(out[i]):std::ceil(out[i]);if(!std::isfinite(v) || v<INT32_MIN || v>INT32_MAX)throw std::length_error("轉換座標超出範圍");result[i]=static_cast<std::int32_t>(v);}return result;
}
inline GuiRegion gui_transform_region_rect(const GuiTransform& m,const GuiRect& r){
    if(r[2]<=r[0] || r[3]<=r[1])return {};const auto bounds=gui_transform_bounds(m,r);
    if(m[1]==0 && m[2]==0)return GuiRegion{bounds};
    if(static_cast<std::int64_t>(bounds[3])-bounds[1]>65536)throw std::length_error("轉換裁切區域過大");
    const std::array points{gui_point(m,r[0],r[1]),gui_point(m,r[2],r[1]),gui_point(m,r[2],r[3]),gui_point(m,r[0],r[3])};GuiRegion result;
    for(auto y=bounds[1];y<bounds[3];++y){const double scan=y+0.5;std::array<double,4> crossings{};unsigned n=0;
        for(unsigned i=0;i<4;++i){const auto& a=points[i];const auto& b=points[(i+1)%4];if((a[1]<=scan && b[1]>scan) || (b[1]<=scan && a[1]>scan))crossings[n++]=a[0]+(scan-a[1])*(b[0]-a[0])/(b[1]-a[1]);}
        if(n<2)continue;std::sort(crossings.begin(),crossings.begin()+n);const auto left=static_cast<std::int32_t>(std::ceil(crossings[0]-0.5)),right=static_cast<std::int32_t>(std::ceil(crossings[n-1]-0.5));if(right>left){if(!result.empty() && result.back()[0]==left && result.back()[2]==right && result.back()[3]==y)result.back()[3]=y+1;else result.push_back({left,y,right,y+1});}}
    return result;
}
}
