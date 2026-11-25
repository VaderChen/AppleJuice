#include "crt_uchar.h"
#include "crt_secure_fill.h"
#include "crt_secure_append.h"

typedef __UINTPTR_TYPE__ Address;
typedef unsigned char Byte;
typedef unsigned short Wide;
typedef struct {
    const Byte* mbc;
    const Wide* ctype;
    CrtUcharState* uchar_states;
} CrtMbcView;

static inline const CrtMbcView* current_view(void){
    Address teb;
#ifdef _WIN64
    __asm__("movq %%gs:0x30, %0" : "=r"(teb));
    return (const CrtMbcView*)(teb + CRT_MBC_TEB64_VIEW);
#else
    __asm__("movl %%fs:0x18, %0" : "=r"(teb));
    return (const CrtMbcView*)(teb + CRT_MBC_TEB32_VIEW);
#endif
}

#define CRT_MBC_BYTE(name, ctype_mask, type_mask) \
    __declspec(dllexport) int name(unsigned value) { \
        if (value > 255) return 0; \
        const CrtMbcView* view = current_view(); \
        return crt_mbc_test_byte(value, ctype_mask ? view->ctype[value] : 0, \
            type_mask ? view->mbc[CRT_MBC_TYPES_OFFSET + 1 + value] : 0, ctype_mask, type_mask); \
    }
#include "crt_mbc_byte_operations.inc"
#undef CRT_MBC_BYTE

__declspec(dllexport) unsigned char* __p__mbctype(void){
    return (unsigned char*)current_view()->mbc + CRT_MBC_TYPES_OFFSET;
}

__declspec(dllexport) unsigned char* __p__mbcasemap(void){
    return (unsigned char*)current_view()->mbc + CRT_MBC_CASE_OFFSET;
}

/* 非驗證型 MBCS 計數沿用 _setmbcp 發布的每執行緒前導位元組表。 */
__declspec(dllexport) Address _mbsnlen(const Byte* text, Address limit){
    if(!limit)return 0;
    const Byte* mbc=current_view()->mbc;
    const Byte* types=mbc+CRT_MBC_TYPES_OFFSET+1;
    Address offset=0,count=0;
    while(offset<limit){
        const unsigned first=text[offset++];
        if(!first)return count;
        if(types[first]&CRT_MBC_LEAD){
            if(offset==limit)return limit;
            if(!text[offset++])return count;
        }
        ++count;
    }
    return limit;
}

#ifndef _WIN64
/* Win64 的相同常式已由 guest_crt_algorithms 提供。 */
__declspec(dllexport) Address strnlen(const char* text,Address limit){
    Address n=0;while(n<limit && text[n])++n;
    return n;
}
__declspec(dllexport) Address wcsnlen(const Wide* text,Address limit){
    Address n=0;while(n<limit && text[n])++n;
    return n;
}
#endif

typedef struct {
    const void* locinfo;
    const Byte* mbc;
} CrtGuestLocale;

static inline const Byte* lead_table(const CrtGuestLocale* locale) {
    const Byte* mbc=locale ? locale->mbc : current_view()->mbc;
    return *(const unsigned*)(mbc+8) ? mbc+CRT_MBC_TYPES_OFFSET+1 : 0;
}

static unsigned read_byte(const void* unused, Address at) {
    (void)unused;
    return *(const Byte*)at;
}

__declspec(dllimport) int __cdecl _set_errno(int);
__declspec(dllimport) void __cdecl _invalid_parameter_noinfo(void);

__declspec(dllexport) Byte* _mbsninc_l(const Byte* text, Address count, const CrtGuestLocale* locale) {
    if (!text || !count) return (Byte*)text;
    const Address bytes=crt_mbc_character_bytes(0,read_byte,lead_table(locale),(Address)text,count);
    return (Byte*)text+(unsigned)bytes;
}

__declspec(dllexport) Byte* _mbsninc(const Byte* text, Address count) {
    return _mbsninc_l(text,count,0);
}

__declspec(dllexport) Byte* _mbsstr_l(const Byte* text, const Byte* pattern, const CrtGuestLocale* locale) {
    if (pattern && !*pattern) return (Byte*)text;
    if (!text || !pattern) {
        _set_errno(22);
        _invalid_parameter_noinfo();
        return 0;
    }
    return (Byte*)crt_mbc_find_string(0,read_byte,lead_table(locale),(Address)text,(Address)pattern);
}

