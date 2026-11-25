#pragma STDC FP_CONTRACT OFF
#include "crt_complex_arithmetic.h"

#define ENTRY __declspec(dllexport)
#define CRT_COMPLEX_API(type, tag, prefix, norm_name) \
    ENTRY CrtComplex##tag prefix##build(type real, type imag) { return (CrtComplex##tag){real, imag}; } \
    ENTRY CrtComplex##tag prefix##mulcc(CrtComplex##tag x, CrtComplex##tag y) { return crt_product_##tag(x, y); } \
    ENTRY CrtComplex##tag prefix##mulcr(CrtComplex##tag x, type y) { return crt_scale_##tag(x, y); } \
    ENTRY type norm_name(CrtComplex##tag x) { return crt_norm_##tag(x); }

CRT_COMPLEX_API(double, d, _C, norm)
CRT_COMPLEX_API(float, f, _FC, normf)
