#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace x64arm {
// SORT_TEXTURE 要求將相同紋理分組，未要求依 COM 位址遞增。
// 以本次 Flush 的首次出現順序定義群組，避免配置策略改變重疊 sprite 的捨入結果。
template<class Items,class Depth>
void d3dx_sort_sprites(Items& items,bool texture_sort,bool depth_sort,bool ascending,Depth&& depth){
    if(!texture_sort && !depth_sort)return;
    std::unordered_map<std::uint64_t,std::size_t> groups;
    if(texture_sort)for(const auto& item:items)groups.try_emplace(item.texture,groups.size());
    std::stable_sort(items.begin(),items.end(),[&](const auto& a,const auto& b){
        if(depth_sort){const auto az=depth(a),bz=depth(b);if(az!=bz)return ascending?az<bz:az>bz;}
        return texture_sort && groups.at(a.texture)<groups.at(b.texture);
    });
}
} // namespace x64arm
