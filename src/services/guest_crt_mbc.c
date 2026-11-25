#include "crt_mbc_contract.h"

typedef __UINTPTR_TYPE__ Address;
typedef unsigned char Byte;
typedef unsigned short Wide;
typedef struct {
    const Byte* mbc;
    const Wide* ctype;
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
