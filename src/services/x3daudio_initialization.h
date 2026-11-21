#pragma once
#include <stdint.h>

#define X3DAUDIO_INSTANCE_SIGNATURE UINT32_C(0x58443341)
#define X3DAUDIO_INSTANCE_WORDS 5
#define X3DAUDIO_INSTANCE_BYTES (X3DAUDIO_INSTANCE_WORDS * sizeof(uint32_t))
#define X3DAUDIO_INVALID_ARGUMENT UINT32_C(0x80070057)

static inline int x3daudio_valid_instance(uint32_t channels,uint32_t speed)
{
    // binary32 的有限正規正數；宿主與客體使用相同判斷。
    return channels && !(channels & ~UINT32_C(0x3ffff)) &&
        speed >= UINT32_C(0x00800000) && speed < UINT32_C(0x7f800000);
}
