#pragma once

#include <stdint.h>

#define HID_ARRAY_SUCCESS UINT32_C(0x00110000)
#define HID_ARRAY_INVALID_PARAMETER UINT32_C(0xc000000d)

static inline uint32_t hid_diff_validate(const uint64_t addresses[4], uint32_t count, unsigned width,
                                        uint64_t address_max) {
    if (!count) return HID_ARRAY_SUCCESS;
    const uint64_t size = (uint64_t)count * width;
    for (unsigned i = 0; i < 4; ++i)
        if (!addresses[i] || addresses[i] > address_max || size - 1 > address_max - addresses[i])
            return HID_ARRAY_INVALID_PARAMETER;

    // Windows 宣告為獨立的輸出陣列；禁止覆寫尚未完成比對的輸入和另一份輸出。
    for (unsigned out = 2; out < 4; ++out) {
        for (unsigned other = 0; other < out; ++other) {
            const uint64_t a = addresses[out], b = addresses[other];
            if ((a >= b ? a - b : b - a) < size) return HID_ARRAY_INVALID_PARAMETER;
        }
    }
    return HID_ARRAY_SUCCESS;
}

static inline uint16_t hid_read_usage(const uint8_t* bytes) {
    return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}

static inline void hid_write_usage(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static inline uint32_t hid_list_item(const uint8_t* at, unsigned width) {
    const uint32_t usage = hid_read_usage(at);
    return width == 4 ? usage | (uint32_t)hid_read_usage(at + 2) << 16 : usage;
}

static inline void hid_write_list_item(uint8_t* at, uint32_t item, unsigned width) {
    hid_write_usage(at, (uint16_t)item);
    if (width == 4) hid_write_usage(at + 2, (uint16_t)(item >> 16));
}

static inline void hid_diff_direction(const uint8_t* base, const uint8_t* other, uint8_t* output,
                                      uint32_t count, unsigned width) {
    uint32_t written = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t usage = hid_list_item(base + width * (uintptr_t)i, width);
        if (!usage) break;
        uint32_t j = 0;
        for (; j < count; ++j) {
            const uint32_t candidate = hid_list_item(other + width * (uintptr_t)j, width);
            if (candidate == usage) break;
            if (!candidate) { j = count; break; }
        }
        if (j == count) hid_write_list_item(output + width * (uintptr_t)written++, usage, width);
    }
    for (; written < count; ++written) hid_write_list_item(output + width * (uintptr_t)written, 0, width);
}

static inline void hid_diff(const uint8_t* previous, const uint8_t* current,
                            uint8_t* released, uint8_t* pressed, uint32_t count, unsigned width) {
    hid_diff_direction(previous, current, released, count, width);
    hid_diff_direction(current, previous, pressed, count, width);
}

static inline uint32_t hid_hello(uint8_t* buffer, uint32_t capacity) {
    static const uint8_t text[] = {'H', 'e', 'l', 'l', 'o', '\n', 0};
    if (!buffer && capacity) return 0;
    const uint32_t size = capacity < sizeof(text) ? capacity : sizeof(text);
    for (uint32_t i = 0; i < size; ++i) buffer[i] = text[i];
    return sizeof(text);
}
