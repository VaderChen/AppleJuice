#pragma once
#include "crt_mbc_contract.h"

/* STRUNCATE 可保留原 errno，也可能同時修復了目的字串的孤立前導位元組。 */
struct CrtAppendResult { unsigned status, error; };
static inline struct CrtAppendResult crt_append_result(unsigned status, unsigned error) {
    struct CrtAppendResult result = {status, error};
    return result;
}

static inline struct CrtAppendResult crt_secure_append(void* memory, CrtMbcRead read_byte,
        CrtMbcWrite write_byte, const unsigned char* types, CrtMbcAddress dest, CrtMbcAddress capacity,
        CrtMbcAddress source, CrtMbcAddress count, int byte_limit, int truncate) {
    if (!dest && !capacity && !count) return crt_append_result(0, 0);
    if (!dest || !capacity) return crt_append_result(22, 22);
    if (!source && count) {
        write_byte(memory, dest, 0);
        return crt_append_result(22, 22);
    }

    CrtMbcAddress end = 0;
    unsigned pending = 0;
    while (end < capacity) {
        const unsigned value = read_byte(memory, dest + end);
        if (!value) break;
        pending = !pending && types && (types[value] & CRT_MBC_LEAD);
        ++end;
    }
    if (end == capacity) {
        write_byte(memory, dest, 0);
        return crt_append_result(22, 22);
    }
    unsigned error = 0;
    if (pending) {
        write_byte(memory, dest + --end, 0);
        pending = 0;
        error = 42;
    }

    while (count) {
        const unsigned first = read_byte(memory, source++);
        if (!first) break;
        const int lead = !pending && types && (types[first] & CRT_MBC_LEAD);
        const int pair = lead && !byte_limit && !truncate;
        unsigned trail = 0;
        if (pair) {
            trail = read_byte(memory, source++);
            if (!trail) {
                error = 42;
                break;
            }
        }
        if (capacity - end <= (pair ? 2u : 1u)) {
            if (byte_limit && lead && (count == 1 || !read_byte(memory, source))) {
                error = 42;
                break;
            }
            if (truncate) {
                write_byte(memory, dest + end - pending, 0);
                return crt_append_result(80, error);
            }
            write_byte(memory, dest, 0);
            return crt_append_result(34, 34);
        }
        write_byte(memory, dest + end++, (unsigned char)first);
        if (pair) write_byte(memory, dest + end++, (unsigned char)trail);
        pending = lead && !pair;
        if (!truncate) --count;
    }
    if (byte_limit && pending) {
        --end;
        error = 42;
    }
    write_byte(memory, dest + end, 0);
    return crt_append_result(error, error);
}
