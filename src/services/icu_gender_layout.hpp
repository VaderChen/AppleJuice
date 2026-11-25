#pragma once
#include <stdint.h>
#include <unicode/ugender.h>

namespace x64arm::icu_gender {
enum class Style : uint32_t { Neutral, MixedNeutral, MaleTaints };
inline constexpr uint32_t count=3;

constexpr uint32_t style_index(uint64_t base,uint64_t address){
    if(address<base || (address-base)%sizeof(Style))return count;
    const auto index=(address-base)/sizeof(Style);
    return index<count?static_cast<uint32_t>(index):count;
}

// 依 ICU 的三種分類規則讀取所需元素，宿主服務與客體入口共用。
template<class Read>
constexpr int32_t list_gender(Style style,int32_t length,Read read){
    if(!length)return UGENDER_OTHER;
    if(length==1)return read(0);
    if(style==Style::Neutral)return UGENDER_OTHER;
    if(style==Style::MaleTaints){
        for(int32_t i=0;i<length;++i)if(read(i)!=UGENDER_FEMALE)return UGENDER_MALE;
        return UGENDER_FEMALE;
    }
    bool has_male=false,has_female=false;
    for(int32_t i=0;i<length;++i){
        const auto gender=read(i);
        if(gender==UGENDER_OTHER)return UGENDER_OTHER;
        has_male|=gender==UGENDER_MALE;
        has_female|=gender==UGENDER_FEMALE;
        if(has_male && has_female)return UGENDER_OTHER;
    }
    return has_male?UGENDER_MALE:UGENDER_FEMALE;
}
}
