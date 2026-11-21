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
