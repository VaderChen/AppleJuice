/*
 * 共用 CRT 客體常式，以 Windows x64 ABI 編譯並保存 .pdata／.xdata。
 * 所有 callback 都是普通客體呼叫；慢路徑與既有配置器在載入時填入，
 * 映射後整個模組唯讀。沒有宿主指標、隱藏排序狀態或依測項選路。
 */
#include "guest_crt_state.h"
typedef __SIZE_TYPE__ Size;
typedef __UINTPTR_TYPE__ Address;
typedef unsigned char Byte;
typedef unsigned short Wide;
typedef unsigned long long Word;
typedef int (*Compare)(const void *, const void *);
#define EXPORT __declspec(dllexport)
#define INLINE static __attribute__((always_inline)) inline

EXPORT void (*volatile host_qsort)(void *, Size, Size, Compare);
EXPORT void *(*volatile host_bsearch)(const void *, const void *, Size, Size, Compare);
EXPORT void *(*volatile host_calloc)(Size, Size);
EXPORT void *(*volatile host_realloc)(void *, Size);
EXPORT Size (*volatile host__msize)(void *);
EXPORT void *(*volatile call_malloc)(Size);
EXPORT void (*volatile call_free)(void *);
EXPORT const Word *volatile heap_state;

static Word load_word(const void *at) {
    Word value;
    __builtin_memcpy(&value, at, sizeof value);
    return value;
}
static void store_word(void *at, Word value) {
    __builtin_memcpy(at, &value, sizeof value);
}
/* 共用交換迴圈，避免分割／堆排序的各呼叫點各自展開整份向量與尾端處理。 */
__attribute__((noinline))
static void exchange(Byte *restrict left, Byte *restrict right, Size width) {
    if (left == right) return;
    while (width >= sizeof(Word)) {
        Word a = load_word(left), b = load_word(right);
        store_word(left, b); store_word(right, a);
        left += sizeof(Word); right += sizeof(Word); width -= sizeof(Word);
    }
    while (width--) { Byte value = *left; *left++ = *right; *right++ = value; }
}
static void sift(Byte *base, Size root, Size count, Size width, Compare compare) {
    while (root < count / 2) {
        Size child = root * 2 + 1;
        if (child + 1 < count && compare(base + child * width, base + (child + 1) * width) < 0)
            ++child;
        if (compare(base + root * width, base + child * width) >= 0) break;
        exchange(base + root * width, base + child * width, width);
        root = child;
    }
}
static void heap_sort(Byte *base, Size count, Size width, Compare compare) {
    for (Size root = count / 2; root;) sift(base, --root, count, width, compare);
    while (count > 1) {
        exchange(base, base + --count * width, width);
        sift(base, 0, count, width, compare);
    }
}
static void sort_range(Byte *base, Size count, Size width, Compare compare, unsigned depth) {
    while (count > 12) {
        if (!depth) { heap_sort(base, count, width, compare); return; }
        --depth;
        Byte *middle = base + (count / 2) * width, *pivot = base + (count - 1) * width;
        if (compare(base, middle) > 0) exchange(base, middle, width);
        if (compare(middle, pivot) > 0) exchange(middle, pivot, width);
        if (compare(base, middle) > 0) exchange(base, middle, width);
        exchange(middle, pivot, width);
        /* 三路分割避免大量相等鍵退化；pivot 始終指向原陣列元素。 */
        Size low = 0, at = 0, high = count - 1;
        while (at < high) {
            int order = compare(base + at * width, pivot);
            if (order < 0) { exchange(base + at * width, base + low * width, width); ++low; ++at; }
            else if (order > 0) exchange(base + at * width, base + --high * width, width);
            else ++at;
        }
        exchange(base + high * width, pivot, width);
        Size right = count - high - 1;
        /* 只遞迴較小半邊，另一半以迴圈接續；深度與工作堆疊都有上界。 */
        if (low < right) {
            if (low > 1) sort_range(base, low, width, compare, depth);
            base += (high + 1) * width; count = right;
        } else {
            if (right > 1) sort_range(base + (high + 1) * width, right, width, compare, depth);
            count = low;
        }
    }
    for (Size at = 1; at < count; ++at)
        for (Size next = at; next && compare(base + (next - 1) * width, base + next * width) > 0; --next)
            exchange(base + (next - 1) * width, base + next * width, width);
}
EXPORT void guest_qsort(void *base, Size count, Size width, Compare compare) {
    if (count < 2 || !width) return;
    /* 沿用服務層的範圍契約；異常參數交回原服務，不繞過原有錯誤流程。 */
    if (!base || !compare || count > 65536 || width > (16U << 20) || count * width > (16U << 20) ||
        (Address)base > ~(Address)0 - count * width) {
        host_qsort(base, count, width, compare); return;
    }
    unsigned depth = 0;
    for (Size remaining = count; remaining > 1; remaining >>= 1) depth += 2;
    sort_range(base, count, width, compare, depth);
}
EXPORT void *guest_bsearch(const void *key, const void *base, Size count, Size width, Compare compare) {
    if (!count || !width) return (void *)0;
    if (!key || !base || !compare || count > (16U << 20) || width > (16U << 20) || count * width > (16U << 20) ||
        (Address)base > ~(Address)0 - count * width)
        return host_bsearch(key, base, count, width, compare);
    Size left = 0, right = count;
    while (left < right) {
        Size middle = left + (right - left) / 2;
        const Byte *element = (const Byte *)base + middle * width;
        int order = compare(key, element);
        if (!order) return (void *)element;
        if (order < 0) right = middle; else left = middle + 1;
    }
    return (void *)0;
}

