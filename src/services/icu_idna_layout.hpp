#pragma once

#include <stddef.h>
#include <stdint.h>

namespace x64arm::icu_idna {
enum class Conversion : size_t {
#define ICU_IDNA_API(...)
#define ICU_IDNA_QUERY(name,count,type,label) name,
#include "icu_idna_services.inc"
#undef ICU_IDNA_QUERY
#undef ICU_IDNA_API
    count
};

struct View {
    static constexpr uint32_t signature=0x31414e49;
    uint32_t magic{signature},reserved{};
    uint64_t fallback[static_cast<size_t>(Conversion::count)]{};
};

static_assert(offsetof(View,fallback)==8);
}
