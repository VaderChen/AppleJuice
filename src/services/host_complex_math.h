#pragma once
/* 以 C99 libm 保留分支切線與有號零；客體 ABI／errno 提交由既有服務層管理。 */
enum X64armComplexKind {
    X64armComplexAbs,X64armComplexArg,X64armComplexReal,X64armComplexImag,
    X64armComplexCos,X64armComplexSin,X64armComplexTan,X64armComplexAcos,X64armComplexAsin,X64armComplexAtan,
    X64armComplexCosh,X64armComplexSinh,X64armComplexTanh,X64armComplexAcosh,X64armComplexAsinh,X64armComplexAtanh,
    X64armComplexExp,X64armComplexLog,X64armComplexLog10,X64armComplexPow,X64armComplexSqrt,X64armComplexConj,X64armComplexProj
};
#ifdef __cplusplus
extern "C" {
#endif
void x64arm_complex_double(enum X64armComplexKind,const double input[4],double output[2]);
void x64arm_complex_float(enum X64armComplexKind,const float input[4],float output[2]);
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
#include <complex>
namespace x64arm {
// CRT c* 函式與 STL <complex> 共用：float 版本以單精度計算，不經 double 中介。
template<class T> std::complex<T> host_complex(enum X64armComplexKind kind,std::complex<T> x,std::complex<T> y={}){
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif
    const T input[]{x.real(),x.imag(),y.real(),y.imag()};T output[2];
    if constexpr(sizeof(T)==4)x64arm_complex_float(kind,input,output);
    else x64arm_complex_double(kind,input,output);
    return {output[0],output[1]};
}
}
#endif
