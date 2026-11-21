#pragma once
#include <stdint.h>

/* D3DX 將指數 31 用作有限數，最大值為 131008。所有轉換直接處理
   位元，不受宿主浮點捨入模式、NaN 安靜化或 flush-to-zero 影響。 */
static inline uint32_t d3dx_expand_half(uint16_t half)
{
    const uint32_t sign=((uint32_t)half & 0x8000u)<<16;
    const uint32_t exponent=(half>>10)&31u;
    uint32_t fraction=half&1023u;
    if(exponent)
        return sign | ((exponent+112u)<<23) | (fraction<<13);
    if(!fraction)return sign;
    unsigned shift=0;
    while(!(fraction&1024u)){fraction<<=1;++shift;}
    return sign | ((113u-shift)<<23) | ((fraction&1023u)<<13);
}

static inline uint16_t d3dx_narrow_half(uint32_t bits)
{
    const unsigned sign=(bits>>16)&0x8000u;
    const unsigned exponent=(bits>>23)&255u;
    if(exponent>=144u)return (uint16_t)(sign|0x7fffu);
    if(exponent<102u)return (uint16_t)sign;

    unsigned significand=(bits&0x7fffffu)|0x800000u;
    /* 參考實作 的非正規數路徑先對齊至 13 個捨入位，再以最接近偶數取捨。 */
    if(exponent<=112u)significand>>=113u-exponent;
    unsigned rounded=(significand+0xfffu+((significand>>13)&1u))>>13;
    if(exponent>112u)rounded+=(exponent-113u)<<10;
    return (uint16_t)(sign | (rounded>0x7fffu?0x7fffu:rounded));
}
