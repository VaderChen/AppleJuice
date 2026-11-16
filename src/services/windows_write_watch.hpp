#pragma once
#include "x64arm/windows_runtime.hpp"
#include "x64arm/memory.hpp"
#include <algorithm>

namespace x64arm::detail {
// 沿用核心 COW 的首次寫入陷入：原生 AOT、直譯及宿主寫入都經同一記憶體契約。
// VirtualQuery 仍依原本保護值回報 MEM_PRIVATE，COW 只用作內部追蹤。
template<class Action>
void visit_write_watch(RuntimeState& runtime,std::uint64_t begin,std::uint64_t end,Action action){
    auto it=runtime.virtual_regions.upper_bound(begin);
    if(it!=runtime.virtual_regions.begin())--it;
    for(;it!=runtime.virtual_regions.end() && it->first<end;++it){
        auto& region=it->second;const auto limit=it->first+region.size;
        if(region.write_watch && limit>begin)action(region,std::max(begin,it->first),std::min(end,limit));
    }
}
inline void collect_write_watch(RuntimeState& runtime,const GuestMemory& memory,std::uint64_t begin,std::uint64_t end){
    visit_write_watch(runtime,begin,end,[&](VirtualRegion& region,std::uint64_t first,std::uint64_t last){
        for(auto at=first&~UINT64_C(4095);at<last;at+=4096){
            const auto mapping=memory.query(GuestAddress{at});
            if(mapping && (mapping->permissions&permission(Access::Write)) && !memory.is_copy_on_write(GuestAddress{at}))region.written_pages.insert(at);
        }
    });
}
inline void arm_write_watch(RuntimeState& runtime,GuestMemory& memory,std::uint64_t begin,std::uint64_t end){
    std::vector<GuestMemory::Mapping> ranges;
    visit_write_watch(runtime,begin,end,[&](VirtualRegion& region,std::uint64_t first,std::uint64_t last){
        for(auto at=first&~UINT64_C(4095);at<last;at+=4096){
            const auto mapping=memory.query(GuestAddress{at});
            if(!mapping || !(mapping->permissions&permission(Access::Write)) || region.written_pages.contains(at) || memory.is_copy_on_write(GuestAddress{at}))continue;
            if(!ranges.empty() && ranges.back().base.value+ranges.back().size==at && ranges.back().permissions==mapping->permissions)ranges.back().size+=4096;
            else ranges.push_back({GuestAddress{at},4096,mapping->permissions});
        }
    });
    for(const auto& range:ranges)memory.make_copy_on_write(range.base,range.size,range.permissions);
}
inline void reset_write_watch(RuntimeState& runtime,GuestMemory& memory,std::uint64_t begin,std::uint64_t end){
    visit_write_watch(runtime,begin,end,[](VirtualRegion& region,std::uint64_t first,std::uint64_t last){
        region.written_pages.erase(region.written_pages.lower_bound(first&~UINT64_C(4095)),region.written_pages.lower_bound(last));
    });
    arm_write_watch(runtime,memory,begin,end);
}
}
