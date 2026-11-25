#pragma once
#include <stdint.h>
#include <unicode/utypes.h>

namespace x64arm::icu_numbering_system {
struct View {
    static constexpr uint32_t signature=0x3153594e;
    uint32_t magic{signature};
    int32_t radix{};
    uint32_t algorithmic{};
    int32_t description_length{};
    uint32_t description_offset{};
};
static_assert(sizeof(View)==20 && alignof(View)==4);

struct DescriptionCopy {
    int32_t units{};
    bool terminate{};
};

constexpr DescriptionCopy plan_description(int32_t length,int32_t capacity,bool has_output,UErrorCode& status){
    if(capacity<0 || (capacity>0 && !has_output)){
        status=U_ILLEGAL_ARGUMENT_ERROR;
        return {};
    }
    if(length>capacity){
        status=U_BUFFER_OVERFLOW_ERROR;
        return {};
    }
    if(length==capacity){
        status=U_STRING_NOT_TERMINATED_WARNING;
        return {length,false};
    }
    if(status==U_STRING_NOT_TERMINATED_WARNING)status=U_ZERO_ERROR;
    return {length,true};
}

}
