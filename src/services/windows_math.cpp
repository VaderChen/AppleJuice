#include "x64arm/windows_services.hpp"
#include "windows_scalar_math.hpp"
#include "windows_math_environment.hpp"
#include "host_complex_math.h"
#include "crt_float_bits.h"
#include "x64arm/x87_fp.hpp"
#include "x64arm/unicode.hpp"
#include "d3dx_sh.hpp"
extern "C" {
#include "softfloat.h"
}
#include <bit>
#include <cerrno>
#include <cfenv>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <queue>
#include <numeric>
#include <fstream>

namespace x64arm {
namespace {
using ComplexKind=X64armComplexKind;
struct ComplexOperation {
    std::string_view name;ComplexKind kind;bool single{},real{},binary{};
};
// 複數的輸入／輸出是 CRT 結構 ABI，不是兩個獨立的浮點參數。
constexpr ComplexOperation complex_operations[]{
#define COMPLEX_REAL(name,kind) {#name,X64armComplex##kind,false,true},{#name "f",X64armComplex##kind,true,true}
#define COMPLEX_UNARY(name,kind) {#name,X64armComplex##kind},{#name "f",X64armComplex##kind,true}
    COMPLEX_REAL(cabs,Abs),COMPLEX_REAL(carg,Arg),COMPLEX_REAL(creal,Real),COMPLEX_REAL(cimag,Imag),
    COMPLEX_UNARY(ccos,Cos),COMPLEX_UNARY(csin,Sin),COMPLEX_UNARY(ctan,Tan),
    COMPLEX_UNARY(cacos,Acos),COMPLEX_UNARY(casin,Asin),COMPLEX_UNARY(catan,Atan),
    COMPLEX_UNARY(ccosh,Cosh),COMPLEX_UNARY(csinh,Sinh),COMPLEX_UNARY(ctanh,Tanh),
    COMPLEX_UNARY(cacosh,Acosh),COMPLEX_UNARY(casinh,Asinh),COMPLEX_UNARY(catanh,Atanh),
    COMPLEX_UNARY(cexp,Exp),COMPLEX_UNARY(clog,Log),COMPLEX_UNARY(clog10,Log10),
    COMPLEX_UNARY(csqrt,Sqrt),COMPLEX_UNARY(conj,Conj),COMPLEX_UNARY(cproj,Proj),
    {"cpow",X64armComplexPow,false,false,true},{"cpowf",X64armComplexPow,true,false,true}
#undef COMPLEX_UNARY
#undef COMPLEX_REAL
};
const ComplexOperation* complex_operation(std::string_view name){
    for(const auto& operation:complex_operations)if(name==operation.name)return &operation;
    return nullptr;
}
template<class T> std::complex<T> complex_value(ComplexKind kind,std::complex<T> x,std::complex<T> y){
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif
    const T input[]{x.real(),x.imag(),y.real(),y.imag()};T output[2];
    if constexpr(sizeof(T)==4)x64arm_complex_float(kind,input,output);
    else x64arm_complex_double(kind,input,output);
    return {output[0],output[1]};
}
}
std::span<const WindowsServiceDescriptor> math_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"d3dx9_42.dll","D3DXSHDot",3},
        {"d3dx9_42.dll","D3DXSHEvalConeLight",9},
        {"d3dx9_42.dll","D3DXMatrixRotationAxis",3},
        {"d3dx9_42.dll","D3DXQuaternionRotationYawPitchRoll",4},
        {"d3dx9_42.dll","D3DXVec3Unproject",6},
        {"d3dx9_42.dll","D3DXVec4Normalize",2},
        {"d3dx9_42.dll","D3DXMatrixMultiplyTranspose",3},
        {"d3dx9_42.dll","D3DXComputeBoundingBox",5},
        {"d3dx9_42.dll","D3DXFloat32To16Array",3},
        {"d3dx9_43.dll","D3DXSHDot",3},
        {"d3dx9_43.dll","D3DXSHEvalConeLight",9},
        {"d3dx9_43.dll","D3DXMatrixRotationAxis",3},
        {"d3dx9_43.dll","D3DXQuaternionRotationYawPitchRoll",4},
        {"d3dx9_43.dll","D3DXVec3Unproject",6},
        {"d3dx9_43.dll","D3DXVec4Normalize",2},
        {"d3dx9_43.dll","D3DXMatrixMultiplyTranspose",3},
        {"d3dx9_43.dll","D3DXComputeBoundingBox",5},
        {"d3dx9_43.dll","D3DXFloat32To16Array",3},
        {"d3dx9_42.dll","D3DXMatrixOrthoLH",5},
        {"d3dx9_42.dll","D3DXMatrixOrthoRH",5},
        {"d3dx9_43.dll","D3DXMatrixOrthoLH",5},
        {"d3dx9_43.dll","D3DXMatrixOrthoRH",5},
        {"d3dx9_42.dll","D3DXSHAdd",4},
        {"d3dx9_42.dll","D3DXSHScale",4},
        {"d3dx9_42.dll","D3DXSHEvalDirection",3},
        {"d3dx9_42.dll","D3DXSHRotate",4},
        {"d3dx9_43.dll","D3DXSHAdd",4},
        {"d3dx9_43.dll","D3DXSHScale",4},
        {"d3dx9_43.dll","D3DXSHEvalDirection",3},
        {"d3dx9_43.dll","D3DXSHRotate",4},
        {"d3dx9_42.dll","D3DXOptimizeFaces",5},
        {"d3dx9_42.dll","D3DXOptimizeVertices",5},
        {"d3dx9_43.dll","D3DXOptimizeFaces",5},
        {"d3dx9_43.dll","D3DXOptimizeVertices",5},
        {"d3dx9_42.dll","D3DXMatrixScaling",4},
        {"d3dx9_42.dll","D3DXMatrixTranslation",4},
        {"d3dx9_42.dll","D3DXMatrixMultiply",3},
        {"d3dx9_42.dll","D3DXMatrixTranspose",2},
        {"d3dx9_42.dll","D3DXMatrixInverse",3},
        {"d3dx9_42.dll","D3DXMatrixRotationQuaternion",2},
        {"d3dx9_42.dll","D3DXMatrixRotationYawPitchRoll",4},
        {"d3dx9_42.dll","D3DXMatrixRotationX",2},
        {"d3dx9_42.dll","D3DXMatrixRotationY",2},
        {"d3dx9_42.dll","D3DXMatrixRotationZ",2},
        {"d3dx9_42.dll","D3DXMatrixPerspectiveFovLH",5},
        {"d3dx9_42.dll","D3DXMatrixOrthoOffCenterLH",7},
        {"d3dx9_42.dll","D3DXMatrixLookAtLH",4},
        {"d3dx9_42.dll","D3DXMatrixReflect",2},
        {"d3dx9_42.dll","D3DXVec2Normalize",2},
        {"d3dx9_42.dll","D3DXVec3Normalize",2},
        {"d3dx9_42.dll","D3DXVec3Transform",3},
        {"d3dx9_42.dll","D3DXVec3TransformCoord",3},
        {"d3dx9_42.dll","D3DXVec3TransformNormal",3},
        {"d3dx9_42.dll","D3DXVec4Transform",3},
        {"d3dx9_42.dll","D3DXQuaternionMultiply",3},
        {"d3dx9_42.dll","D3DXQuaternionRotationMatrix",2},
        {"d3dx9_42.dll","D3DXQuaternionNormalize",2},
        {"d3dx9_42.dll","D3DXQuaternionInverse",2},
        {"d3dx9_42.dll","D3DXPlaneFromPointNormal",3},
        {"d3dx9_42.dll","D3DXComputeBoundingSphere",5},
        {"d3dx9_42.dll","D3DXFloat16To32Array",3},
        {"d3dx9_43.dll","D3DXMatrixScaling",4},
        {"d3dx9_43.dll","D3DXMatrixTranslation",4},
        {"d3dx9_43.dll","D3DXMatrixMultiply",3},
        {"d3dx9_43.dll","D3DXMatrixTranspose",2},
        {"d3dx9_43.dll","D3DXMatrixInverse",3},
        {"d3dx9_43.dll","D3DXMatrixRotationQuaternion",2},
        {"d3dx9_43.dll","D3DXMatrixRotationYawPitchRoll",4},
        {"d3dx9_43.dll","D3DXMatrixRotationX",2},
        {"d3dx9_43.dll","D3DXMatrixRotationY",2},
        {"d3dx9_43.dll","D3DXMatrixRotationZ",2},
        {"d3dx9_43.dll","D3DXMatrixPerspectiveFovLH",5},
        {"d3dx9_43.dll","D3DXMatrixOrthoOffCenterLH",7},
        {"d3dx9_43.dll","D3DXMatrixLookAtLH",4},
        {"d3dx9_43.dll","D3DXMatrixReflect",2},
        {"d3dx9_43.dll","D3DXVec2Normalize",2},
        {"d3dx9_43.dll","D3DXVec3Normalize",2},
        {"d3dx9_43.dll","D3DXVec3Transform",3},
        {"d3dx9_43.dll","D3DXVec3TransformCoord",3},
        {"d3dx9_43.dll","D3DXVec3TransformNormal",3},
        {"d3dx9_43.dll","D3DXVec4Transform",3},
        {"d3dx9_43.dll","D3DXQuaternionMultiply",3},
        {"d3dx9_43.dll","D3DXQuaternionRotationMatrix",2},
        {"d3dx9_43.dll","D3DXQuaternionNormalize",2},
        {"d3dx9_43.dll","D3DXQuaternionInverse",2},
        {"d3dx9_43.dll","D3DXPlaneFromPointNormal",3},
        {"d3dx9_43.dll","D3DXComputeBoundingSphere",5},
        {"d3dx9_43.dll","D3DXFloat16To32Array",3},
        {"msvbvm60.dll","_allmul",2,3},{"crt","_allmul",2,3},
        {"msvbvm60.dll","_CIatan",0},{"msvbvm60.dll","_CIcos",0},{"msvbvm60.dll","_CIexp",0},
        {"msvbvm60.dll","_CIlog",0},{"msvbvm60.dll","_CIsin",0},{"msvbvm60.dll","_CIsqrt",0},{"msvbvm60.dll","_CItan",0},
        {"crt", "_ecvt_s", 6},{"crt","div",2},{"crt","ldiv",2},
        {"crt","_dtest",1},{"crt","_fdtest",1},{"crt","_ldtest",1},
        {"crt","wcstod",2},{"crt","wcstof",2},{"crt","_wtof",1},
        {"crt","_CIacos",0},{"crt","_CIasin",0},{"crt","_CIatan",0},{"crt","_CIatan2",0},
        {"crt","_CIcos",0},{"crt","_CIcosh",0},{"crt","_CIexp",0},{"crt","_CIfmod",0},
        {"crt","_CIlog",0},{"crt","_CIlog10",0},{"crt","_CIpow",0},{"crt","_CIsin",0},
        {"crt","_CIsinh",0},{"crt","_CIsqrt",0},{"crt","_CItan",0},{"crt","_CItanh",0},
        {"crt","_ftol",0},{"crt","_ftol2",0},{"crt","_ftol2_sse",0},
        {"crt","_clear87",0},{"crt","_clearfp",0},{"crt","_status87",0},{"crt","_statusfp",0},{"crt","_statusfp2",2},
        {"crt", "fetestexcept", 1}, {"crt", "feclearexcept", 1}, {"crt", "_dclass", 1}, {"crt", "_fdclass", 1}, {"crt", "_ldclass", 1}, {"crt", "_fpclass", 1}, {"crt", "_fpclassf", 1}, {"crt", "_dsign", 1}, {"crt", "_fdsign", 1}, {"crt", "_ldsign", 1}, {"crt", "_control87", 2}, {"crt", "_controlfp", 2}, {"crt","_controlfp_s",3},
        {"crt","__fpe_flt_rounds",0},{"crt","_fpreset",0},
        {"crt","_dpcomp",2,3},{"crt","_fdpcomp",2},{"crt","ilogb",1,1},{"crt","ilogbf",1},
        {"crt", "acos", 1,1}, {"crt", "acosh", 1,1}, {"crt", "asin", 1,1}, {"crt", "asinh", 1,1},
        {"crt", "atan", 1,1}, {"crt", "atanh", 1,1}, {"crt", "cbrt", 1,1}, {"crt", "ceil", 1,1},
        {"crt", "cos", 1,1}, {"crt", "cosh", 1,1}, {"crt", "erf", 1,1}, {"crt", "erfc", 1,1},
        {"crt", "exp", 1,1}, {"crt", "exp2", 1,1}, {"crt", "expm1", 1,1}, {"crt", "fabs", 1,1},
        {"crt", "floor", 1,1}, {"crt", "log", 1,1}, {"crt", "log10", 1,1}, {"crt", "log1p", 1,1},
        {"crt", "log2", 1,1}, {"crt", "logb", 1,1}, {"crt", "nearbyint", 1,1}, {"crt", "rint", 1,1},
        {"crt", "round", 1,1}, {"crt", "sin", 1,1}, {"crt", "sinh", 1,1}, {"crt", "sqrt", 1,1},
        {"crt", "tan", 1,1}, {"crt", "tanh", 1,1}, {"crt", "tgamma", 1,1}, {"crt", "lgamma", 1,1}, {"crt", "lgammaf", 1}, {"crt", "lgammal", 1,1}, {"crt", "trunc", 1,1},
        {"crt", "atan2", 2,3}, {"crt", "copysign", 2,3}, {"crt", "_copysign", 2,3}, {"crt", "fdim", 2,3},
        {"crt", "fmax", 2,3}, {"crt", "fmin", 2,3}, {"crt", "fmod", 2,3}, {"crt", "hypot", 2,3},
        {"crt", "nextafter", 2,3}, {"crt", "pow", 2,3}, {"crt", "remainder", 2,3}, {"crt", "fma", 3,7},
        {"crt", "frexp", 2,1}, {"crt", "modf", 2,1}, {"crt", "ldexp", 2,1}, {"crt", "scalbn", 2,1},
        {"crt", "sinf", 1}, {"crt", "cosf", 1}, {"crt", "tanf", 1}, {"crt", "asinf", 1}, {"crt", "acosf", 1}, {"crt", "atanf", 1},
        {"crt", "sinhf", 1}, {"crt", "coshf", 1}, {"crt", "tanhf", 1}, {"crt", "sqrtf", 1}, {"crt", "cbrtf", 1},
        {"crt", "asinhf", 1}, {"crt", "acoshf", 1}, {"crt", "atanhf", 1}, {"crt", "erff", 1}, {"crt", "erfcf", 1},
        {"crt", "expf", 1}, {"crt", "exp2f", 1}, {"crt", "expm1f", 1}, {"crt", "logf", 1}, {"crt", "log2f", 1}, {"crt", "log10f", 1}, {"crt", "log1pf", 1},
        {"crt", "floorf", 1}, {"crt", "ceilf", 1}, {"crt", "roundf", 1}, {"crt", "truncf", 1}, {"crt", "fabsf", 1}, {"crt", "nearbyintf", 1}, {"crt", "rintf", 1},
        {"crt", "atan2f", 2}, {"crt", "powf", 2}, {"crt", "fmodf", 2}, {"crt", "remainderf", 2}, {"crt", "hypotf", 2}, {"crt", "_hypotf", 2},
        {"crt", "copysignf", 2}, {"crt", "fminf", 2}, {"crt", "fmaxf", 2}, {"crt", "nextafterf", 2}, {"crt", "fdimf", 2},
        {"crt", "fmaf", 3}, {"crt", "frexpf", 2}, {"crt", "modff", 2}, {"crt", "ldexpf", 2}, {"crt", "scalbnf", 2},
        {"crt", "_j0", 1,1}, {"crt", "_j1", 1,1}, {"crt", "_jn", 2,2}, {"crt", "_y0", 1,1}, {"crt", "_y1", 1,1}, {"crt", "_yn", 2,2},
        {"crt", "_finite", 1,1}, {"crt", "_isnan", 1,1},
        {"crt", "lrint", 1,1}, {"crt", "lrintf", 1}, {"crt", "lrintl", 1,1}, {"crt", "llrint", 1,1}, {"crt", "llrintf", 1}, {"crt", "llrintl", 1,1}, {"crt", "lround", 1,1}, {"crt", "lroundf", 1}, {"crt", "lroundl", 1,1}, {"crt", "llround", 1,1}, {"crt", "llroundf", 1}, {"crt", "llroundl", 1,1},
        {"crt", "atof", 1}, {"crt", "atoi", 1}, {"crt", "atol", 1}, {"crt", "_atoi64", 1},
        {"crt", "_strtoi64", 3}, {"crt", "_strtoui64", 3}, {"crt", "strtol", 3}, {"crt", "strtoul", 3}, {"crt", "strtoll", 3}, {"crt", "strtoull", 3}, {"crt", "strtod", 2}, {"crt", "strtof", 2}
    };
    static const auto complete=[]{
        std::vector<WindowsServiceDescriptor> result(std::begin(table),std::end(table));
        for(const auto& operation:complex_operations){
            // 16-byte 結果在兩種 ABI 都有第一個隱藏指標；8-byte 結果使用 RAX／EDX:EAX。
            const unsigned inputs=operation.binary?2U:1U,hidden=!operation.single && !operation.real?1U:0U;
            const auto mask=(1U<<inputs)-1;
            result.push_back({"crt",operation.name,inputs+hidden,operation.single?mask:0U,operation.single?0U:mask<<hidden});
        }
        return result;
    }();
    return complete;
}
namespace {
double scalar(const GuestState& state, unsigned index) {
    std::uint64_t raw = 0;
    for (unsigned i = 0; i < 8; ++i) raw |= std::uint64_t{state.xmm[index][i]} << (i * 8);
    return std::bit_cast<double>(raw);
}
float scalar_float(const GuestState& state, unsigned index) {
    std::uint32_t raw=0;for(unsigned i=0;i<4;++i)raw|=std::uint32_t{state.xmm[index][i]}<<(i*8);
    return std::bit_cast<float>(raw);
}
std::uint64_t scalar_result_bits(double value,bool single){
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif
    return single?std::bit_cast<std::uint32_t>(static_cast<float>(value)):std::bit_cast<std::uint64_t>(value);
}
void conversion_result(GuestState& state,bool x86,double value,bool single){
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif
    // 未選取的 float 轉換不能投機執行，否則 double 回傳也可能在提交後新增例外。
    if(x86){
        const auto top=((state.x87_status>>11)+7U)&7U;
        const auto converted=f64_to_extF80({std::bit_cast<std::uint64_t>(value)});
        state.x87[top]={converted.signif,converted.signExp};state.x87_status=static_cast<std::uint16_t>((state.x87_status&~0x3800U)|(top<<11));
        state.x87_tag=static_cast<std::uint16_t>((state.x87_tag&~(3U<<(top*2)))|(x87_tag(state.x87[top])<<(top*2)));
    }else{
        const auto raw=scalar_result_bits(value,single);
        for(unsigned i=0;i<(single?4U:8U);++i)state.xmm[0][i]=static_cast<std::uint8_t>(raw>>(8*i));
    }
}
using Unary = double(*)(double);
static const std::pair<std::string_view, Unary> math_double_unary[]{
    {"acos",std::acos},{"acosh",std::acosh},{"asin",std::asin},{"asinh",std::asinh},{"atan",std::atan},{"atanh",std::atanh},
    {"cbrt",std::cbrt},{"ceil",std::ceil},{"cos",std::cos},{"cosh",std::cosh},{"erf",std::erf},{"erfc",std::erfc},
    {"exp",std::exp},{"exp2",std::exp2},{"expm1",std::expm1},{"fabs",std::fabs},{"floor",std::floor},
    {"log",std::log},{"log10",std::log10},{"log1p",std::log1p},{"log2",std::log2},{"logb",std::logb},
    {"nearbyint",std::nearbyint},{"rint",std::rint},{"round",std::round},{"sin",std::sin},{"sinh",std::sinh},
    {"sqrt",std::sqrt},{"tan",std::tan},{"tanh",std::tanh},{"tgamma",std::tgamma},{"lgamma",std::lgamma},{"trunc",std::trunc}
};
using Binary = double(*)(double,double);
static const std::pair<std::string_view, Binary> math_double_binary[]{
    {"atan2",std::atan2},{"copysign",std::copysign},{"_copysign",std::copysign},{"fdim",std::fdim},
    {"fmax",std::fmax},{"fmin",std::fmin},{"fmod",std::fmod},{"hypot",std::hypot},
    {"nextafter",std::nextafter},{"pow",std::pow},{"remainder",std::remainder}
};
using FloatUnary=float(*)(float);
static const std::pair<std::string_view,FloatUnary> math_float_unary[]{
    {"sinf",std::sin},{"cosf",std::cos},{"tanf",std::tan},{"asinf",std::asin},{"acosf",std::acos},{"atanf",std::atan},
    {"sinhf",std::sinh},{"coshf",std::cosh},{"tanhf",std::tanh},{"sqrtf",std::sqrt},{"cbrtf",std::cbrt},
    {"asinhf",std::asinh},{"acoshf",std::acosh},{"atanhf",std::atanh},{"erff",std::erf},{"erfcf",std::erfc},
    {"expf",std::exp},{"exp2f",std::exp2},{"expm1f",std::expm1},{"logf",std::log},{"log2f",std::log2},{"log10f",std::log10},{"log1pf",std::log1p},
    {"floorf",std::floor},{"ceilf",std::ceil},{"roundf",std::round},{"truncf",std::trunc},{"fabsf",std::fabs},{"nearbyintf",std::nearbyint},{"rintf",std::rint}};
using FloatBinary=float(*)(float,float);
static const std::pair<std::string_view,FloatBinary> math_float_binary[]{
    {"atan2f",std::atan2},{"powf",std::pow},{"fmodf",std::fmod},{"remainderf",std::remainder},{"hypotf",std::hypot},{"_hypotf",std::hypot},
    {"copysignf",std::copysign},{"fminf",std::fmin},{"fmaxf",std::fmax},{"nextafterf",std::nextafter},{"fdimf",std::fdim}};
struct IntegerRounding {
    std::string_view name;
    bool single{},large{},directional{};
};
static constexpr IntegerRounding integer_rounding[]{
    {"lrint",false,false,true},{"lrintf",true,false,true},{"lrintl",false,false,true},
    {"llrint",false,true,true},{"llrintf",true,true,true},{"llrintl",false,true,true},
    {"lround",false,false,false},{"lroundf",true,false,false},{"lroundl",false,false,false},
    {"llround",false,true,false},{"llroundf",true,true,false},{"llroundl",false,true,false}};
const IntegerRounding* integer_rounding_operation(std::string_view name) noexcept {
    for(const auto& operation:integer_rounding)if(operation.name==name)return &operation;
    return nullptr;
}
std::uint64_t round_integer(double input,const IntegerRounding& operation){
#if defined(__clang__)
    // 例外旗標是客體可見結果；不得提前計算未選取的 rint／round 分支。
#pragma STDC FENV_ACCESS ON
#endif
    // Windows long 為 32 位；lrint 使用目前 RC，lround 的半值固定離零。
    // 超界／NaN／Infinity 保留既有 Windows CRT 契約：回傳零並提交 FE_INVALID。
    const auto rounded=operation.directional?std::rint(input):std::round(input);
    const auto limit=operation.large?0x1p63:0x1p31;
    if(!std::isfinite(rounded) || rounded< -limit || rounded>=limit){std::feraiseexcept(FE_INVALID);return 0;}
    if(operation.directional && rounded!=input)std::feraiseexcept(FE_INEXACT);
    // 只做一次已驗證範圍的浮點轉換；避免最佳化器同時計算 32／64 位候選值，
    // 讓合法的 64 位結果因不會採用的 32 位轉換而誤設 FE_INVALID。
    const auto value=static_cast<std::uint64_t>(static_cast<std::int64_t>(rounded));
    return operation.large?value:static_cast<std::uint32_t>(value);
}
using Environment=WindowsMathEnvironment;
unsigned math_exception_flags(unsigned raised){
    return ((raised&FE_INVALID)?1U:0U)|((raised&FE_DIVBYZERO)?4U:0U)|
        ((raised&FE_OVERFLOW)?8U:0U)|((raised&FE_UNDERFLOW)?16U:0U)|((raised&FE_INEXACT)?32U:0U);
}
unsigned math_error(unsigned raised,int host_error){
    return host_error ? (host_error==EDOM?33U:host_error==ERANGE?34U:22U):
        (raised&FE_INVALID)?33U:(raised&(FE_DIVBYZERO|FE_OVERFLOW|FE_UNDERFLOW))?34U:0U;
}
unsigned commit_math_environment(GuestState& state,GuestMemory& memory,std::uint64_t errno_cell,bool x86,const Environment& environment){
    const auto raised=static_cast<unsigned>(Environment::raised());
    const auto fp_flags=math_exception_flags(raised);
    if(x86){state.x87_status=static_cast<std::uint16_t>(state.x87_status|fp_flags);if(fp_flags&~static_cast<unsigned>(state.x87_control)&63U)state.x87_status|=0x8080;}
    else state.mxcsr|=fp_flags;
    const auto error=math_error(raised,environment.error());
    if (error) memory.store(GuestAddress{errno_cell},32,static_cast<std::uint32_t>(error));
    return static_cast<unsigned>(raised);
}

}

