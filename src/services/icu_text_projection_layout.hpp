#pragma once

#include <stdint.h>

namespace x64arm::icu_text {
struct Snapshot {
    int64_t native_length;
    uint32_t count;
    uint32_t reserved;
};
struct CodePoint {
    int64_t native_start;
    uint32_t value;
    uint32_t reserved;
};
static_assert(sizeof(Snapshot)==16 && sizeof(CodePoint)==16);
inline constexpr const char own_utf16_export[]="x64arm_utext_own_utf16";
inline constexpr const char retain_project_export[]="x64arm_utext_retain_project";
inline constexpr const char project_export[]="x64arm_utext_project";
}
