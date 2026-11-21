#pragma once

#include <stdint.h>

namespace x64arm::icu_calendar {
constexpr unsigned attribute_count=5;
struct GuestCalendarView {
    int32_t attributes[attribute_count];
    uint32_t reserved;
    uintptr_t type;
};
}
