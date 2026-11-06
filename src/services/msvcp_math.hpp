#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace x64arm::msvcp_detail {
// <complex> 使用的 _Cosh／_Sinh／_Exp／_Dtest。回傳碼即 MSVC 的 FP_ZERO 0、FP_INFINITE 1、FP_NAN 2、FP_NORMAL -1、FP_SUBNORMAL -2。
template<class T> std::int16_t float_class(T value){
    switch(std::fpclassify(value)){
    case FP_ZERO:return 0;
    case FP_INFINITE:return 1;
    case FP_NAN:return 2;
    case FP_SUBNORMAL:return -2;
    default:return -1;
    }
}
// x＝y·e^x·2^eoff。先把 2 的次方分離再調整尺度，中間值不會提前上溢或下溢。
template<class T> std::int16_t scaled_exp(T& x,T y,int eoff){
    constexpr auto infinity=std::numeric_limits<T>::infinity();
    constexpr double certain=sizeof(T)==4?229.0:1842.0;   // 指數最大編碼×0.9，超過必定上下溢。
    if(y==T{}){x=y;return 0;}
    if(std::isnan(x))return 2;
    if(static_cast<double>(x)<-certain){x=std::copysign(T{},y);return 0;}
    if(static_cast<double>(x)>certain){x=std::copysign(infinity,y);return 1;}
    constexpr double inverse_ln2=1.4426950408889634073599246810018921,ln2_high=22713.0/32768.0,ln2_low=1.4286068203094172321214581765680755e-6;
    const double value=x,scaled=value*inverse_ln2;
    const int power=static_cast<int>(scaled+(scaled<0?-0.5:0.5));
    const double reduced=(value-power*ln2_high)-power*ln2_low;
    x=static_cast<T>(std::scalbn(static_cast<double>(y)*std::exp(reduced),power+eoff));
    const auto code=float_class(x);
    if(!code)x=std::copysign(T{},y);else if(code==1)x=std::copysign(infinity,y);
    return code;
}
// e^-x 相對 e^x 小於一個最低有效位的門檻：(有效位數＋2)/2·ln2。
template<class T> inline constexpr T exponential_negligible=sizeof(T)==4?static_cast<T>(9.01091):static_cast<T>(18.7149);
// y·cosh(x)；range_error 對應 STL 在大引數路徑上下溢時設定的 ERANGE。
template<class T> T scaled_cosh(T x,T y,bool& range_error){
    if(std::isnan(x) || std::isinf(x))return x;
    if(x==T{} || y==T{})return y;
    x=std::fabs(x);
    if(x<exponential_negligible<T>){(void)scaled_exp(x,T{1},-1);return y*(x+static_cast<T>(0.25)/x);}
    const auto code=scaled_exp(x,y,-1);range_error=code==0 || code==1;
    return x;
}
// y·sinh(x)，正負號跟隨 x。
template<class T> T scaled_sinh(T x,T y,bool& range_error){
    if(std::isnan(x))return x;
    if(std::isinf(x))return y!=T{}?x:std::signbit(x)?-y:y;
    if(x==T{})return x*y;
    if(y==T{})return x<T{}?-y:y;
    const bool negative=x<T{};x=std::fabs(x);
    if(x<T{1})x=std::sinh(x)*y;
    else if(x<exponential_negligible<T>){(void)scaled_exp(x,T{1},-1);x=y*(x-static_cast<T>(0.25)/x);}
    else{const auto code=scaled_exp(x,y,-1);range_error=code==0 || code==1;}
    return negative?-x:x;
}
}