/* 與 guest_heap_block_size_impl 使用同一個配置區格式；不建立第二份配置狀態。 */
static Size arena_capacity(void *pointer) {
    const Word *state = heap_state;
    Address address = (Address)pointer;
    if (!state || address - state[1] >= state[2]) return 0;
    Address chunk = address & ~(Address)0x3ffff;
    if (load_word((void *)chunk) != 0x50414548343658abULL) return 0;
    Word cls = load_word((void *)(chunk + 8)), block = load_word((void *)(chunk + 16));
    if (!cls || cls > 497 || block != (cls <= 256 ? cls * 16 : 4096 + (cls - 256) * 256) ||
        address < chunk + 64 || address > chunk + 0x40000 - block ||
        (unsigned)(address - chunk - 64) % (unsigned)block) return 0;
    return (Size)block;
}
EXPORT Size guest__msize(void *pointer) {
    Size capacity = arena_capacity(pointer);
    return capacity ? capacity : host__msize(pointer);
}
EXPORT void *guest_calloc(Size count, Size width) {
    /* 連同 Windows CRT 預留的 31 bytes 檢查；溢位時保留 errno／new-handler 語意。 */
    Size size;
    if (__builtin_mul_overflow(count, width, &size) || size > ~(Size)0 - 31)
        return host_calloc(count, width);
    Byte *result = call_malloc(size);
    if (!result) return (void *)0;
    Byte *at = result;
    if (!size) size = 1;
    while (size >= sizeof(Word)) { store_word(at, 0); at += sizeof(Word); size -= sizeof(Word); }
    while (size--) *at++ = 0;
    return result;
}
EXPORT void *guest_realloc(void *pointer, Size size) {
    /* 一般 heap、NULL 與錯誤指標仍使用原有契約；配置區才走共用客體配置器。 */
    Size previous = arena_capacity(pointer);
    if (!previous || size > ~(Size)0 - 31) return host_realloc(pointer, size);
    if (!size) { call_free(pointer); return (void *)0; }
    if (size <= previous) return pointer;
    Byte *result = call_malloc(size);
    if (!result) return (void *)0;
    Byte *to = result; const Byte *from = pointer;
    while (previous >= sizeof(Word)) {
        store_word(to, load_word(from));
        to += sizeof(Word); from += sizeof(Word); previous -= sizeof(Word);
    }
    while (previous--) *to++ = *from++;
    call_free(pointer);
    return result;
}

