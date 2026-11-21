#pragma once

#include <stdint.h>

namespace x64arm::icu_normalization {
constexpr const char* profile_names[]{"nfc","nfkc","nfkc_cf","nfkc_scf"};
constexpr unsigned profile_count=4*(sizeof(profile_names)/sizeof(*profile_names));

enum GuestQuery {
#define ICU_NORM_API(name,count)
#define ICU_NORM_QUERY(name,count) query_##name,
#include "icu_normalization_services.inc"
#undef ICU_NORM_QUERY
#undef ICU_NORM_API
    query_count
};

struct GuestNormalizer {
    uint32_t profile;
    uint32_t reserved;
    uintptr_t gateway[query_count];
};

struct ComposePair {
    uint32_t first,second,result;
    uint16_t profiles;
};
}
