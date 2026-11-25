// RTL_ENTRY 讓相同的整數契約分別編入客體 PE 與宿主慢路徑。
RTL_ENTRY(uint64_t,RtlConvertLongToLargeInteger)(int32_t value) { return (uint64_t)(int64_t)value; }
RTL_ENTRY(uint64_t,RtlConvertUlongToLargeInteger)(uint32_t value) { return value; }
RTL_ENTRY(uint64_t,RtlLargeIntegerAdd)(uint64_t a,uint64_t b) { return a+b; }
RTL_ENTRY(uint64_t,RtlLargeIntegerSubtract)(uint64_t a,uint64_t b) { return a-b; }
RTL_ENTRY(uint64_t,RtlLargeIntegerNegate)(uint64_t value) { return -value; }
RTL_ENTRY(uint64_t,RtlEnlargedUnsignedMultiply)(uint32_t a,uint32_t b) { return (uint64_t)a*b; }
RTL_ENTRY(uint64_t,RtlEnlargedIntegerMultiply)(int32_t a,int32_t b) { return (uint64_t)((int64_t)a*b); }
RTL_ENTRY(uint64_t,RtlExtendedIntegerMultiply)(uint64_t a,int32_t b) { return a*(uint64_t)(int64_t)b; }
RTL_ENTRY(uint64_t,RtlLargeIntegerShiftLeft)(uint64_t a,unsigned count) { return a<<(count&63); }
RTL_ENTRY(uint64_t,RtlLargeIntegerShiftRight)(uint64_t a,unsigned count) { return a>>(count&63); }
RTL_ENTRY(uint64_t,RtlLargeIntegerArithmeticShift)(int64_t a,unsigned count) { return (uint64_t)(a>>(count&63)); }

static inline uint64_t rtl_product_high(uint64_t a,uint64_t b) {
#ifdef __SIZEOF_INT128__
    return (uint64_t)(((unsigned __int128)a*b)>>64);
#else
    const uint32_t al=(uint32_t)a, bl=(uint32_t)b, ah=(uint32_t)(a>>32), bh=(uint32_t)(b>>32);
    const uint64_t carry=((uint64_t)al*bl)>>32;
    const uint64_t cross=(uint64_t)ah*bl+carry;
    const uint64_t other=(uint64_t)al*bh+(uint32_t)cross;
    return (uint64_t)ah*bh+(cross>>32)+(other>>32);
#endif
}
RTL_ENTRY(uint64_t,RtlExtendedMagicDivide)(uint64_t dividend,uint64_t reciprocal,unsigned shift) {
    const int negative=(int)(dividend>>63);
    const uint64_t high=rtl_product_high(negative?-dividend:dividend,reciprocal);
    const uint64_t quotient=high>>(shift&63);
    return negative?-quotient:quotient;
}