/* 掃描家族共用 256-bit 字元集合；不經 locale、宿主字串複製或服務邊界。 */
static void character_set(Word bits[4], const Byte *set) {
    bits[0] = bits[1] = bits[2] = bits[3] = 0;
    for (; *set; ++set) bits[*set >> 6] |= (Word)1 << (*set & 63);
}
EXPORT Size guest_strspn(const char *text, const char *accept) {
    Word bits[4]; character_set(bits, (const Byte *)accept);
    const Byte *at = (const Byte *)text;
    while (*at && ((bits[*at >> 6] >> (*at & 63)) & 1)) ++at;
    return (Size)(at - (const Byte *)text);
}
EXPORT Size guest_strcspn(const char *text, const char *reject) {
    Word bits[4]; character_set(bits, (const Byte *)reject);
    const Byte *at = (const Byte *)text;
    while (*at && !((bits[*at >> 6] >> (*at & 63)) & 1)) ++at;
    return (Size)(at - (const Byte *)text);
}
EXPORT char *guest_strpbrk(const char *text, const char *accept) {
    Word bits[4]; character_set(bits, (const Byte *)accept);
    for (const Byte *at = (const Byte *)text; *at; ++at)
        if ((bits[*at >> 6] >> (*at & 63)) & 1) return (char *)at;
    return (char *)0;
}
EXPORT void *guest_memchr(const void *memory, int character, Size size) {
    const Byte *at = memory;
    for (; size; --size, ++at) if (*at == (Byte)character) return (void *)at;
    return (void *)0;
}
EXPORT char *guest_strrchr(const char *text, int character) {
    char *found = (char *)0;
    for (;;) {
        Byte value = *(const Byte *)text;
        if (value == (Byte)character) found = (char *)text;
        if (!value) return found;
        ++text;
    }
}
EXPORT Size guest_wcslen(const Wide *text) {
    const Wide *at = text;
    while (*at) ++at;
    return (Size)(at - text);
}
EXPORT Size guest_wcsnlen(const Wide *text, Size limit) {
    Size count = 0;
    while (count < limit && text[count]) ++count;
    return count;
}
EXPORT Size guest_strnlen(const char *text, Size limit) {
    Size count = 0;
    while (count < limit && text[count]) ++count;
    return count;
}
EXPORT int guest_wcscmp(const Wide *left, const Wide *right) {
    while (*left == *right) { if (!*left) return 0; ++left; ++right; }
    return *left < *right ? -1 : 1;
}
EXPORT int guest_wcsncmp(const Wide *left, const Wide *right, Size count) {
    for (; count; --count, ++left, ++right) {
        if (*left != *right) return *left < *right ? -1 : 1;
        if (!*left) break;
    }
    return 0;
}
EXPORT Wide *guest_wcschr(const Wide *text, Wide character) {
    for (;;) {
        if (*text == character) return (Wide *)text;
        if (!*text++) return (Wide *)0;
    }
}
EXPORT Wide *guest_wcsrchr(const Wide *text, Wide character) {
    Wide *found = (Wide *)0;
    for (;;) {
        if (*text == character) found = (Wide *)text;
        if (!*text++) return found;
    }
}

/* 字元、errno 與整數轉換共用每個執行緒的客體狀態，不快取宿主的 current。 */
INLINE const GuestCrtThreadState *thread_state(void) {
    Address teb;
    __asm__("movq %%gs:0x30, %0" : "=r"(teb));
    return (const GuestCrtThreadState *)(teb + GUEST_CRT_STATE_OFFSET);
}
/* 與 crt_c_locale.hpp 相同的分類位元；常數表避免每次呼叫重算全部分類。 */
#define C_UPPER(c) ((c) >= 'A' && (c) <= 'Z')
#define C_LOWER(c) ((c) >= 'a' && (c) <= 'z')
#define C_DIGIT(c) ((c) >= '0' && (c) <= '9')
#define C_MASK(c) ((C_UPPER(c) ? 1 : C_LOWER(c) ? 2 : C_DIGIT(c) ? 4 : 0) | \
    (((c) == ' ' || ((c) >= 9 && (c) <= 13)) ? 8 : 0) | \
    (((c) > 32 && (c) < 127 && !C_UPPER(c) && !C_LOWER(c) && !C_DIGIT(c)) ? 16 : 0) | \
    (((c) < 32 || (c) == 127) ? 32 : 0) | ((c) == ' ' ? 64 : 0) | \
    ((C_DIGIT(c) || ((c) >= 'a' && (c) <= 'f') || ((c) >= 'A' && (c) <= 'F')) ? 128 : 0))
#define C_ROW(c) C_MASK(c),C_MASK(c+1),C_MASK(c+2),C_MASK(c+3), \
    C_MASK(c+4),C_MASK(c+5),C_MASK(c+6),C_MASK(c+7), \
    C_MASK(c+8),C_MASK(c+9),C_MASK(c+10),C_MASK(c+11), \
    C_MASK(c+12),C_MASK(c+13),C_MASK(c+14),C_MASK(c+15)
static const Wide c_masks[128] = {
    C_ROW(0),C_ROW(16),C_ROW(32),C_ROW(48),C_ROW(64),C_ROW(80),C_ROW(96),C_ROW(112)
};
#undef C_ROW
#undef C_MASK
#undef C_UPPER
#undef C_LOWER
#undef C_DIGIT

