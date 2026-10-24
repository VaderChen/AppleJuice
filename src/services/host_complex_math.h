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
