#pragma once
#include "guest_scalar_math_code.hpp"
#include "x64arm/windows_threads.hpp"

namespace x64arm::detail {
inline bool heap_inspection_service(std::string_view name) {
    return name=="HeapValidate" || name=="HeapWalk" || name=="HeapLock" ||
        name=="_heapchk" || name=="_heapwalk" || name=="_heapmin";
}
// 直譯器可在客體配置器的多個寫入之間切換虛擬執行緒。此時空閒串列、快取頭與
// 快取數量尚未完成發布，不能把中間狀態判為毀損。原生路徑也共用相同的入口範圍。
template<class Threads>
bool guest_heap_inspection_busy(std::uint64_t base,const Threads& threads,std::uint64_t observer,bool global_locked) {
    if(!base)return false;
    for(const auto& [id,thread]:threads.items){
        if(id==observer || thread.status==ThreadStatus::Exited)continue;
        const auto pc=thread.context.rip.value;
        if(pc<base)continue;
        // TLS 只須等待串列頭與數量的成組發布；函式其他指令沒有中間狀態。
        for(const auto& range:guest_heap_publication_ranges)
            if(pc-base>=range.begin && pc-base<range.end)return true;
        if(!global_locked)continue;
        for(const auto& routine:guest_scalar_math_routines){
            const auto name=routine.symbol;
            if(name!="malloc" && name!="free" && name!="_aligned_malloc" && name!="_aligned_free" &&
               name!="HeapAlloc" && name!="HeapFree")continue;
            if(pc-base>=routine.entry && pc-base<routine.slot)return true;
        }
    }
    return false;
}
}