INLINE int character_mask(unsigned c, unsigned wide, Address locale, unsigned *mask) {
    const GuestCrtThreadState *state = thread_state();
    if (locale && locale != state->locale_address) return 0;
    if (!state->ctype_codepage) { *mask = c < 128 ? c_masks[c] : 0; return 1; }
    if (wide) return 0; /* 非 C 寬字元仍由既有 Unicode／ICU 契約處理。 */
    *mask = c < 256 ? ((const Wide *)(Address)state->ctype_address)[c] : 0;
    return 1;
}
INLINE int predicate(unsigned c, unsigned requested, unsigned wide, Address locale, unsigned *result) {
    unsigned mask;
    if (!character_mask(c, wide, locale, &mask)) return 0;
    if (!wide && (requested == 4 || requested == 128)) mask = c < 128 ? c_masks[c] : 0;
    if (requested == 64 && c == '\t') mask |= 64;
    *result = (mask & requested) != 0;
    return 1;
}
#define PREDICATE(name,mask,wide) \
    EXPORT unsigned (*volatile host_##name)(unsigned); \
    EXPORT unsigned guest_text_##name(unsigned input) { \
        unsigned result, c = wide ? (Wide)input : input; \
        return predicate(c,mask,wide,0,&result) ? result : host_##name(input); \
    }
#define PREDICATE_L(name,mask,wide) \
    EXPORT unsigned (*volatile host_##name)(unsigned,Address); \
    EXPORT unsigned guest_text_##name(unsigned input,Address locale) { \
        unsigned result, c = wide ? (Wide)input : input; \
        return predicate(c,mask,wide,locale,&result) ? result : host_##name(input,locale); \
    }
