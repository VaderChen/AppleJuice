#pragma once

#define CRT_MBC_INFO_SIZE 544
#define CRT_MBC_TYPES_OFFSET 28
#define CRT_MBC_TYPES_SIZE 257
#define CRT_MBC_CASE_OFFSET (CRT_MBC_TYPES_OFFSET + CRT_MBC_TYPES_SIZE)
#define CRT_MBC_CASE_SIZE 256
#define CRT_MBC_PROCESS_INFO 0xa00
#define CRT_MBC_PROCESS_TYPES (CRT_MBC_PROCESS_INFO + CRT_MBC_INFO_SIZE)
#define CRT_MBC_PROCESS_CASE (CRT_MBC_PROCESS_TYPES + CRT_MBC_TYPES_SIZE)

/* Win32 的 TLS 擴充槽使用 0x2000–0x3000，Win64 使用 0x2000–0x4000。 */
#define CRT_MBC_TEB32_VIEW 0x3800
#define CRT_MBC_TEB32_CTYPE 0x3820
#define CRT_MBC_TEB64_VIEW 0x4500

#define CRT_MBC_KANA_TEXT 1
#define CRT_MBC_KANA_PUNCT 2
#define CRT_MBC_LEAD 4
#define CRT_MBC_TRAIL 8
#define CRT_MBC_SINGLE_UPPER 16
#define CRT_MBC_SINGLE_LOWER 32

static inline int crt_mbc_test_byte(unsigned value, unsigned ctype, unsigned type,
                                    unsigned ctype_mask, unsigned type_mask) {
    if (value > 255) return 0;
    if (ctype_mask == 0x40 && value == '\t') return 1;
    return (ctype & ctype_mask) != 0 || (type & type_mask) != 0;
}
