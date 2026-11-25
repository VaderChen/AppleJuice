#pragma once

#include <unicode/ucol.h>
#include <cstdint>
#include <vector>

namespace x64arm::icu_collation_key {
struct Bound {
    std::int32_t required{};
    std::vector<std::uint8_t> bytes;
};

inline Bound make_bound(std::vector<std::uint8_t> key,std::int32_t length,UColBoundMode mode,std::uint32_t levels,
                        std::int32_t capacity,UErrorCode& status){
    if(U_FAILURE(status))return {};
    if(key.empty() || key.size()>INT32_MAX-4U || length< -1 ||
       (length>=0 && static_cast<std::size_t>(length)>key.size()) ||
       mode<UCOL_BOUND_LOWER || mode>UCOL_BOUND_UPPER_LONG || capacity<0){
        status=U_ILLEGAL_ARGUMENT_ERROR;
        return {};
    }
    key.resize(key.size()+2);
    const auto initial=status;
    Bound result;
    result.required=ucol_getBound(key.data(),length,mode,levels,nullptr,0,&status);
    // ICU 的短緩衝區檢查未計入結尾 NUL，達到含 NUL 的長度才提供宿主緩衝區。
    if(U_SUCCESS(status) && result.required>0 && capacity>=result.required){
        result.bytes.resize(static_cast<std::size_t>(result.required));
        status=initial;
        const auto copied=ucol_getBound(key.data(),length,mode,levels,result.bytes.data(),result.required,&status);
        if(U_FAILURE(status) || copied!=result.required)result.bytes.clear();
    }
    return result;
}
}