__declspec(dllexport) Byte* _mbsstr(const Byte* text, const Byte* pattern) {
    return _mbsstr_l(text,pattern,0);
}

static int compare_strings(const Byte* lhs,const Byte* rhs,Address count,int bytes,const CrtGuestLocale* locale) {
    if (!count) return 0;
    if (!lhs || !rhs) {
        _set_errno(22);
        _invalid_parameter_noinfo();
        return 0x7fffffff;
    }
    return crt_mbc_compare(0,read_byte,lead_table(locale),(Address)lhs,(Address)rhs,count,bytes);
}

__declspec(dllexport) int _mbscmp_l(const Byte* lhs,const Byte* rhs,const CrtGuestLocale* locale) {
    return compare_strings(lhs,rhs,(Address)-1,0,locale);
}
__declspec(dllexport) int _mbscmp(const Byte* lhs,const Byte* rhs) {
    return _mbscmp_l(lhs,rhs,0);
}
__declspec(dllexport) int _mbsncmp_l(const Byte* lhs,const Byte* rhs,Address count,const CrtGuestLocale* locale) {
    return compare_strings(lhs,rhs,count,0,locale);
}
__declspec(dllexport) int _mbsncmp(const Byte* lhs,const Byte* rhs,Address count) {
    return _mbsncmp_l(lhs,rhs,count,0);
}
__declspec(dllexport) int _mbsnbcmp_l(const Byte* lhs,const Byte* rhs,Address count,const CrtGuestLocale* locale) {
    return compare_strings(lhs,rhs,count,1,locale);
}
__declspec(dllexport) int _mbsnbcmp(const Byte* lhs,const Byte* rhs,Address count) {
    return _mbsnbcmp_l(lhs,rhs,count,0);
}

static Address decode_uchar(void* destination, const Byte* source, Address count, CrtUcharState* state,
                            enum CrtUcharFunction function) {
    if (!state) state = &current_view()->uchar_states[function];
    CrtUcharState next = *state;
    unsigned value = 0;
    const int size = crt_uchar_decode(&next, function == CrtMbrtoc16, 0, read_byte, (Address)source, count, &value);
    if (source && destination && (size >= 0 || size == CrtUcharPending)) {
        if (function == CrtMbrtoc16)
            *(Wide*)destination = (Wide)value;
        else
            *(unsigned*)destination = value;
    }
    *state = next;
    if (size == CrtUcharInvalid) _set_errno(42);
    return (Address)size;
}
static Address encode_uchar(Byte* destination, unsigned value, CrtUcharState* state,
                            enum CrtUcharFunction function) {
    if (!state) state = &current_view()->uchar_states[function];
    CrtUcharState next = *state;
    Byte bytes[4];
    const int size = crt_uchar_encode(&next, function == CrtC16rtomb, destination ? value : 0, bytes);
    if (destination)
        for (int i = 0; i < size; ++i)
            destination[i] = bytes[i];
    *state = next;
    if (size == CrtUcharInvalid) _set_errno(42);
    return (Address)size;
}
__declspec(dllexport) Address mbrtoc16(Wide* out, const Byte* src, Address length, CrtUcharState* state) {
    return decode_uchar(out, src, length, state, CrtMbrtoc16);
}
__declspec(dllexport) Address mbrtoc32(unsigned* out, const Byte* src, Address length, CrtUcharState* state) {
    return decode_uchar(out, src, length, state, CrtMbrtoc32);
}
__declspec(dllexport) Address c16rtomb(Byte* out, Wide value, CrtUcharState* state) {
    return encode_uchar(out, value, state, CrtC16rtomb);
}
__declspec(dllexport) Address c32rtomb(Byte* out, unsigned value, CrtUcharState* state) {
    return encode_uchar(out, value, state, CrtC32rtomb);
}

static void write_byte(void* unused, Address at, Byte value) {
    (void)unused;
    *(Byte*)at = value;
}

