#pragma once

#include <stdint.h>

typedef struct {
    uint32_t split;
    int overlaps;
} MfFp16Plan;

static inline uint32_t mf_fp16_plan(uint64_t destination, uint64_t source, uint32_t count,
                                   uint64_t address_max, int expand, MfFp16Plan* plan) {
    plan->split = 0;
    plan->overlaps = 0;
    if (!count) return 0;
    if (!destination || !source) return UINT32_C(0x80004003);

    const uint64_t large = (uint64_t)count * 4;
    const uint64_t small = (uint64_t)count * 2;
    const uint64_t input = expand ? small : large;
    const uint64_t output = expand ? large : small;
    if (large > address_max || source > address_max || destination > address_max ||
        input - 1 > address_max - source || output - 1 > address_max - destination)
        return UINT32_C(0x80070057);

    plan->overlaps = destination >= source ? destination - source < input : source - destination < output;
    if (!plan->overlaps) return 0;

    // 以兩種元素寬度的交點切分，先轉換不會覆蓋另一段來源的那一側。
    uint64_t split = 0;
    if (expand && source > destination)
        split = (source - destination) / 2;
    if (!expand && destination > source) {
        const uint64_t delta = destination - source;
        split = delta / 2 + delta % 2;
    }
    plan->split = split < count ? (uint32_t)split : count;
    return 0;
}

static inline uint32_t mf_fp16_expand(uint16_t value) {
    const uint32_t sign = ((uint32_t)value & 0x8000u) << 16;
    uint32_t exponent = (value >> 10) & 31u;
    uint32_t fraction = value & 1023u;
    if (exponent == 31)
        return sign | 0x7f800000u | (fraction ? (fraction << 13) | 0x400000u : 0);
    if (exponent)
        return sign | ((exponent + 112) << 23) | (fraction << 13);
    if (!fraction) return sign;
    exponent = 113;
    while (fraction < 1024) {
        fraction <<= 1;
        --exponent;
    }
    return sign | (exponent << 23) | ((fraction & 1023u) << 13);
}

static inline uint16_t mf_fp16_narrow(uint32_t value) {
    const uint32_t sign = (value >> 16) & 0x8000u;
    const uint32_t magnitude = value & 0x7fffffffu;
    const uint32_t exponent = magnitude >> 23;
    if (exponent == 255) {
        const uint32_t payload = value & 0x7fffffu;
        return (uint16_t)(sign | 0x7c00u | (payload ? (payload >> 13) | 0x200u : 0));
    }
    if (exponent > 142) return (uint16_t)(sign | 0x7c00u);
    if (exponent < 102) return (uint16_t)sign;

    // 恰為中點時，僅在截斷後的尾數為奇數時進位。
    if (exponent >= 113) {
        const uint32_t rounded = magnitude + 0xfffu + ((magnitude >> 13) & 1u);
        return (uint16_t)(sign | ((rounded - 0x38000000u) >> 13));
    }
    const uint32_t significand = (value & 0x7fffffu) | 0x800000u;
    const uint32_t shift = 126 - exponent;
    const uint32_t rounded = significand + (UINT32_C(1) << (shift - 1)) - 1 +
                             ((significand >> shift) & 1u);
    return (uint16_t)(sign | (rounded >> shift));
}