#define PREDICATE_FAMILY(name,mask) \
    PREDICATE(is##name,mask,0) PREDICATE_L(_is##name##_l,mask,0) \
    PREDICATE(isw##name,mask,1) PREDICATE_L(_isw##name##_l,mask,1)
PREDICATE_FAMILY(alnum,0x107)
PREDICATE_FAMILY(alpha,0x103)
PREDICATE_FAMILY(blank,0x40)
PREDICATE_FAMILY(cntrl,0x20)
PREDICATE_FAMILY(digit,4)
PREDICATE_FAMILY(graph,0x117)
PREDICATE_FAMILY(lower,2)
PREDICATE_FAMILY(print,0x157)
PREDICATE_FAMILY(punct,0x10)
PREDICATE_FAMILY(space,8)
PREDICATE_FAMILY(upper,1)
PREDICATE_FAMILY(xdigit,0x80)
#undef PREDICATE_FAMILY
#undef PREDICATE_L
#undef PREDICATE

#define MASK_ENTRY(name,wide) \
    EXPORT unsigned (*volatile host_##name)(unsigned,unsigned); \
    EXPORT unsigned guest_text_##name(unsigned input,unsigned requested) { \
        unsigned mask, c = wide ? (Wide)input : input; \
        return character_mask(c,wide,0,&mask) ? mask & (Wide)requested : host_##name(input,requested); \
    }
#define MASK_ENTRY_L(name,wide) \
    EXPORT unsigned (*volatile host_##name)(unsigned,unsigned,Address); \
    EXPORT unsigned guest_text_##name(unsigned input,unsigned requested,Address locale) { \
        unsigned mask, c = wide ? (Wide)input : input; \
        return character_mask(c,wide,locale,&mask) ? mask & (Wide)requested : host_##name(input,requested,locale); \
    }
MASK_ENTRY(_isctype,0)
MASK_ENTRY(iswctype,1)
MASK_ENTRY_L(_isctype_l,0)
MASK_ENTRY_L(_iswctype_l,1)
#undef MASK_ENTRY
#undef MASK_ENTRY_L

INLINE int map_character(unsigned c, unsigned wide, unsigned upper, Address locale, unsigned *result) {
    const GuestCrtThreadState *state = thread_state();
    if (locale && locale != state->locale_address) return 0;
    if (!state->ctype_codepage) {
        *result = upper && c >= 'a' && c <= 'z' ? c - 32 :
                  !upper && c >= 'A' && c <= 'Z' ? c + 32 : c;
        return 1;
    }
    if (wide) return 0;
    *result = c < 256 ? ((const Byte *)(Address)(upper ? state->upper_address : state->lower_address))[c] : c;
    return 1;
}
#define MAP_ENTRY(name,wide,upper) \
    EXPORT unsigned (*volatile host_##name)(unsigned); \
    EXPORT unsigned guest_text_##name(unsigned input) { \
        unsigned result, c = wide ? (Wide)input : input; \
        return map_character(c,wide,upper,0,&result) ? result : host_##name(input); \
    }
#define MAP_ENTRY_L(name,wide,upper) \
    EXPORT unsigned (*volatile host_##name)(unsigned,Address); \
    EXPORT unsigned guest_text_##name(unsigned input,Address locale) { \
        unsigned result, c = wide ? (Wide)input : input; \
        return map_character(c,wide,upper,locale,&result) ? result : host_##name(input,locale); \
    }
MAP_ENTRY(tolower,0,0) MAP_ENTRY(toupper,0,1)
MAP_ENTRY(towlower,1,0) MAP_ENTRY(towupper,1,1)
MAP_ENTRY_L(_tolower_l,0,0) MAP_ENTRY_L(_toupper_l,0,1)
MAP_ENTRY_L(_towlower_l,1,0) MAP_ENTRY_L(_towupper_l,1,1)
#undef MAP_ENTRY
#undef MAP_ENTRY_L

EXPORT int *guest_text__errno(void) { return (int *)(Address)thread_state()->errno_address; }
EXPORT int (*volatile host__get_errno)(int *);
EXPORT int guest_text__get_errno(int *destination) {
    if (!destination) return host__get_errno(destination);
    *destination = *guest_text__errno(); return 0;
}
EXPORT int guest_text__set_errno(int value) { *guest_text__errno() = value; return 0; }

INLINE unsigned digit_value(unsigned c) {
    if (c - '0' < 10) return c - '0';
    c |= 32;
    return c - 'a' < 26 ? c - 'a' + 10 : 255;
}
INLINE int integer_fast(const void *source, int radix) {
    const GuestCrtThreadState *state = thread_state();
    return source && (radix == 0 || (radix >= 2 && radix <= 36)) &&
           !state->ctype_codepage && !state->numeric_codepage;
}
/* 32／64-bit 上界在解析前決定；溢位後繼續掃描，endptr 仍指向最後有效數字之後。 */
#define PARSER(name,Unit) \
static __attribute__((noinline)) Word name(const Unit *source,Unit **endptr,unsigned radix, \
                                          unsigned bits,unsigned is_signed) { \
    const Unit *at = source; \
    while (*at == ' ' || (unsigned)(*at - 9) < 5) ++at; \
    unsigned negative = *at == '-'; \
    if (*at == '+' || negative) ++at; \
    if ((radix == 0 || radix == 16) && at[0] == '0' && (at[1] | 32) == 'x' && \
        digit_value(at[2]) < 16) { at += 2; radix = 16; } \
    else if (!radix) radix = *at == '0' ? 8 : 10; \
    const Unit *first = at; \
    Word limit = bits == 32 ? 0xffffffffULL : ~0ULL; \
    if (is_signed) limit = (limit >> 1) + negative; \
    Word cutoff = limit / radix, remainder = limit % radix, value = 0; \
    unsigned overflow = 0; \
    for (;;) { \
        unsigned digit = digit_value(*at); \
        if (digit >= radix) break; \
        if (value > cutoff || (value == cutoff && digit > remainder)) overflow = 1; \
        if (!overflow) value = value * radix + digit; \
        ++at; \
    } \
    if (endptr) *endptr = (Unit *)(at == first ? source : at); \
    if (overflow) { *guest_text__errno() = 34; value = limit; } \
    if (negative && (!overflow || is_signed)) value = 0 - value; \
    return value; \
}
PARSER(parse_narrow,Byte)
PARSER(parse_wide,Wide)
#undef PARSER
#define INTEGER_ENTRY(name,Unit,Result,bits,is_signed,parser) \
    EXPORT Result (*volatile host_##name)(const Unit *,Unit **,int); \
    EXPORT Result guest_text_##name(const Unit *source,Unit **endptr,int radix) { \
        return integer_fast(source,radix) ? (Result)parser(source,endptr,(unsigned)radix,bits,is_signed) : \
               host_##name(source,endptr,radix); \
    }
INTEGER_ENTRY(strtol,Byte,unsigned,32,1,parse_narrow)
INTEGER_ENTRY(strtoul,Byte,unsigned,32,0,parse_narrow)
INTEGER_ENTRY(strtoll,Byte,Word,64,1,parse_narrow)
INTEGER_ENTRY(strtoull,Byte,Word,64,0,parse_narrow)
INTEGER_ENTRY(_strtoi64,Byte,Word,64,1,parse_narrow)
INTEGER_ENTRY(_strtoui64,Byte,Word,64,0,parse_narrow)
INTEGER_ENTRY(wcstol,Wide,unsigned,32,1,parse_wide)
INTEGER_ENTRY(wcstoul,Wide,unsigned,32,0,parse_wide)
INTEGER_ENTRY(wcstoll,Wide,Word,64,1,parse_wide)
INTEGER_ENTRY(wcstoull,Wide,Word,64,0,parse_wide)
INTEGER_ENTRY(_wcstoi64,Wide,Word,64,1,parse_wide)
INTEGER_ENTRY(_wcstoui64,Wide,Word,64,0,parse_wide)
#undef INTEGER_ENTRY
#define WIDE_INTEGER(name,Result,bits) \
    EXPORT Result (*volatile host_##name)(const Wide *); \
    EXPORT Result guest_text_##name(const Wide *source) { \
        return integer_fast(source,10) ? (Result)parse_wide(source,(Wide **)0,10,bits,1) : host_##name(source); \
    }
WIDE_INTEGER(_wtoi,unsigned,32)
WIDE_INTEGER(_wtol,unsigned,32)
WIDE_INTEGER(_wtoi64,Word,64)
#undef WIDE_INTEGER

/* 整數格式化共用除法與反轉流程；錯誤交回原服務以保留清空、errno 與 handler。 */
static __attribute__((noinline)) int format_integer(Word value, unsigned bits, unsigned is_signed,
    void *buffer, Size capacity, unsigned radix, unsigned wide) {
    if (!buffer || !capacity || radix < 2 || radix > 36) return 0;
    Word mask = bits == 32 ? 0xffffffffULL : ~0ULL;
    value &= mask;
    unsigned negative = is_signed && radix == 10 && (value >> (bits - 1));
    if (negative) value = (0 - value) & mask;
    Byte scratch[65]; unsigned count = 0;
    do {
        Word quotient = value / radix;
        unsigned digit = (unsigned)(value - quotient * radix);
        scratch[count++] = (Byte)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value = quotient;
    } while (value);
    if (negative) scratch[count++] = '-';
    if (count >= capacity) return 0;
    if (wide) {
        Wide *out = (Wide *)buffer;
        for (unsigned n = 0; n < count; ++n) out[n] = scratch[count - n - 1];
        out[count] = 0;
    } else {
        Byte *out = (Byte *)buffer;
        for (unsigned n = 0; n < count; ++n) out[n] = scratch[count - n - 1];
        out[count] = 0;
    }
    return 1;
}
#define FORMAT_ENTRY(name,Unit,Input,bits,is_signed,wide) \
    EXPORT Unit *(*volatile host_##name)(Input,Unit *,int); \
    EXPORT int (*volatile host_##name##_s)(Input,Unit *,Size,int); \
    EXPORT Unit *guest_text_##name(Input value,Unit *buffer,int radix) { \
        return format_integer(value,bits,is_signed,buffer,65,(unsigned)radix,wide) ? buffer : \
               host_##name(value,buffer,radix); \
    } \
    EXPORT int guest_text_##name##_s(Input value,Unit *buffer,Size capacity,int radix) { \
        return format_integer(value,bits,is_signed,buffer,capacity,(unsigned)radix,wide) ? 0 : \
               host_##name##_s(value,buffer,capacity,radix); \
    }
FORMAT_ENTRY(_itoa,Byte,unsigned,32,1,0)
FORMAT_ENTRY(_ltoa,Byte,unsigned,32,1,0)
FORMAT_ENTRY(_ultoa,Byte,unsigned,32,0,0)
FORMAT_ENTRY(_i64toa,Byte,Word,64,1,0)
FORMAT_ENTRY(_ui64toa,Byte,Word,64,0,0)
FORMAT_ENTRY(_itow,Wide,unsigned,32,1,1)
FORMAT_ENTRY(_ltow,Wide,unsigned,32,1,1)
FORMAT_ENTRY(_ultow,Wide,unsigned,32,0,1)
FORMAT_ENTRY(_i64tow,Wide,Word,64,1,1)
FORMAT_ENTRY(_ui64tow,Wide,Word,64,0,1)
#undef FORMAT_ENTRY
