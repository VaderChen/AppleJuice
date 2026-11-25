#include <stdint.h>
#define RTL_ENTRY(T,name) __declspec(dllexport) T __stdcall guest_##name
#include "rtl_large_integer.h"

// 只有除數為零或窄商溢位時回到同一函式的宿主入口，以共用 Windows 例外派送。
__declspec(dllimport) uint32_t __stdcall RtlEnlargedUnsignedDivide(uint64_t,uint32_t,uint32_t*);
__declspec(dllimport) uint64_t __stdcall RtlExtendedLargeIntegerDivide(uint64_t,uint32_t,uint32_t*);
__declspec(dllimport) uint64_t __stdcall RtlLargeIntegerDivide(uint64_t,uint64_t,uint64_t*);

static inline uint32_t divide_words(uint32_t high,uint32_t low,uint32_t divisor,uint32_t *remainder) {
    uint32_t quotient, rest;
    __asm__("divl %4" : "=a"(quotient),"=d"(rest) : "0"(low),"1"(high),"rm"(divisor) : "cc");
    if(remainder)*remainder=rest;
    return quotient;
}

RTL_ENTRY(uint32_t,RtlEnlargedUnsignedDivide)(uint64_t a,uint32_t b,uint32_t *remainder) {
    if((a>>32)>=b)return RtlEnlargedUnsignedDivide(a,b,remainder);
    return divide_words((uint32_t)(a>>32),(uint32_t)a,b,remainder);
}

RTL_ENTRY(uint64_t,RtlExtendedLargeIntegerDivide)(uint64_t a,uint32_t b,uint32_t *remainder) {
    if(!b)return RtlExtendedLargeIntegerDivide(a,b,remainder);
    const uint32_t high=(uint32_t)(a>>32);
    const uint32_t upper=high/b;
    return ((uint64_t)upper<<32) | divide_words(high%b,(uint32_t)a,b,remainder);
}

RTL_ENTRY(uint64_t,RtlLargeIntegerDivide)(uint64_t a,uint64_t b,uint64_t *remainder) {
    if(!b)return RtlLargeIntegerDivide(a,b,remainder);
    uint64_t quotient=0;
    if(b<=UINT32_MAX){
        uint32_t rest;
        quotient=guest_RtlExtendedLargeIntegerDivide(a,(uint32_t)b,&rest);
        a=rest;
    }else if(a>=b){
        // 除數已超過 32 位元，對齊最高位後最多只需 32 次比較。
        unsigned shift=(unsigned)(__builtin_clz((uint32_t)(b>>32))-__builtin_clz((uint32_t)(a>>32)));
        uint64_t aligned=b<<shift;
        do{
            quotient<<=1;
            if(a>=aligned){a-=aligned;++quotient;}
            aligned>>=1;
        }while(shift--);
    }
    if(remainder)*remainder=a;
    return quotient;
}