enum class ScalarRounding : std::uint8_t { General, Floor, Ceil, Trunc, Away, Current, Exact };
struct ScalarMathOperation {
    std::string_view name;
    Unary double_unary{};
    Binary double_binary{};
    FloatUnary float_unary{};
    FloatBinary float_binary{};
    const IntegerRounding* integer{};
    ScalarRounding rounding{ScalarRounding::General};
    bool single() const noexcept {return float_unary || float_binary;}
};
const ScalarMathOperation* scalar_math_operation(const WindowsServiceDescriptor& service) {
    // 純量值進、純量值出：不配置、不等候、不回呼，不操作 GUI／IO 或 LastError。
    // 同一登錄表同時供一般 provider 與快速入口使用，不另外辨識程式或呼叫位址。
    static const auto operations=[]{
        std::vector<ScalarMathOperation> result;
        for(const auto& [name,function]:math_double_unary)if(name!="lgamma")result.push_back({name,function});
        for(const auto& [name,function]:math_double_binary)result.push_back({name,{},function});
        for(const auto& [name,function]:math_float_unary)result.push_back({name,{},{},function});
        for(const auto& [name,function]:math_float_binary)result.push_back({name,{},{},{},function});
        for(const auto& operation:integer_rounding)result.push_back({operation.name,{},{},{},{},&operation});
        for(auto& operation:result){
            const auto name=operation.name;
            if(name=="floor" || name=="floorf")operation.rounding=ScalarRounding::Floor;
            else if(name=="ceil" || name=="ceilf")operation.rounding=ScalarRounding::Ceil;
            else if(name=="trunc" || name=="truncf")operation.rounding=ScalarRounding::Trunc;
            else if(name=="round" || name=="roundf")operation.rounding=ScalarRounding::Away;
            else if(name=="nearbyint" || name=="nearbyintf")operation.rounding=ScalarRounding::Current;
            else if(name=="rint" || name=="rintf")operation.rounding=ScalarRounding::Exact;
        }
        return result;
    }();
    const auto find=[&](const WindowsServiceDescriptor& descriptor)->const ScalarMathOperation*{
        if(descriptor.family!="crt")return nullptr;
        for(const auto& operation:operations)if(operation.name==descriptor.symbol)return &operation;
        return nullptr;
    };
    const auto descriptors=runtime_service_descriptors();
    static const auto indexed=[&]{
        std::vector<const ScalarMathOperation*> result;result.reserve(descriptors.size());
        for(const auto& descriptor:descriptors)result.push_back(find(descriptor));
        return result;
    }();
    const auto address=reinterpret_cast<std::uintptr_t>(&service),begin=reinterpret_cast<std::uintptr_t>(descriptors.data());
    if(address>=begin && address-begin<descriptors.size_bytes() && (address-begin)%sizeof(service)==0)return indexed[(address-begin)/sizeof(service)];
    return find(service);
}
bool scalar_math_requested() noexcept {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_SCALAR_MATH");return !setting || std::string_view(setting)!="0";}();
    return enabled;
}
unsigned scalar_math_recipe(const WindowsServiceDescriptor& service) noexcept {
    // 核心 ARM64 配方涵蓋已核准輸入與浮點模式；其餘交回同一入口的純量值服務。
    // 範圍錯誤的原生路徑須具備 resident_scalar_errors 提供的私人格位契約。
    const auto* operation=scalar_math_operation(service);
    if(!operation)return 0;
    if(operation->name=="expf")return 1;
    if(operation->name=="log2")return 2;
    if(operation->name=="logf")return 3;
    if(operation->name=="sinf")return 4;
    if(operation->name=="cosf")return 5;
    if(operation->name=="lrintf")return 6;
    if(operation->name=="lrint")return 7;
    if(operation->name=="lroundf")return 8;
    if(operation->name=="lround")return 9;
    if(operation->name=="acosf")return 10;
    if(operation->name=="powf")return 11;
    if(operation->name=="hypot")return 12;
    if(operation->name=="cos")return 13;
    if(operation->name=="sin")return 14;
    if(operation->name=="log")return 15;
    if(operation->name=="exp")return 16;
    static const bool atan_recipes=[] {
        const auto* setting=std::getenv("X64ARM_SCALAR_ATAN_RECIPES");
        return !setting || std::string_view(setting)!="0";
    }();
    if(atan_recipes){
        if(operation->name=="atanf")return 17;
        if(operation->name=="atan")return 18;
        if(operation->name=="atan2f")return 19;
        if(operation->name=="atan2")return 20;
    }
    static const bool range_native=[] {
        const auto* setting=std::getenv("X64ARM_SCALAR_RANGE_NATIVE");
        return !setting || std::string_view(setting)!="0";
    }();
    if(range_native && operation->name=="acos")return 21;
    return 0;
}
namespace {
#if defined(__aarch64__) && defined(__APPLE__)
template<class Float> Float hardware_round_integral(Float input,unsigned mode) {
    Float result;
    // 明確指定捨入方向，不改寫 FPCR，也不讓 FRINTI 使用宿主的 RC。
    // 呼叫前已排除非零次正規與 NaN；這些 FRINT 不產生 inexact。
#define X64ARM_ROUND_INTEGRAL(op) \
    if constexpr(sizeof(Float)==4)asm volatile(op " %s0, %s1":"=w"(result):"w"(input):"memory"); \
    else asm volatile(op " %d0, %d1":"=w"(result):"w"(input):"memory")
    switch(mode){
    case 0: X64ARM_ROUND_INTEGRAL("frintn");break;
    case 1: X64ARM_ROUND_INTEGRAL("frintm");break;
    case 2: X64ARM_ROUND_INTEGRAL("frintp");break;
    case 3: X64ARM_ROUND_INTEGRAL("frintz");break;
    default: X64ARM_ROUND_INTEGRAL("frinta");break;
    }
#undef X64ARM_ROUND_INTEGRAL
    return result;
}
template<class UInt,class Float,unsigned Fraction,unsigned Exponent>
bool direct_round_integral(UInt input,UInt& output,std::uint32_t& mxcsr,ScalarRounding operation) {
    constexpr UInt sign=UInt{1}<<(sizeof(UInt)*8-1),fraction=(UInt{1}<<Fraction)-1;
    constexpr UInt exponent=((UInt{1}<<Exponent)-1)<<Fraction;
    // 特殊值仍走共用 libm 環境，保留宿主 FZ/DN、NaN payload 與例外／errno 契約。
    if((input&exponent)==exponent || (!(input&exponent) && (input&fraction)))return false;
    unsigned mode;
    switch(operation){
    case ScalarRounding::Floor:mode=1;break;
    case ScalarRounding::Ceil:mode=2;break;
    case ScalarRounding::Trunc:mode=3;break;
    case ScalarRounding::Away:mode=4;break;
    default:mode=(mxcsr>>13)&3U;break;
    }
    const auto result=std::bit_cast<UInt>(hardware_round_integral(std::bit_cast<Float>(input),mode));
    // rint 唯一額外效果為精度 sticky；直接提交，不污染宿主 FPSR。
    if(operation==ScalarRounding::Exact && (input&~sign)!=(result&~sign))mxcsr|=32U;
    output=result;
    return true;
}
bool direct_round_integral(std::uint64_t input,std::uint64_t& output,std::uint32_t& mxcsr,const ScalarMathOperation& operation) {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_SCALAR_ROUNDING");return !setting || std::string_view(setting)!="0";}();
    if(!enabled)return false;
    if(operation.single()){
        std::uint32_t result{};
        if(!direct_round_integral<std::uint32_t,float,23,8>(static_cast<std::uint32_t>(input),result,mxcsr,operation.rounding))return false;
        output=result;return true;
    }
    return direct_round_integral<std::uint64_t,double,52,11>(input,output,mxcsr,operation.rounding);
}
bool direct_round_integral(GuestState& state,const ScalarMathOperation& operation) {
    std::uint64_t input{},result{};std::memcpy(&input,state.xmm[0].data(),sizeof(input));
    if(!direct_round_integral(input,result,state.mxcsr,operation))return false;
    std::memcpy(state.xmm[0].data(),&result,operation.single()?4:8);return true;
}
#endif
}
#ifdef X64ARM_HOST_SERVICES_SCALAR_NATIVE
// X64ARM_SCALAR_ERRNO_RESIDENT=0 讓需要 errno 的結果退回完整服務入口，供同成品開關對照。
static bool scalar_errno_resident_requested() noexcept {
    static const bool enabled=[] {const auto* setting=std::getenv("X64ARM_SCALAR_ERRNO_RESIDENT");return !setting || std::string_view(setting)!="0";}();
    return enabled;
}
template<bool Worker>
static bool invoke_native_scalar_math_impl(ResidentScalarCall& call,GuestMemory& memory,const ScalarMathOperation& operation,
                                           std::uint64_t errno_cell,const ResidentScalarErrors* errors) noexcept {
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif
    try{
        // 沿 WindowsCall 的完整 Win64 框架契約；無效時不運算，交回原服務產生同一故障。
        if(memory.execution_mode()!=ExecutionMode::X64 || call.stack%16!=8 || call.stack>UINT64_MAX-40)return false;
        std::uint64_t target{};
        if constexpr(Worker){
            // worker 僅借用不改映射／頁面快取的視窗；跨頁 home 區及 COW
            // 等未核准存取交回協調者，保留原 WindowsCall 的故障契約。
            const auto stack=memory.native_window(GuestAddress{call.stack},permission(Access::Read));
            const auto home=memory.native_window(GuestAddress{call.stack+8},permission(Access::Write));
            if(!stack.data || stack.after<8 || !home.data || home.after<32)return false;
            std::memcpy(&target,stack.data,8);
        }else{
            memory.check(GuestAddress{call.stack},8,Access::Read);
            memory.check(GuestAddress{call.stack+8},32,Access::Write);
            target=memory.load(GuestAddress{call.stack},64);
        }
        auto mxcsr=call.mxcsr;std::uint64_t result{},integer{};
        const auto width=operation.integer?0U:operation.single()?4U:8U;
#if defined(__aarch64__) && defined(__APPLE__)
        if(operation.rounding!=ScalarRounding::General && direct_round_integral(call.first,result,mxcsr,operation)){
            call.integer=0;call.result=result;call.width=width;call.mxcsr=mxcsr;call.next=target;
            call.environment.retain();return true;
        }
#endif
        Environment environment(mxcsr,&call.environment);
        const auto input_double=[&](unsigned n){return std::bit_cast<double>(n?call.second:call.first);};
        const auto input_float=[&](unsigned n){return std::bit_cast<float>(static_cast<std::uint32_t>(n?call.second:call.first));};
        double value{};float single_value{};
        if(operation.integer){
            const auto& conversion=*operation.integer;
            integer=round_integer(conversion.single?static_cast<double>(input_float(0)):input_double(0),conversion);
        }else if(operation.double_unary)value=operation.double_unary(input_double(0));
        else if(operation.double_binary)value=operation.double_binary(input_double(0),input_double(1));
        else if(operation.float_unary)single_value=operation.float_unary(input_float(0));
        else single_value=operation.float_binary(input_float(0),input_float(1));
        const auto raised=static_cast<unsigned>(Environment::raised());
        mxcsr|=math_exception_flags(raised);
        const auto error=math_error(raised,environment.error());
        // errno 與完整入口相同：先寫 errno 格位，再由核心提交回傳值與返回堆疊。
        // errno 格位與返回堆疊重疊、或關閉開關時，仍交回完整入口保留原寫入順序；
        // 寫入故障由 catch 交回原服務產生同一故障，這裡不改動 call。
        if(error){
            if(!scalar_errno_resident_requested())return false;
            if(errno_cell+4>call.stack && errno_cell<call.stack+40)return false;
            if constexpr(Worker){
                if(!errors->cell || errno_cell!=errors->guest_address)return false;
                const auto error_value=static_cast<std::uint32_t>(error);
                std::memcpy(errors->cell,&error_value,sizeof(error_value));
            }else memory.store(GuestAddress{errno_cell},32,static_cast<std::uint32_t>(error));
        }
        if(!operation.integer)result=scalar_result_bits(operation.single()?static_cast<double>(single_value):value,operation.single());
        call.integer=integer;call.result=result;call.width=width;call.mxcsr=mxcsr;call.next=target;
        const auto magnitude=std::bit_cast<std::uint32_t>(single_value)&0x7fffffffU;
        const bool exact_return=operation.integer || !operation.single() || !magnitude ||
            (magnitude>=0x00800000U && magnitude<0x7f800000U);
        if(exact_return && !(raised&0x80U))call.environment.retain_committed_status();
        return true;
    }catch(...){call.environment.retained=0;return false;}
}
bool invoke_native_scalar_math(ResidentScalarCall& call,GuestMemory& memory,const ScalarMathOperation& operation,
                               std::uint64_t errno_cell) noexcept {
    return invoke_native_scalar_math_impl<false>(call,memory,operation,errno_cell,nullptr);
}
bool invoke_native_context_scalar_math(ResidentScalarCall& call,GuestMemory& memory,const ScalarMathOperation& operation,
                                       const ResidentScalarErrors& errors) noexcept {
    return invoke_native_scalar_math_impl<true>(call,memory,operation,errors.guest_address,&errors);
}
#endif
void invoke_scalar_math(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service,
                        const ScalarMathOperation& operation,std::uint64_t errno_cell,bool x86,const ResidentHostEnvironment* boundary) {
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif
    const WindowsCall arg(state,memory,service);
#if defined(__aarch64__) && defined(__APPLE__)
    // Win64 純量取整家族共用硬體路徑；Win32 x87 ABI 沿用原來的環境與回傳流程。
    if(!x86 && operation.rounding!=ScalarRounding::General && direct_round_integral(state,operation)){
        arg.return_to(state,0);
#ifdef X64ARM_HOST_SERVICES_RETAINED_FP
        // 此路徑已排除 NaN／次正規；FRINT 非 exact 形式不寫 FPSR，rint 的 PE 直接交 MXCSR。
        if(boundary)boundary->retain();
#endif
        return;
    }
#endif
    Environment environment(x86?static_cast<std::uint32_t>((state.x87_control>>10)&3U)<<13:state.mxcsr,boundary);
    const auto input_double=[&](unsigned n){return x86?std::bit_cast<double>(arg(n)):scalar(state,n);};
    const auto input_float=[&](unsigned n){return x86?std::bit_cast<float>(static_cast<std::uint32_t>(arg(n))):scalar_float(state,n);};
    if(operation.integer){
        const auto& conversion=*operation.integer;
        const auto value=round_integer(conversion.single?static_cast<double>(input_float(0)):input_double(0),conversion);
        const auto raised=commit_math_environment(state,memory,errno_cell,x86,environment);
        if(x86 && conversion.large)state.reg(Gpr::Rdx)=static_cast<std::uint32_t>(value>>32);
        arg.return_to(state,value);
#ifdef X64ARM_HOST_SERVICES_COMMITTED_FP_STATUS
        // CRT 沒有把 IDC 當成 DE 提交；含 IDC 的狀態不能借給核心重新摺疊。
        if(boundary && !x86 && !(raised&0x80U))boundary->retain_committed_status();
#else
        (void)raised;
#endif
        return;
    }
    double value{};float single_value{};
    if(operation.double_unary)value=operation.double_unary(input_double(0));
    else if(operation.double_binary)value=operation.double_binary(input_double(0),input_double(1));
    else if(operation.float_unary)single_value=operation.float_unary(input_float(0));
    else single_value=operation.float_binary(input_float(0),input_float(1));
    const auto raised=commit_math_environment(state,memory,errno_cell,x86,environment);
    conversion_result(state,x86,operation.single()?static_cast<double>(single_value):value,operation.single());
    arg.return_to(state,0);
#ifdef X64ARM_HOST_SERVICES_COMMITTED_FP_STATUS
    // Win64 double 回傳只搬移位元；float 沿用既有 double 中介 ABI，但僅對正規值
    // 與零宣告保留，確保提交後的兩次精度轉換不產生新的例外。特殊值沿用清除流程。
    const auto bits=std::bit_cast<std::uint32_t>(single_value),magnitude=bits&0x7fffffffU;
    const bool exact_return=!operation.single() || !magnitude ||
        (magnitude>=0x00800000U && magnitude<0x7f800000U);
    if(boundary && !x86 && exact_return && !(raised&0x80U))boundary->retain_committed_status();
#else
    (void)raised;
#endif
}
std::optional<std::uint32_t> WindowsServices::invoke_math(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service) {
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif
    if(scalar_math_requested())if(const auto* operation=scalar_math_operation(service)){
        invoke_scalar_math(state,memory,service,*operation,runtime_.errno_cell,x86_);return std::nullopt;
    }
    const WindowsCall arg(state, memory, service); const auto symbol = service.symbol;
    if(const auto* operation=complex_operation(symbol)){
        const auto execute=[&]<class T>(){
            constexpr bool single=sizeof(T)==4;
            const bool hidden=!single && !operation->real;
            const unsigned first=hidden?1U:0U;
            const auto input=[&](unsigned index)->std::complex<T>{
                if constexpr(single){const auto bits=arg(index);return {std::bit_cast<float>(static_cast<std::uint32_t>(bits)),std::bit_cast<float>(static_cast<std::uint32_t>(bits>>32))};}
                else {const auto pointer=arg(index);memory.check(GuestAddress{pointer},16,Access::Read);
                    return {std::bit_cast<double>(memory.load(GuestAddress{pointer},64)),std::bit_cast<double>(memory.load(GuestAddress{pointer+8},64))};}
            };
            // 先取得完整輸入再寫回，保留輸出與輸入重疊及精確記憶體故障契約。
            const auto x=input(first),y=operation->binary?input(first+1):std::complex<T>{};
            const auto output=hidden?arg(0):0;
            if(hidden)memory.check(GuestAddress{output},16,Access::Write);
            Environment environment(x86_?static_cast<std::uint32_t>((state.x87_control>>10)&3U)<<13:state.mxcsr);
            const auto result=complex_value(operation->kind,x,y);
            if(operation->real){
                if(x86_)conversion_result(state,true,result.real(),single);
                else {
                    const auto bits=[&] {if constexpr(single)return std::uint64_t{std::bit_cast<std::uint32_t>(result.real())};else return std::bit_cast<std::uint64_t>(result.real());}();
                    for(unsigned n=0;n<sizeof(T);++n)state.xmm[0][n]=static_cast<std::uint8_t>(bits>>(n*8));
                }
                commit_math_environment(state,memory,runtime_.errno_cell,x86_,environment);
                arg.return_to(state,0);
            }else if constexpr(single){
                const auto bits=std::bit_cast<std::uint32_t>(result.real())|(std::uint64_t{std::bit_cast<std::uint32_t>(result.imag())}<<32);
                commit_math_environment(state,memory,runtime_.errno_cell,x86_,environment);
                arg.return_to(state,bits,true);
            }else {
                commit_math_environment(state,memory,runtime_.errno_cell,x86_,environment);
                memory.store(GuestAddress{output},64,std::bit_cast<std::uint64_t>(result.real()));
                memory.store(GuestAddress{output+8},64,std::bit_cast<std::uint64_t>(result.imag()));
                arg.return_to(state,output);
            }
        };
        if(operation->single)execute.template operator()<float>();else execute.template operator()<double>();
        return std::nullopt;
    }
    if(symbol=="_fpreset"){
        state.mxcsr=0x1f80;
        if(x86_){state.x87_control=0x27f;state.x87_status=0;state.x87_tag=0xffff;
            state.x87_instruction_pointer=state.x87_data_pointer=0;
            state.x87_opcode=state.x87_code_selector=state.x87_data_selector=0;}
        arg.return_to(state,0);return std::nullopt;
    }
    if(symbol=="_dpcomp" || symbol=="_fdpcomp" || symbol=="ilogb" || symbol=="ilogbf"){
        const unsigned width=(symbol=="_fdpcomp" || symbol=="ilogbf")?32U:64U;
        const auto bits=[&](unsigned index){
            if(x86_)return arg(index);
            std::uint64_t value=0;for(unsigned n=0;n<width/8;++n)value|=std::uint64_t{state.xmm[index][n]}<<(n*8);return value;
        };
        if(symbol.starts_with('_'))arg.return_to(state,static_cast<unsigned>(x64arm_ordered_float_bits(bits(0),bits(1),width)));
        else {
            int invalid=0;const auto value=x64arm_ilogb_float_bits(bits(0),width,&invalid);
            if(invalid){
                memory.store(GuestAddress{runtime_.errno_cell},32,33);
                if(x86_){state.x87_status|=1;if(!(state.x87_control&1))state.x87_status|=0x8080;}
                else state.mxcsr|=1;
            }
            arg.return_to(state,static_cast<std::uint32_t>(value));
        }
        return std::nullopt;
    }
    if(symbol=="__fpe_flt_rounds"){
        constexpr std::array<std::uint32_t,4> modes{1,3,2,0};const auto sse=(state.mxcsr>>13)&3U;
        const auto value=x86_ && ((state.x87_control>>10)&3U)!=sse?UINT32_MAX:modes[sse];
        arg.return_to(state,value);return std::nullopt;
    }
#include "d3dx_math_handlers.inc"
    if(symbol=="_allmul"){
        if(!x86_)throw HostCallFault("_allmul 是 Win32 編譯器 helper ABI");
        // CRT／VB6 共用兩個 QWORD；此 helper 特別由 callee 彈出 16 bytes。
        const WindowsCall helper(state,memory,2,true,3);
        helper.return_to(state,helper(0)*helper(1),true);return std::nullopt;
    }
    if(symbol=="lgamma" || symbol=="lgammaf" || symbol=="lgammal"){
        const bool single=symbol=="lgammaf";
        Environment environment(x86_?static_cast<std::uint32_t>((state.x87_control>>10)&3U)<<13:state.mxcsr);
        // MSVC 的 long double 與 double 同為 64 位；Win32 參數在堆疊、結果在 ST(0)。
        const auto input=single?static_cast<double>(x86_?std::bit_cast<float>(static_cast<std::uint32_t>(arg(0))):scalar_float(state,0)):(x86_?std::bit_cast<double>(arg(0)):scalar(state,0));
        const auto value=single?static_cast<double>(std::lgamma(static_cast<float>(input))):std::lgamma(input);
        auto raised=Environment::raised();
        // 宿主 libm 對 pole 不一定設定 errno／fenv；依數學定義補足 Windows CRT 的錯誤契約。
        if(std::isfinite(input) && input<=0 && std::trunc(input)==input){errno=ERANGE;raised|=FE_DIVBYZERO;}
        else if(std::isfinite(input) && std::isinf(value)){errno=ERANGE;raised|=FE_OVERFLOW;}
        const unsigned flags=((raised&FE_INVALID)?1U:0U)|((raised&FE_DIVBYZERO)?4U:0U)|((raised&FE_OVERFLOW)?8U:0U)|((raised&FE_UNDERFLOW)?16U:0U)|((raised&FE_INEXACT)?32U:0U);
        if(x86_){state.x87_status=static_cast<std::uint16_t>(state.x87_status|flags);if(flags&~static_cast<unsigned>(state.x87_control)&63U)state.x87_status|=0x8080;}
        else state.mxcsr|=flags;
        if(errno || (raised&(FE_INVALID|FE_DIVBYZERO|FE_OVERFLOW|FE_UNDERFLOW)))memory.store(GuestAddress{runtime_.errno_cell},32,errno==EDOM || (raised&FE_INVALID)?33:34);
        conversion_result(state,x86_,value,single);arg.return_to(state,0);return std::nullopt;
    }
    if(symbol=="div" || symbol=="ldiv"){
        const auto numerator=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(0)));
        const auto denominator=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(1)));
        if(!denominator || (numerator==INT32_MIN && denominator==-1)){
            (void)write_standard_output(2,std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>("integer division error\n"),23));
            return 3;
        }
        const auto quotient=static_cast<std::uint32_t>(numerator/denominator),remainder=static_cast<std::uint32_t>(numerator%denominator);
        // Windows int／long 都是 32 位；8-byte POD 在 x86 使用 EDX:EAX，x64 使用 RAX。
        arg.return_to(state,quotient|(std::uint64_t{remainder}<<32),true);return std::nullopt;
    }
    if(symbol=="_clear87" || symbol=="_clearfp" || symbol=="_status87" || symbol=="_statusfp" || symbol=="_statusfp2"){
        constexpr std::array<std::uint32_t,6> flags{0x10,0x80000,8,4,2,1};
        const auto map=[&](unsigned status){std::uint32_t result=0;for(unsigned n=0;n<6;++n)if(status&(1U<<n))result|=flags[n];return result;};
        auto x87=x86_?map(state.x87_status):0U;const auto sse=map(state.mxcsr);
        if(x86_ && (state.x87_status&0x40))x87|=(state.x87_status&0x200)?0x200U:0x400U;
        if(symbol=="_statusfp2"){if(arg(0))memory.store(GuestAddress{arg(0)},32,x87);if(arg(1))memory.store(GuestAddress{arg(1)},32,sse);arg.return_to(state,0);}
        else {if(symbol.starts_with("_clear")){state.mxcsr&=~63U;if(x86_)state.x87_status&=0x7f00;}else if(x86_)check_x87_pending(state);arg.return_to(state,x87|sse);}return std::nullopt;
    }
    if(symbol.starts_with("_CI") || symbol=="_ftol" || symbol=="_ftol2" || symbol=="_ftol2_sse"){
        if(!x86_)throw HostCallFault("此 CRT 特殊呼叫慣例僅提供 x86");
        check_x87_pending(state);
        const bool integral=symbol.starts_with("_ftol");
        // 整數轉換完全由 SoftFloat 處理，不讀寫宿主 FPCR／FPSR 或 errno。
        std::optional<Environment> environment;
        if(!integral)environment.emplace(static_cast<std::uint32_t>((state.x87_control>>10)&3U)<<13);
        struct SoftEnvironment {
            uint_fast8_t rounding{softfloat_roundingMode},tininess{softfloat_detectTininess},exceptions{softfloat_exceptionFlags};
            explicit SoftEnvironment(unsigned control){constexpr uint_fast8_t modes[]{softfloat_round_near_even,softfloat_round_min,softfloat_round_max,softfloat_round_minMag};softfloat_roundingMode=modes[(control>>10)&3];softfloat_detectTininess=softfloat_tininess_afterRounding;softfloat_exceptionFlags=0;}
            ~SoftEnvironment(){softfloat_roundingMode=rounding;softfloat_detectTininess=tininess;softfloat_exceptionFlags=exceptions;}
        } soft_environment(state.x87_control);
        const auto top=(state.x87_status>>11)&7U;unsigned raised=0;bool stack_fault=false;
        const auto read=[&](unsigned logical){const auto index=(top+logical)&7U;if(((state.x87_tag>>(index*2))&3U)==3){raised|=1;stack_fault=true;return extFloat80_t{UINT64_C(0xc000000000000000),0xffff};}const auto value=state.x87[index];if(!(value.sign_exponent&0x7fffU) && value.significand)raised|=2;return extFloat80_t{value.significand,value.sign_exponent};};
        const auto first=read(0);const bool binary_input=symbol=="_CIatan2" || symbol=="_CIfmod" || symbol=="_CIpow";
        std::uint64_t integer=0;double value=0;
        if(integral)integer=static_cast<std::uint64_t>(extF80_to_i64_r_minMag(first,true));
        else {
            const auto a=std::bit_cast<double>(extF80_to_f64(first).v);
            if(binary_input){const auto b=std::bit_cast<double>(extF80_to_f64(read(1)).v);value=symbol=="_CIatan2"?std::atan2(b,a):symbol=="_CIfmod"?std::fmod(b,a):std::pow(b,a);}
            else {using Function=double(*)(double);static const std::pair<std::string_view,Function> functions[]{
                {"_CIacos",std::acos},{"_CIasin",std::asin},{"_CIatan",std::atan},{"_CIcos",std::cos},{"_CIcosh",std::cosh},{"_CIexp",std::exp},{"_CIlog",std::log},{"_CIlog10",std::log10},{"_CIsin",std::sin},{"_CIsinh",std::sinh},{"_CIsqrt",std::sqrt},{"_CItan",std::tan},{"_CItanh",std::tanh}};
                bool found=false;for(const auto& [name,function]:functions)if(symbol==name){value=function(a);found=true;break;}if(!found)throw HostCallFault("未知的 CRT x87 數學入口");}
        }
        const auto host=integral?0:Environment::raised();const auto soft=softfloat_exceptionFlags;
        if((host&FE_INVALID) || (soft&softfloat_flag_invalid))raised|=1;
        if((host&FE_DIVBYZERO) || (soft&softfloat_flag_infinite))raised|=4;
        if((host&FE_OVERFLOW) || (soft&softfloat_flag_overflow))raised|=8;
        if((host&FE_UNDERFLOW) || (soft&softfloat_flag_underflow))raised|=16;
        if((host&FE_INEXACT) || (soft&softfloat_flag_inexact))raised|=32;
        state.x87_status=static_cast<std::uint16_t>(state.x87_status|raised|(stack_fault?0x40:0));if(stack_fault)state.x87_status&=~0x200U;
        if(!integral){
            const auto error=errno?(errno==EDOM?33:errno==ERANGE?34:22):(raised&1)?33:(raised&28)?34:0;
            if(error)memory.store(GuestAddress{runtime_.errno_cell},32,static_cast<unsigned>(error));
        }
        if(raised&~static_cast<unsigned>(state.x87_control)&63U){state.x87_status|=0x8080;check_x87_pending(state);}
        if(integral || binary_input){state.x87_tag=static_cast<std::uint16_t>(state.x87_tag|(3U<<(top*2)));state.x87_status=static_cast<std::uint16_t>((state.x87_status&~0x3800U)|(((top+1)&7U)<<11));}
        if(!integral){const auto destination=(state.x87_status>>11)&7U;const auto out=f64_to_extF80({std::bit_cast<std::uint64_t>(value)});state.x87[destination]={out.signif,out.signExp};state.x87_tag=static_cast<std::uint16_t>((state.x87_tag&~(3U<<(destination*2)))|(x87_tag(state.x87[destination])<<(destination*2)));}
        arg.return_to(state,integer,integral);return std::nullopt;
    }
    if(symbol=="fetestexcept" || symbol=="feclearexcept") {
        constexpr std::array<std::uint32_t,6> bits{0x10,0x80000,8,4,2,1};std::uint32_t result=0;
        const auto requested=static_cast<std::uint32_t>(arg(0));
        for(unsigned n=0;n<bits.size();++n)if(requested&bits[n]) {
            if(symbol=="feclearexcept"){state.mxcsr&=~(1U<<n);if(x86_)state.x87_status=static_cast<std::uint16_t>(state.x87_status&~(1U<<n));}
            else if((state.mxcsr|(x86_?state.x87_status:0U))&(1U<<n))result|=bits[n];
        }
        arg.return_to(state,result);return std::nullopt;
    }
    if(symbol=="_dclass" || symbol=="_fdclass" || symbol=="_ldclass" || symbol=="_fpclass" || symbol=="_fpclassf" || symbol=="_dtest" || symbol=="_fdtest" || symbol=="_ldtest") {
        const unsigned width=(symbol=="_fdclass" || symbol=="_fpclassf" || symbol=="_fdtest")?32:64;
        std::uint64_t raw=0;
        if(symbol.ends_with("test"))raw=memory.load(GuestAddress{arg(0)},static_cast<std::uint8_t>(width));
        else if(x86_)raw=memory.load(GuestAddress{state.reg(Gpr::Rsp)+4},static_cast<std::uint8_t>(width));
        else for(unsigned n=0;n<width/8;++n)raw|=std::uint64_t{state.xmm[0][n]}<<(n*8);
        const unsigned mantissa=width==64?52:23;const auto exponent=(raw>>mantissa)&(width==64?0x7ffU:0xffU);
        const auto fraction=raw&((UINT64_C(1)<<mantissa)-1);const bool negative=raw>>(width-1);
        std::int32_t classification;
        if(exponent==(width==64?0x7ffU:0xffU))classification=fraction?2:1;
        else classification=exponent?-1:fraction?-2:0;
        std::uint32_t result=static_cast<std::uint32_t>(classification);
        if(symbol.starts_with("_fpclass")) {
            if(classification==2)result=(fraction&(UINT64_C(1)<<(mantissa-1)))?2U:1U;
            else if(classification==1)result=negative?4:512;
            else if(classification==-1)result=negative?8:256;
            else if(classification==-2)result=negative?16:128;
            else result=negative?32:64;
        }
        arg.return_to(state,result);return std::nullopt;
    }
    if(symbol=="_dsign" || symbol=="_fdsign" || symbol=="_ldsign") {
        const unsigned width=symbol=="_fdsign"?32:64;
        const auto sign=x86_?(memory.load(GuestAddress{state.reg(Gpr::Rsp)+4},static_cast<std::uint8_t>(width))>>(width-1)):(state.xmm[0][width/8-1]>>7);
        arg.return_to(state,sign?0x8000:0);return std::nullopt;
    }
    if(symbol=="_control87" || symbol=="_controlfp" || symbol=="_controlfp_s") {
        const bool secure=symbol=="_controlfp_s";const unsigned first=secure?1U:0U;
        const auto requested=static_cast<std::uint32_t>(arg(first));
        auto mask=static_cast<std::uint32_t>(arg(first+1));if(symbol!="_control87")mask&=~0x80000U;
        const bool invalid=(!x86_ && (mask&0x70000U)) || (secure && (requested&mask&~0x30f031fU));
        if(secure && arg(0))memory.check(GuestAddress{arg(0)},4,Access::Write);
        if(invalid)mask=0;
        constexpr std::array<std::uint32_t,6> exceptions{0x10,0x80000,8,4,2,1};
        for(unsigned i=0;i<exceptions.size();++i)if(mask&exceptions[i]) {
            const auto bit=1U<<(7+i);state.mxcsr=(state.mxcsr&~bit)|((requested&exceptions[i])?bit:0);
            if(x86_)state.x87_control=static_cast<std::uint16_t>((state.x87_control&~(1U<<i))|((requested&exceptions[i])?(1U<<i):0));
        }
        const auto rounding_mask=(mask&0x300U)<<5;
        state.mxcsr=(state.mxcsr&~rounding_mask)|((requested<<5)&rounding_mask);
        if(x86_) {
            const auto rc=(mask&0x300U)<<2;state.x87_control=static_cast<std::uint16_t>((state.x87_control&~rc)|((requested<<2)&rc));
            if(mask&0x30000U){const auto pc=requested&0x30000U;state.x87_control=static_cast<std::uint16_t>((state.x87_control&~0x300U)|(pc==0x20000?0:pc==0x10000?0x200:0x300));}
            if(mask&0x40000U)state.x87_control=static_cast<std::uint16_t>((state.x87_control&~0x1000U)|((requested&0x40000U)?0x1000:0));
        }
        if(mask&0x3000000U){
            const auto mode=requested&0x3000000U;state.mxcsr&=~0x8040U;
            if(mode==0x1000000 || mode==0x2000000)state.mxcsr|=0x8000;
            if(mode==0x1000000 || mode==0x3000000)state.mxcsr|=0x40;
        }
        std::uint32_t result=(state.mxcsr>>5)&0x300U;
        for(unsigned i=0;i<exceptions.size();++i)if(state.mxcsr&(1U<<(7+i)))result|=exceptions[i];
        const bool flush=state.mxcsr&0x8000,zero=state.mxcsr&0x40;
        result|=flush?(zero?0x1000000U:0x2000000U):(zero?0x3000000U:0U);
        if(x86_){
            const auto pc=state.x87_control&0x300;result|=pc==0?0x20000U:pc==0x200?0x10000U:0U;if(state.x87_control&0x1000)result|=0x40000;
            std::uint32_t x87=(state.x87_control>>2)&0x300U;for(unsigned i=0;i<exceptions.size();++i)if(state.x87_control&(1U<<i))x87|=exceptions[i];
            if((x87^result)&0x8031fU)result|=0x80000000U;
        }
        if(secure && arg(0))memory.store(GuestAddress{arg(0)},32,result);
        if(invalid){
            memory.store(GuestAddress{runtime_.errno_cell},32,22);
            if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,secure?22U:0U);return std::nullopt;}
            return 0xc0000417U;
        }
        arg.return_to(state,secure?0U:result);return std::nullopt;
    }
    if(symbol=="_ecvt_s"){
        const auto value=x86_?std::bit_cast<double>(memory.load(GuestAddress{state.reg(Gpr::Rsp)+12},64)):scalar(state,2);
        const auto count=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(x86_?memory.load(GuestAddress{state.reg(Gpr::Rsp)+20},32):arg(3)));
        const auto decimal=x86_?memory.load(GuestAddress{state.reg(Gpr::Rsp)+24},32):arg(4),sign=x86_?memory.load(GuestAddress{state.reg(Gpr::Rsp)+28},32):arg(5);
        if(!arg(0) || !arg(1) || !decimal || !sign){
            memory.store(GuestAddress{runtime_.errno_cell},32,22);
            if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,22);return std::nullopt;}
            return 0xc0000417U;
        }
        const auto digits=static_cast<std::size_t>(std::max(count,0));
        if(digits>1048576)throw HostCallFault("CRT 浮點字串精度超過上限");
        memory.check(GuestAddress{decimal},4,Access::Write);memory.check(GuestAddress{sign},4,Access::Write);
        if(digits>=arg(1)){memory.store(GuestAddress{arg(0)},8,0);memory.store(GuestAddress{runtime_.errno_cell},32,34);arg.return_to(state,34);return std::nullopt;}
        Environment environment(0x1f80);const bool negative=std::signbit(value);int point=0;std::string mantissa;
        if(std::isnan(value)){mantissa=negative?"1#IND":"1#QNAN";point=1;}
        else if(std::isinf(value)){mantissa="1#INF";point=1;}
        else if(value==0)mantissa="0";
        else {
            // binary64 的 17 位有效十進位表示；多餘要求位數以零補齊。
            std::array<char,64> formatted{};std::snprintf(formatted.data(),formatted.size(),"%.16e",std::fabs(value));
            const std::string scientific(formatted.data());const auto exponent=scientific.find('e');
            if(exponent==scientific.npos)throw HostCallFault("無法產生 CRT 十進位表示");
            point=static_cast<int>(std::strtol(scientific.c_str()+exponent+1,nullptr,10))+1;
            for(std::size_t i=0;i<exponent;++i)if(scientific[i]>='0' && scientific[i]<='9')mantissa.push_back(scientific[i]);
        }
        const bool carry=count>=0 && digits<mantissa.size() && mantissa[digits]>='5';mantissa.resize(digits,'0');
        if(carry){auto i=mantissa.size();while(i && mantissa[i-1]=='9'){mantissa[--i]='0';}
            if(i)++mantissa[i-1];else {if(!mantissa.empty())mantissa[0]='1';++point;}}
        std::vector<std::uint8_t> output(mantissa.begin(),mantissa.end());output.push_back(0);memory.write(GuestAddress{arg(0)},output);
        memory.store(GuestAddress{decimal},32,static_cast<std::uint32_t>(point));memory.store(GuestAddress{sign},32,negative?1:0);
        arg.return_to(state,0);return std::nullopt;
    }
    Environment environment(x86_?static_cast<std::uint32_t>((state.x87_control>>10)&3U)<<13:state.mxcsr);
    // Win32 cdecl 依描述表讀取 4／8-byte 堆疊參數；Win64 使用 XMM。
    const auto input_double=[&](unsigned i){return i<service.arguments?(x86_?std::bit_cast<double>(arg(i)):scalar(state,i)):0.0;};
    const auto input_float=[&](unsigned i){return x86_?std::bit_cast<float>(static_cast<std::uint32_t>(arg(i))):scalar_float(state,i);};
    const auto x = input_double(0), y = input_double(1);
    double value = 0; std::uint64_t integer = 0; bool floating = true;


    bool handled = false, single = false; float float_value=0;


    for(const auto& [name,function]:math_float_unary)if(symbol==name){float_value=function(input_float(0));single=handled=true;break;}
    if(!handled)for(const auto& [name,function]:math_float_binary)if(symbol==name){float_value=function(input_float(0),input_float(1));single=handled=true;break;}
    if(!handled && (symbol=="fmaf" || symbol=="frexpf" || symbol=="modff" || symbol=="ldexpf" || symbol=="scalbnf")){
        const auto f=input_float(0);single=handled=true;
        if(symbol=="fmaf")float_value=std::fma(f,input_float(1),input_float(2));
        else if(symbol=="frexpf"){int exponent=0;float_value=std::frexp(f,&exponent);memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(exponent));}
        else if(symbol=="modff"){float integral=0;float_value=std::modf(f,&integral);memory.store(GuestAddress{arg(1)},32,std::bit_cast<std::uint32_t>(integral));}
        else float_value=std::ldexp(f,std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(1))));
    }
    for (const auto& [name, function] : math_double_unary) if (symbol == name) { value = function(x); handled = true; break; }
    if (!handled) for (const auto& [name, function] : math_double_binary) if (symbol == name) { value = function(x,y); handled = true; break; }
    if(const auto* conversion=integer_rounding_operation(symbol)){
        integer=round_integer(conversion->single?static_cast<double>(input_float(0)):x,*conversion);
        floating=false;handled=true;
    }
    if (!handled) {
        if (symbol == "fma") value = std::fma(x,y,input_double(2));
        else if (symbol == "frexp") { int exponent = 0; value = std::frexp(x,&exponent); memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(exponent)); }
        else if (symbol == "modf") { double integral = 0; value = std::modf(x,&integral); memory.store(GuestAddress{arg(1)},64,std::bit_cast<std::uint64_t>(integral)); }
        else if (symbol == "ldexp" || symbol == "scalbn") value = std::ldexp(x,std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(1))));
        else if (symbol == "_finite" || symbol == "_isnan") { floating = false; integer = symbol == "_finite" ? std::isfinite(x) : std::isnan(x); }
        else if (symbol == "_j0") value = ::j0(x);
        else if (symbol == "_j1") value = ::j1(x);
        else if (symbol == "_y0") value = ::y0(x);
        else if (symbol == "_y1") value = ::y1(x);
        else if (symbol == "_jn") value = ::jn(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(0))),y);
        else if (symbol == "_yn") value = ::yn(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(0))),y);
        else {
            const bool wide=symbol=="wcstod" || symbol=="wcstof" || symbol=="_wtof";
            if(wide && !arg(0)){
                memory.store(GuestAddress{runtime_.errno_cell},32,22);
                conversion_result(state,x86_,0,symbol=="wcstof");
                if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,0);return std::nullopt;}
                return 0xc0000417U;
            }
            // C locale 的數值語法為 ASCII；保留每個 UTF-16 單位對應一個位置，非 ASCII 即終止掃描。
            std::string text;
            if(wide){for(const auto c:read_utf16(memory,GuestAddress{arg(0)}))text.push_back(c<128?static_cast<char>(c):'\x7f');}
            else text=read_ansi(memory,arg(0));
            char* end = nullptr;
            const bool plain = symbol == "atof" || symbol=="_wtof" || symbol == "atoi" || symbol == "atol" || symbol == "_atoi64";
            if (!plain && arg(1)) memory.check(GuestAddress{arg(1)},x86_?4:8,Access::Write);
            if (symbol == "strtof" || symbol=="wcstof") {float_value=std::strtof(text.c_str(),&end);single=true;}
            else if (symbol == "strtod" || symbol == "atof" || symbol=="wcstod" || symbol=="_wtof") value = std::strtod(text.c_str(),&end);
            else {
                floating = false;
                const int base = plain ? 10 : std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(2)));
                if (base != 0 && (base < 2 || base > 36)) { errno = EINVAL; end = const_cast<char*>(text.c_str()); }
                else if (symbol == "strtoul" || symbol == "strtoull" || symbol == "_strtoui64") {
                    integer = std::strtoull(text.c_str(),&end,base);
                    // Windows long 固定為 32 bits，宿主 LP64 不可直接沿用。
                    if (symbol == "strtoul") {
                        const auto first = text.find_first_not_of(" \t\r\n\f\v");
                        const bool negative = first != std::string::npos && text[first] == '-';
                        const auto magnitude = negative && errno != ERANGE ? UINT64_C(0) - integer : integer;
                        if (magnitude > UINT32_MAX) { integer = UINT32_MAX; errno = ERANGE; }
                        else integer &= UINT32_MAX;
                    }
                } else {
                    auto result = std::strtoll(text.c_str(),&end,base);
                    if (symbol == "strtol" || symbol == "atoi" || symbol == "atol") {
                        if (result < INT32_MIN) { result = INT32_MIN; errno = ERANGE; }
                        if (result > INT32_MAX) { result = INT32_MAX; errno = ERANGE; }
                        integer = static_cast<std::uint32_t>(result);
                    } else integer = static_cast<std::uint64_t>(result);
                }
            }
            if (!plain && arg(1)) memory.store(GuestAddress{arg(1)},x86_?32:64,arg(0)+static_cast<std::uint64_t>(end-text.c_str())*(wide?2U:1U));
        }
    }
    commit_math_environment(state,memory,runtime_.errno_cell,x86_,environment);
    if(floating)conversion_result(state,x86_,single?static_cast<double>(float_value):value,single);
    if(x86_ && (symbol=="strtoll" || symbol=="strtoull" || symbol=="_strtoi64" || symbol=="_strtoui64" || symbol=="_atoi64" || symbol.starts_with("llrint") || symbol.starts_with("llround")))state.reg(Gpr::Rdx)=static_cast<std::uint32_t>(integer>>32);
    arg.return_to(state,integer); return std::nullopt;
}
}
