#pragma once

#include <stdint.h>

namespace x64arm::icu_number {
struct FieldPosition {
    int32_t field,begin,end;
};

struct FieldIteratorView {
    int32_t count,cursor;
    uintptr_t records;
};

struct FormattableView {
    int32_t type;
    uint32_t numeric;
};
}
