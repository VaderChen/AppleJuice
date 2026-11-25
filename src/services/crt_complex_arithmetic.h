#pragma once

/* Windows 的 long double 與 double 使用相同的表示。兩種精度分別運算，
   不把單精度中間值升為 double，也不將實數乘法展開成複數乘法。 */
#define X64ARM_COMPLEX_ARITHMETIC(type, tag) \
    typedef struct { type real, imag; } CrtComplex##tag; \
    static inline CrtComplex##tag crt_product_##tag(CrtComplex##tag x, CrtComplex##tag y) { \
        return (CrtComplex##tag){x.real*y.real - x.imag*y.imag, x.real*y.imag + x.imag*y.real}; \
    } \
    static inline CrtComplex##tag crt_scale_##tag(CrtComplex##tag x, type y) { \
        return (CrtComplex##tag){x.real*y, x.imag*y}; \
    } \
    static inline type crt_norm_##tag(CrtComplex##tag x) { return x.real*x.real + x.imag*x.imag; }

X64ARM_COMPLEX_ARITHMETIC(float, f)
X64ARM_COMPLEX_ARITHMETIC(double, d)
#undef X64ARM_COMPLEX_ARITHMETIC
