#pragma once
#include "crt_mbc_contract.h"

/* 八位元組的可複製 mbstate_t。UTF-8 前綴與現有 CRT 的 restartable 解碼器共用表示法。 */
typedef struct {
    unsigned char bytes[8];
} CrtUcharState;
enum CrtUcharFunction { CrtMbrtoc16, CrtMbrtoc32, CrtC16rtomb, CrtC32rtomb };
enum { CrtUcharInvalid = -1, CrtUcharPartial = -2, CrtUcharPending = -3 };
enum { CrtUcharPrefix = 0x6d, CrtUcharLow = 0x16, CrtUcharHigh = 0x17 };

static inline void crt_uchar_clear(CrtUcharState* state) {
    for (unsigned i = 0; i < 8; ++i)
        state->bytes[i] = 0;
}
static inline void crt_uchar_save_unit(CrtUcharState* state, unsigned unit, unsigned char tag) {
    crt_uchar_clear(state);
    state->bytes[0] = (unsigned char)unit;
    state->bytes[1] = (unsigned char)(unit >> 8);
    state->bytes[7] = tag;
}
static inline unsigned crt_uchar_saved_unit(const CrtUcharState* state) {
    return (unsigned)state->bytes[0] | ((unsigned)state->bytes[1] << 8);
}
static inline int crt_uchar_invalid(CrtUcharState* state) {
    crt_uchar_clear(state);
    return CrtUcharInvalid;
}
static inline unsigned crt_uchar_width(unsigned lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xc2 && lead <= 0xdf) return 2;
    if (lead >= 0xe0 && lead <= 0xef) return 3;
    if (lead >= 0xf0 && lead <= 0xf4) return 4;
    return 0;
}
/* 即使後續位元組尚未抵達，第二個位元組已足以判斷過長、代理碼或超出 U+10FFFF。 */
static inline int crt_uchar_continuation(unsigned lead, unsigned index, unsigned byte) {
    if (byte < 0x80 || byte > 0xbf) return 0;
    if (index != 1) return 1;
    return !(lead == 0xe0 && byte < 0xa0) && !(lead == 0xed && byte >= 0xa0) && !(lead == 0xf0 && byte < 0x90) &&
           !(lead == 0xf4 && byte >= 0x90);
}
static inline int crt_uchar_decode(CrtUcharState* state, int utf16, const void* memory, CrtMbcRead read_byte,
                                   CrtMbcAddress source, CrtMbcAddress available, unsigned* result) {
    unsigned char* bytes = state->bytes;
    if (utf16 && bytes[7] == CrtUcharLow) {
        *result = crt_uchar_saved_unit(state);
        crt_uchar_clear(state);
        return CrtUcharPending;
    }
    if (!source) available = 1;
    if (!available) return CrtUcharPartial;
    unsigned used = 0, stored = bytes[7] == CrtUcharPrefix ? bytes[6] : 0;
    if ((bytes[7] && bytes[7] != CrtUcharPrefix) || stored > 3) return crt_uchar_invalid(state);
    if (!stored) {
        bytes[0] = source ? (unsigned char)read_byte(memory, source) : 0;
        used = 1;
        stored = 1;
    }
    const unsigned lead = bytes[0], width = crt_uchar_width(lead);
    if (!width || stored > width) return crt_uchar_invalid(state);
    while (stored < width) {
        if (used == available) {
            bytes[6] = (unsigned char)stored;
            bytes[7] = CrtUcharPrefix;
            return CrtUcharPartial;
        }
        const unsigned byte = source ? read_byte(memory, source + used) : 0;
        if (!crt_uchar_continuation(lead, stored, byte)) return crt_uchar_invalid(state);
        bytes[stored++] = (unsigned char)byte;
        ++used;
    }
    unsigned value = width == 1 ? lead : lead & (0x7fU >> width);
    for (unsigned i = 1; i < width; ++i)
        value = (value << 6) | (bytes[i] & 63U);
    if (utf16 && value >= 0x10000) {
        value -= 0x10000;
        crt_uchar_save_unit(state, 0xdc00U + (value & 1023), CrtUcharLow);
        *result = 0xd800U + (value >> 10);
    } else {
        crt_uchar_clear(state);
        *result = value;
    }
    return *result ? (int)used : 0;
}
static inline int crt_uchar_encode(CrtUcharState* state, int utf16, unsigned value, unsigned char output[4]) {
    if (utf16 && state->bytes[7] == CrtUcharHigh) {
        if (value < 0xdc00 || value > 0xdfff) return crt_uchar_invalid(state);
        value = 0x10000 + ((crt_uchar_saved_unit(state) - 0xd800) << 10) + (value - 0xdc00);
    } else if (state->bytes[7]) {
        return crt_uchar_invalid(state);
    } else if (utf16 && value >= 0xd800 && value <= 0xdbff) {
        crt_uchar_save_unit(state, value, CrtUcharHigh);
        return 0;
    }
    if ((value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) return crt_uchar_invalid(state);
    crt_uchar_clear(state);
    if (value < 0x80) {
        output[0] = (unsigned char)value;
        return 1;
    }
    const unsigned count = value < 0x800 ? 2U : value < 0x10000 ? 3U : 4U;
    for (unsigned i = count - 1; i > 0; --i) {
        output[i] = (unsigned char)(0x80 | (value & 63));
        value >>= 6;
    }
    output[0] = (unsigned char)((count == 2 ? 0xc0 : count == 3 ? 0xe0 : 0xf0) | value);
    return (int)count;
}
