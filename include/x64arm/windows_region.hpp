#pragma once
#include <array>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
namespace x64arm {
using GuiRect=std::array<std::int32_t,4>;
using GuiRegion=std::vector<GuiRect>;
inline bool gui_rect_empty(const GuiRect& r){return r[0]>=r[2] || r[1]>=r[3];}
inline GuiRect gui_region_bounds(const GuiRegion& region){
    if(region.empty())return {};auto result=region.front();
    for(const auto& r:region)for(unsigned i=0;i<4;++i)result[i]=i<2?std::min(result[i],r[i]):std::max(result[i],r[i]);return result;
}
inline unsigned gui_region_kind(const GuiRegion& region){return region.empty()?1U:region.size()==1?2U:3U;}
// 以水平帶正規化，保留洞與分離區塊；來源可與 API 的目的 region 相同。
GuiRegion gui_region_combine(const GuiRegion& a,const GuiRegion& b,unsigned mode);
inline bool gui_region_coordinates(const GuiRegion& region){for(const auto& r:region)for(auto v:r)if(v<-(1<<26) || v>=(1<<26))return false;return true;}
}
