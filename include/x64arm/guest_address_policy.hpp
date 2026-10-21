#pragma once
#include "x64arm/memory.hpp"
#include <algorithm>
#include <cstdlib>

namespace x64arm {
inline bool prefer_low_guest_stack() {
    const auto* setting=std::getenv("X64ARM_LOW_STACK");
    return setting && std::string_view(setting)=="1";
}
// 在 64 KiB 粒度上選擇未映射空洞；呼叫端可另外提供尚未提交的 reservation。
// 只選址，不修改映射、容量、權限或已存在的配置；不足時由呼叫端沿用高位址政策。
template<class ReservedEnd>
std::optional<GuestAddress> find_guest_address_gap(const GuestMemory& memory,std::uint64_t begin,
                                                std::uint64_t size,std::uint64_t limit,ReservedEnd reserved_end) {
    constexpr std::uint64_t alignment=65536;
    if(!size || begin<alignment || begin>UINT64_MAX-(alignment-1))return {};
    auto at=(begin+alignment-1)&~(alignment-1);
    while(at<limit && size<=limit-at){
        auto next=at;
        if(const auto mapping=memory.query(GuestAddress{at}))next=std::max(next,mapping->base.value+mapping->size);
        if(const auto mapping=memory.next_mapping(GuestAddress{at});mapping && mapping->base.value<at+size)
            next=std::max(next,mapping->base.value+mapping->size);
        next=std::max(next,reserved_end(at,at+size));
        if(next==at)return GuestAddress{at};
        if(next>UINT64_MAX-(alignment-1))return {};
        at=(next+alignment-1)&~(alignment-1);
    }
    return {};
}
inline std::optional<GuestAddress> find_guest_address_gap(const GuestMemory& memory,std::uint64_t begin,
                                                        std::uint64_t size,std::uint64_t limit) {
    return find_guest_address_gap(memory,begin,size,limit,[](auto first,auto){return first;});
}
}
