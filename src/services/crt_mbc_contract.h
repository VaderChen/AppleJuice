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

/* 宿主透過 GuestMemory 讀取，常駐客體直接讀取。兩條路徑共用相同的字元邊界。 */
typedef __UINTPTR_TYPE__ CrtMbcAddress;
typedef unsigned (*CrtMbcRead)(const void*, CrtMbcAddress);
static inline CrtMbcAddress crt_mbc_character_bytes(const void* memory, CrtMbcRead read_byte,
        const unsigned char* types, CrtMbcAddress text, CrtMbcAddress count) {
    CrtMbcAddress offset=0;
    while (count--) {
        unsigned first=read_byte(memory,text+offset);
        if (!first) break;
        if (types && (types[first]&CRT_MBC_LEAD)) {
            if (!read_byte(memory,text+offset+1)) break;
            ++offset;
        }
        ++offset;
    }
    return offset;
}

static inline CrtMbcAddress crt_mbc_find_string(const void* memory, CrtMbcRead read_byte,
        const unsigned char* types, CrtMbcAddress text, CrtMbcAddress pattern) {
    const unsigned initial=read_byte(memory,pattern);
    if (!initial) return text;
    for (;;) {
        unsigned first=read_byte(memory,text);
        if (!first) return 0;
        if (first==initial) {
            CrtMbcAddress i=1;
            unsigned needle;
            while ((needle=read_byte(memory,pattern+i)) && needle==read_byte(memory,text+i)) ++i;
            if (!needle) return text;
        }
        ++text;
        if (types && (types[first]&CRT_MBC_LEAD) && read_byte(memory,text)) ++text;
    }
}

static inline unsigned crt_mbc_compare_unit(const void* memory, CrtMbcRead read_byte,
        const unsigned char* types, CrtMbcAddress at, CrtMbcAddress available) {
    const unsigned first=read_byte(memory,at);
    if (!types || !(types[first]&CRT_MBC_LEAD) || !first) return first;
    if (available<2) return 0;
    const unsigned trail=read_byte(memory,at+1);
    return trail ? (first<<8)|trail : 0;
}

/* 字元相同時兩邊的編碼寬度也相同。位元組上限內的孤立前導位元組等同字串結尾。 */
static inline int crt_mbc_compare(const void* memory, CrtMbcRead read_byte,
        const unsigned char* types, CrtMbcAddress lhs, CrtMbcAddress rhs,
        CrtMbcAddress remaining, int byte_limit) {
    while (remaining) {
        const CrtMbcAddress available=byte_limit ? remaining : 2;
        const unsigned left=crt_mbc_compare_unit(memory,read_byte,types,lhs,available);
        const unsigned right=crt_mbc_compare_unit(memory,read_byte,types,rhs,available);
        if (left!=right) return left<right ? -1 : 1;
        if (!left) break;
        const unsigned width=left>255 ? 2 : 1;
        remaining-=byte_limit ? width : 1;
        lhs+=width;
        rhs+=width;
    }
    return 0;
}
