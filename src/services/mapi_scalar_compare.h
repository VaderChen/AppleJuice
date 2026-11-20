#pragma once
#include <stdint.h>

/* 呼叫端只在相同型別時使用；間接值另外依客體指標寬度讀取。 */
static inline int mapi_scalar_order(unsigned type, uint64_t a, uint64_t b)
{
#define MAPI_ORDER(x,y) ((x) > (y) ? 1 : (x) == (y) ? 0 : -1)
    switch (type) {
    case 2: return MAPI_ORDER((int16_t)a,(int16_t)b);
    case 3: case 10: return MAPI_ORDER((int32_t)a,(int32_t)b);
    case 4: {
        uint32_t x=(uint32_t)a,y=(uint32_t)b;
        float l,r;
        __builtin_memcpy(&l,&x,4); __builtin_memcpy(&r,&y,4);
        return MAPI_ORDER(l,r);
    }
    case 5: case 7: {
        double l,r;
        __builtin_memcpy(&l,&a,8); __builtin_memcpy(&r,&b,8);
        return MAPI_ORDER(l,r);
    }
    case 6: case 20: return MAPI_ORDER((int64_t)a,(int64_t)b);
    case 11: return !!(uint16_t)a - !!(uint16_t)b;
    case 64: return MAPI_ORDER(a,b);
    default: return 0;
    }
#undef MAPI_ORDER
}
