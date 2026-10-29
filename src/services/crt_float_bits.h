#pragma once
/* CRT 分類原語共用 IEEE 位元契約，避免 NaN 比較污染宿主或客體浮點旗標。 */
static inline int x64arm_classify_float_bits(unsigned long long bits,unsigned width) {
    const unsigned fraction_width=width==32?23:52;
    const unsigned exponent_mask=width==32?255:2047;
    const unsigned exponent=(unsigned)(bits>>fraction_width)&exponent_mask;
    const unsigned long long fraction=bits&((1ULL<<fraction_width)-1);
    if(exponent==exponent_mask)return fraction?2:1;
    return exponent?-1:fraction?-2:0;
}
static inline int x64arm_fpclass_float_bits(unsigned long long bits,unsigned width) {
    const unsigned fraction_width=width==32?23:52;
    const int classification=x64arm_classify_float_bits(bits,width);
    const int negative=(int)((bits>>(width-1))&1);
    if(classification==2)return bits&(1ULL<<(fraction_width-1))?2:1;
    if(classification==1)return negative?4:512;
    if(classification==-1)return negative?8:256;
    if(classification==-2)return negative?16:128;
    return negative?32:64;
}
static inline unsigned short x64arm_sign_float_bits(unsigned long long bits,unsigned width) {
    return (unsigned short)(((bits>>(width-1))&1)<<15);
}
static inline int x64arm_ordered_float_bits(unsigned long long a,unsigned long long b,unsigned width) {
    const unsigned long long sign=1ULL<<(width-1),mask=sign-1;
    const unsigned long long infinity=width==32?0x7f800000ULL:0x7ff0000000000000ULL;
    const unsigned long long magnitude_a=a&mask,magnitude_b=b&mask;
    if(magnitude_a>infinity || magnitude_b>infinity)return 0;
    if(a==b || !(magnitude_a|magnitude_b))return 2;
    const unsigned long long ordered_a=(a&sign)?~a:a|sign,ordered_b=(b&sign)?~b:b|sign;
    /* 32-bit 反相只保留 32 bits，維持兩種寬度的有號浮點順序。 */
    const unsigned long long all=width==32?0xffffffffULL:~0ULL;
    return (ordered_a&all)<(ordered_b&all)?1:4;
}
static inline int x64arm_ilogb_float_bits(unsigned long long bits,unsigned width,int* invalid) {
    const unsigned fraction_width=width==32?23:52,bias=width==32?127:1023;
    const unsigned long long fraction_mask=(1ULL<<fraction_width)-1;
    const unsigned exponent=(unsigned)((bits>>fraction_width)&(width==32?255:2047));
    const unsigned long long fraction=bits&fraction_mask;
    *invalid=!exponent?!fraction:exponent==(width==32?255U:2047U);
    if(*invalid)return !exponent?(-2147483647-1):2147483647;
    if(exponent)return (int)exponent-(int)bias;
    unsigned highest=0;for(unsigned long long value=fraction;value>>=1;)++highest;
    return (int)highest+1-(int)bias-(int)fraction_width;
}
