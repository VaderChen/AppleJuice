#pragma once
#include <stdint.h>

namespace x64arm::icu_collation_key {
constexpr uint32_t primary(uint32_t order){return order>>16;}
constexpr uint32_t secondary(uint32_t order){return (order>>8)&0xffU;}
constexpr uint32_t tertiary(uint32_t order){return order&0xffU;}

template<class Read>
constexpr uint32_t hash(uint32_t length,Read read){
    const auto step=length<32?1U:length/32U;
    uint32_t value{};
    for(uint32_t offset=0;offset<length;offset+=step)value=value*37U+read(offset);
    return value==0 || value==2?1U:value;
}
}
