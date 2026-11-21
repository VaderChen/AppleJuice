#pragma once

#include <stddef.h>
#include <stdint.h>

namespace x64arm::icu_region {
#define ICU_REGION_LAYOUT(regions,words,codes,numerics) \
    inline constexpr size_t count=regions,containment_words=words,code_count=codes,numeric_count=numerics;
#define ICU_REGION_ENTRY(...)
#define ICU_REGION_CODE(...)
#define ICU_REGION_NUMBER(...)
#include "guest_icu_region_catalog.inc"
#undef ICU_REGION_NUMBER
#undef ICU_REGION_CODE
#undef ICU_REGION_ENTRY
#undef ICU_REGION_LAYOUT

inline constexpr uint16_t no_region=UINT16_MAX;

struct View {
    char code[4];
    int32_t numeric;
    uint16_t parent;
    uint8_t type;
    uint8_t reserved;
    uint32_t padding;
    uint64_t descendants[containment_words];
};

static_assert(count<no_region && containment_words==(count+63U)/64U);
static_assert(offsetof(View,descendants)==16 && sizeof(View)==16+8*containment_words);
}
