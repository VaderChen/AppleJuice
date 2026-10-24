#include "host_complex_math.h"
#include <complex.h>
#include <math.h>
#pragma STDC FENV_ACCESS ON

/* 不使用 a + I*b 建構，避免額外乘法破壞無限大、NaN 與負零。 */
#define DEFINE_COMPLEX(width,type,suffix) \
void x64arm_complex_##width(enum X64armComplexKind kind,const type input[4],type output[2]) { \
    type complex x=__builtin_complex(input[0],input[1]),y=__builtin_complex(input[2],input[3]),value; \
    switch(kind) { \
    case X64armComplexAbs:output[0]=cabs##suffix(x);output[1]=0;return; \
    case X64armComplexArg:output[0]=carg##suffix(x);output[1]=0;return; \
    case X64armComplexReal:output[0]=input[0];output[1]=0;return; \
    case X64armComplexImag:output[0]=input[1];output[1]=0;return; \
    case X64armComplexCos:value=ccos##suffix(x);break; \
    case X64armComplexSin:value=csin##suffix(x);break; \
    case X64armComplexTan:value=ctan##suffix(x);break; \
    case X64armComplexAcos:value=cacos##suffix(x);break; \
    case X64armComplexAsin:value=casin##suffix(x);break; \
    case X64armComplexAtan:value=catan##suffix(x);break; \
    case X64armComplexCosh:value=ccosh##suffix(x);break; \
    case X64armComplexSinh:value=csinh##suffix(x);break; \
    case X64armComplexTanh:value=ctanh##suffix(x);break; \
    case X64armComplexAcosh:value=cacosh##suffix(x);break; \
    case X64armComplexAsinh:value=casinh##suffix(x);break; \
    case X64armComplexAtanh:value=catanh##suffix(x);break; \
    case X64armComplexExp:value=cexp##suffix(x);break; \
    case X64armComplexLog:value=clog##suffix(x);break; \
    case X64armComplexLog10:value=clog##suffix(x);output[0]=creal##suffix(value)/(type)2.3025850929940456840179914546843642;output[1]=cimag##suffix(value)/(type)2.3025850929940456840179914546843642;return; \
    case X64armComplexPow:value=cpow##suffix(x,y);break; \
    case X64armComplexSqrt:value=csqrt##suffix(x);break; \
    case X64armComplexConj:value=conj##suffix(x);break; \
    case X64armComplexProj:value=cproj##suffix(x);break; \
    default:output[0]=NAN;output[1]=NAN;return; \
    } \
    output[0]=creal##suffix(value);output[1]=cimag##suffix(value); \
}
DEFINE_COMPLEX(double,double,)
DEFINE_COMPLEX(float,float,f)
#undef DEFINE_COMPLEX
