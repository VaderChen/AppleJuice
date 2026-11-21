#include "hid_array_operations.h"

#if defined(__i386__)
#define HID_CALL __attribute__((stdcall))
#else
#define HID_CALL
#endif
#define HID_EXPORT __declspec(dllexport) uint32_t HID_CALL

HID_EXPORT HidD_Hello(uint8_t* buffer, uint32_t capacity) {
    const uint32_t size = capacity < 7 ? capacity : 7;
    const uintptr_t address = (uintptr_t)buffer;
    if (size && size - 1 > UINTPTR_MAX - address) return 0;
    return hid_hello(buffer, capacity);
}

static uint32_t usage_list_difference(const uint8_t* previous, const uint8_t* current,
                                      uint8_t* released, uint8_t* pressed, uint32_t count, unsigned width) {
    const uint64_t addresses[] = {(uintptr_t)previous, (uintptr_t)current,
                                  (uintptr_t)released, (uintptr_t)pressed};
    const uint32_t status = hid_diff_validate(addresses, count, width, UINTPTR_MAX);
    if (status == HID_ARRAY_SUCCESS && count) hid_diff(previous, current, released, pressed, count, width);
    return status;
}

HID_EXPORT HidP_UsageListDifference(const uint8_t* previous, const uint8_t* current,
                                    uint8_t* released, uint8_t* pressed, uint32_t count) {
    return usage_list_difference(previous, current, released, pressed, count, 2);
}

HID_EXPORT HidP_UsageAndPageListDifference(const uint8_t* previous, const uint8_t* current,
                                           uint8_t* released, uint8_t* pressed, uint32_t count) {
    return usage_list_difference(previous, current, released, pressed, count, 4);
}
