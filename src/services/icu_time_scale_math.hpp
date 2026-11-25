#pragma once
#include <unicode/utmscale.h>
#include <cstdint>
#include <initializer_list>

namespace x64arm::icu_time_scale {

inline constexpr std::int64_t scales[UDTS_MAX_SCALE][UTSV_MAX_SCALE_VALUE]={
#include "icu_time_scale_data.inc"
};

constexpr std::int64_t divide(std::int64_t value,std::int64_t units){
    if(units==1)return value;
#if defined(__i386__)
    // 32 位元客體用移位長除法，避免依賴額外的編譯器執行階段。
    auto remaining=value<0?0-std::uint64_t(value):std::uint64_t(value);
    const auto divisor=std::uint64_t(units);
    if(remaining<divisor)return 0;
    auto shift=__builtin_clzll(divisor)-__builtin_clzll(remaining);
    auto aligned=divisor<<shift;
    std::uint64_t quotient=0;
    for(;shift>=0;--shift,aligned>>=1){
        if(remaining<aligned)continue;
        remaining-=aligned;
        quotient|=UINT64_C(1)<<shift;
    }
    return value<0?-static_cast<std::int64_t>(quotient):static_cast<std::int64_t>(quotient);
#else
    return value/units;
#endif
}

constexpr const std::int64_t* row(int scale,UErrorCode* status){
    if(!status || *status>U_ZERO_ERROR)return nullptr;
    if(scale>=0 && scale<UDTS_MAX_SCALE)return scales[scale];
    *status=U_ILLEGAL_ARGUMENT_ERROR;
    return nullptr;
}

constexpr std::int64_t value(int scale,int field,UErrorCode* status){
    const auto* data=row(scale,status);
    if(!data)return 0;
    if(field>=0 && field<UTSV_MAX_SCALE_VALUE)return data[field];
    *status=U_ILLEGAL_ARGUMENT_ERROR;
    return 0;
}

constexpr std::int64_t convert(std::int64_t input,int scale,UErrorCode* status,bool to_external){
    const auto* data=row(scale,status);
    if(!data)return 0;
    const auto minimum=data[to_external?UTSV_TO_MIN_VALUE:UTSV_FROM_MIN_VALUE];
    const auto maximum=data[to_external?UTSV_TO_MAX_VALUE:UTSV_FROM_MAX_VALUE];
    if(input<minimum || input>maximum){
        *status=U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    const auto units=data[UTSV_UNITS_VALUE];
    const auto epoch=data[UTSV_EPOCH_OFFSET_VALUE];
    if(!to_external)return (input+epoch)*units;

    // 捨入至最近單位，中點遠離零。先改用反向偏移可避開極端值溢位。
    const auto half=data[UTSV_UNITS_ROUND_VALUE];
    if(input<data[UTSV_MIN_ROUND_VALUE])
        return divide(input+half,units)-data[UTSV_EPOCH_OFFSET_PLUS_1_VALUE];
    if(input>data[UTSV_MAX_ROUND_VALUE])
        return divide(input-half,units)-data[UTSV_EPOCH_OFFSET_MINUS_1_VALUE];
    return divide(input+(input<0?-half:half),units)-epoch;
}

constexpr bool check_bounds(){
    for(int scale=0;scale<UDTS_MAX_SCALE;++scale){
        UErrorCode status=U_ZERO_ERROR;
        const auto* data=scales[scale];
        for(const auto edge:{UTSV_FROM_MIN_VALUE,UTSV_FROM_MAX_VALUE}){
            const auto boundary=data[edge];
            if(convert(convert(boundary,scale,&status,false),scale,&status,true)!=boundary)return false;
        }
        for(const auto edge:{UTSV_TO_MIN_VALUE,UTSV_TO_MAX_VALUE})
            (void)convert(data[edge],scale,&status,true);
        if(status>U_ZERO_ERROR)return false;
    }
    return true;
}
static_assert(check_bounds());

}
