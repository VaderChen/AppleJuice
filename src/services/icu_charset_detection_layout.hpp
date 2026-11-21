#pragma once

#include <stdint.h>
#include <unicode/ucsdet.h>

namespace x64arm::icu_charset_detection {
struct Detector {
    static constexpr uint32_t signature=0x31544443;
    uint32_t magic{signature};
    UBool filter{},filter_dirty{};
    uint16_t reserved{};

    UBool enable_filter(UBool enabled){
        const auto previous=filter;
        filter=enabled;
        filter_dirty=true;
        return previous;
    }
};

struct Match {
    static constexpr uint32_t signature=0x314d4443;
    uint32_t magic{signature};
    int32_t confidence{};
    uint64_t name{},language{};
};

static_assert(sizeof(Detector)==8 && sizeof(Match)==24);
}