static int secure_fill(Byte* text, Address size, unsigned value, enum CrtFillLimit limit, Address count,
                       const CrtGuestLocale* locale, int multibyte) {
    const Byte* types = text && size && multibyte ? lead_table(locale) : 0;
    const int error = crt_secure_fill(0, read_byte, write_byte, types, (Address)text, size, value, limit, count);
    if (error) _set_errno(error);
    if (error == 22) _invalid_parameter_noinfo();
    return error;
}
__declspec(dllexport) int _strset_s(char* text, Address size, int value) {
    return secure_fill((Byte*)text, size, (unsigned)value, CrtFillAll, 0, 0, 0);
}
__declspec(dllexport) int _mbsset_s_l(Byte* text, Address size, unsigned value, const CrtGuestLocale* locale) {
    return secure_fill(text, size, value, CrtFillAll, 0, locale, 1);
}
__declspec(dllexport) int _mbsset_s(Byte* text, Address size, unsigned value) {
    return _mbsset_s_l(text, size, value, 0);
}
#define CRT_COUNTED_FILL(name, limit) \
    __declspec(dllexport) int name##_l(Byte* text, Address size, unsigned value, Address count, const CrtGuestLocale* locale) { \
        return secure_fill(text, size, value, limit, count, locale, 1); \
    } \
    __declspec(dllexport) int name(Byte* text, Address size, unsigned value, Address count) { \
        return name##_l(text, size, value, count, 0); \
    }
CRT_COUNTED_FILL(_mbsnset_s, CrtFillCharacters)
CRT_COUNTED_FILL(_mbsnbset_s, CrtFillBytes)
#undef CRT_COUNTED_FILL

static int secure_append(Byte* dest, Address size, const Byte* src, Address count,
                         const CrtGuestLocale* locale, int byte_limit) {
    const Byte* types = dest && size && (src || !count) ? lead_table(locale) : 0;
    const struct CrtAppendResult result = crt_secure_append(0, read_byte, write_byte, types,
        (Address)dest, size, (Address)src, count, byte_limit, count == (Address)-1);
    if (result.error) _set_errno(result.error);
    if (result.error == 22 || result.error == 34) _invalid_parameter_noinfo();
    return (int)result.status;
}
#define CRT_COUNTED_APPEND(name, bytes) \
    __declspec(dllexport) int name##_l(Byte* dest, Address size, const Byte* src, Address count, const CrtGuestLocale* locale) { \
        return secure_append(dest, size, src, count, locale, bytes); \
    } \
    __declspec(dllexport) int name(Byte* dest, Address size, const Byte* src, Address count) { \
        return name##_l(dest, size, src, count, 0); \
    }
CRT_COUNTED_APPEND(_mbsncat_s, 0)
CRT_COUNTED_APPEND(_mbsnbcat_s, 1)
#undef CRT_COUNTED_APPEND

#include "crt_error_text.h"
__declspec(dllimport) int* __cdecl _errno(void);

static Address error_prefix_length(void* unused, Address text, Address limit, unsigned width) {
    (void)unused;
    Address length=0;
    if(width==1)while(length<limit && ((const Byte*)text)[length])++length;
    if(width==2)while(length<limit && ((const Wide*)text)[length])++length;
    return length;
}
static void error_copy(void* unused, Address dest, Address src, Address count) {
    (void)unused;
    for(Address i=0;i<count;++i)((Byte*)dest)[i]=((const Byte*)src)[i];
}
static void error_ascii(void* unused, Address dest, unsigned width, const char* text, Address length) {
    (void)unused;
    if(width==1){
        for(Address i=0;i<length;++i)((Byte*)dest)[i]=(Byte)text[i];
        ((Byte*)dest)[length]=0;
    }else{
        for(Address i=0;i<length;++i)((Wide*)dest)[i]=(Byte)text[i];
        ((Wide*)dest)[length]=0;
    }
}
static int error_text(void* dest, Address capacity, unsigned width, const void* prefix, unsigned code) {
    if(!dest || !capacity){
        _set_errno(22);
        _invalid_parameter_noinfo();
        return 22;
    }
    const CrtErrorWriter writer={0,error_prefix_length,error_copy,error_ascii};
    crt_format_error(&writer,(Address)dest,capacity,width,(Address)prefix,code);
    return 0;
}
__declspec(dllexport) int strerror_s(char* dest, Address count, int code) {
    return error_text(dest,count,1,0,(unsigned)code);
}
__declspec(dllexport) int _wcserror_s(Wide* dest, Address count, int code) {
    return error_text(dest,count,2,0,(unsigned)code);
}
__declspec(dllexport) int _strerror_s(char* dest, Address count, const char* prefix) {
    return error_text(dest,count,1,prefix,(unsigned)*_errno());
}
__declspec(dllexport) int __wcserror_s(Wide* dest, Address count, const Wide* prefix) {
    return error_text(dest,count,2,prefix,(unsigned)*_errno());
}
