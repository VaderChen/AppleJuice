#pragma once

#include <stdint.h>

namespace x64arm::icu_text {
struct Snapshot {
    uint32_t native_length;
    uint32_t count;
};
struct CodePoint {
    uint32_t native_start;
    uint32_t value;
};
static_assert(sizeof(Snapshot)==8 && sizeof(CodePoint)==8);
inline constexpr const char project_export[]="x64arm_utext_project";
}
