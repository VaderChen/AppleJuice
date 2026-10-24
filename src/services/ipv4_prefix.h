#ifndef X64ARM_IPV4_PREFIX_H
#define X64ARM_IPV4_PREFIX_H
#include <stdint.h>

/* Windows ULONG 中的網路位元組順序；API 與介面快照沿用同一轉換。 */
static inline uint32_t x64arm_ipv4_mask(uint32_t length) {
    uint32_t mask = length >= 32 ? UINT32_MAX : length ? UINT32_MAX << (32 - length) : 0;
    return ((mask & UINT32_C(0xff)) << 24) | ((mask & UINT32_C(0xff00)) << 8) |
           ((mask >> 8) & UINT32_C(0xff00)) | (mask >> 24);
}
static inline uint32_t x64arm_ipv4_length(uint32_t mask) {
    uint32_t length = 0;
    while (length < 32 && (mask & (UINT32_C(0x80) >> (length % 8)) << (length / 8 * 8))) ++length;
    return mask == x64arm_ipv4_mask(length) ? length : UINT32_MAX;
}
#endif
