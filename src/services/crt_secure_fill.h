#pragma once
#include "crt_mbc_contract.h"

enum CrtFillLimit { CrtFillAll, CrtFillCharacters, CrtFillBytes };

/* 回傳 Windows errno_t；呼叫端只在 EINVAL 時通知無效參數處理器。 */
static inline int crt_secure_fill(void* memory, CrtMbcRead read_byte, CrtMbcWrite write_byte,
                                  const unsigned char* types, CrtMbcAddress text, CrtMbcAddress capacity,
                                  unsigned value, enum CrtFillLimit limit, CrtMbcAddress count) {
    if (!text || !capacity)
        return !text && !capacity && limit != CrtFillAll && !count ? 0 : 22;
    const unsigned char low = (unsigned char)value;
    const unsigned char high = types ? (unsigned char)(value >> 8) : 0;
    if (types && (high ? (!low || !(types[high] & CRT_MBC_LEAD)) : (types[low] & CRT_MBC_LEAD))) {
        write_byte(memory, text, 0);
        return 42;
    }
    int status = 0;
    if (high && limit == CrtFillBytes) {
        if (count & 1) status = 42;
        count /= 2;
    }
    int pending_trail = 0;
    CrtMbcAddress offset = 0;
    while (offset < capacity) {
        const unsigned first = read_byte(memory, text + offset);
        if (!first) return status;
        if (capacity - offset < 2) break;
        if (limit != CrtFillAll && !count) {
            /* 原字串的前導位元組剛被覆蓋，清除遺留的後隨位元組。 */
            if (pending_trail) {
                write_byte(memory, text + offset, read_byte(memory, text + offset + 1) ? ' ' : 0);
                status = 42;
                pending_trail = 0;
            }
            ++offset;
            continue;
        }
        unsigned second = 0;
        if (high) {
            second = read_byte(memory, text + offset + 1);
            if (!second) {
                write_byte(memory, text + offset, 0);
                return 42;
            }
        }
        pending_trail = types && !pending_trail && (types[first] & CRT_MBC_LEAD);
        write_byte(memory, text + offset++, high ? high : low);
        if (high) {
            if (capacity - offset < 2) break;
            pending_trail = !pending_trail && (types[second] & CRT_MBC_LEAD);
            write_byte(memory, text + offset++, low);
        }
        if (limit != CrtFillAll) --count;
    }
    write_byte(memory, text, 0);
    return 22;
}
