#pragma once
#include <stdint.h>

/* MAPI 的高 16 位是屬性 ID，低 16 位是型別，0x1000 表示多值。 */
static inline int mapi_valid_property_tag(uint32_t tag)
{
    switch (tag & 0xefffu) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
    case 10: case 11: case 13: case 20: case 30: case 31: case 64: case 72: case 258:
        return 1;
    default:
        return 0;
    }
}

static inline int mapi_property_matches(uint32_t candidate, uint32_t query, int id_only)
{
    if ((candidate >> 16) != (query >> 16)) return 0;
    if (id_only) return 1;
    return mapi_valid_property_tag(candidate) && (!(query & 0xffffu) || candidate == query);
}

static inline unsigned mapi_property_stride(unsigned pointer_bytes)
{
    return 8 + 2 * pointer_bytes;
}
