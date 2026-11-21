#pragma once

#include <stdint.h>
#include <unicode/unumberrangeformatter.h>

namespace x64arm::icu_modern_number {
struct RangeView {
    static constexpr uint32_t signature=0x554e5200;
    uint32_t magic{signature};
    UNumberRangeIdentityResult identity{UNUM_IDENTITY_RESULT_COUNT};
};
static_assert(sizeof(RangeView)==8);
}
