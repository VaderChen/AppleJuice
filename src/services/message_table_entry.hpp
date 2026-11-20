#pragma once
#include "x64arm/windows_services.hpp"

namespace x64arm {
// MESSAGE_RESOURCE_DATA 與變長項目的偏移一律以整張資源的大小檢查。
inline std::uint64_t message_table_entry(const GuestMemory& memory,std::uint64_t base,std::uint32_t size,std::uint32_t id){
    if(size<4)return 0;
    const auto count=memory.load(GuestAddress{base},32);
    if(count>(size-4)/12)return 0;
    for(std::uint64_t block=0;block<count;++block){
        const auto at=base+4+block*12;
        const auto first=memory.load(GuestAddress{at},32),last=memory.load(GuestAddress{at+4},32);
        if(id<first || id>last)continue;
        auto offset=memory.load(GuestAddress{at+8},32);
        for(auto n=first;offset<=size && size-offset>=4;++n){
            const auto length=memory.load(GuestAddress{base+offset},16);
            if(length<4 || length>size-offset)return 0;
            if(n==id)return base+offset;
            offset+=length;
        }
        break;
    }
    return 0;
}
}
