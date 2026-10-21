#pragma once
#include <charconv>
#include <cstdio>
#include <string>
#include <stdexcept>
#if defined(__APPLE__)
#include <locale.h>
#include <xlocale.h>
#endif
namespace x64arm {
// 浮點字元轉換不受宿主 locale 影響；Apple libc++ 尚無浮點 to_chars 時共用 C locale。
inline std::string crt_float_chars(double number,std::chars_format format,int precision){
    if(precision<0 || precision>1048576)throw std::bad_alloc{};
    std::string text(static_cast<std::size_t>(precision)+512,'\0');
#if defined(__APPLE__)
    static const auto locale=newlocale(LC_NUMERIC_MASK,"C",nullptr);if(!locale)throw std::bad_alloc{};
    const auto spec=format==std::chars_format::fixed?"%.*f":format==std::chars_format::scientific?"%.*e":"%.*g";
    const auto size=format==std::chars_format::hex?snprintf_l(text.data(),text.size(),locale,"%.*a",precision,number):snprintf_l(text.data(),text.size(),locale,spec,precision,number);
    if(size<0 || static_cast<std::size_t>(size)>=text.size())throw std::bad_alloc{};text.resize(static_cast<std::size_t>(size));
    if(format==std::chars_format::hex){const auto at=text.starts_with('-')?1U:0U;if(text.substr(at,2)=="0x")text.erase(at,2);}
#else
    const auto result=std::to_chars(text.data(),text.data()+text.size(),number,format,precision);
    if(result.ec!=std::errc{})throw std::bad_alloc{};text.resize(static_cast<std::size_t>(result.ptr-text.data()));
#endif
    return text;
}
}
