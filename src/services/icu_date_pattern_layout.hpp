#pragma once

#include <stdint.h>

namespace x64arm::icu_date_pattern {
constexpr unsigned field_count=16;
constexpr unsigned decimal_index=0;
constexpr unsigned date_time_index=1;
constexpr unsigned name_index=2;
constexpr unsigned format_index=2+field_count;
constexpr unsigned value_count=2+2*field_count;

struct PatternValue {
    int32_t length;
    uint32_t reserved;
    uintptr_t address;
};

struct GuestPatternView {
    int32_t hour_cycle;
    int32_t hour_status;
    PatternValue values[value_count];
};
}
