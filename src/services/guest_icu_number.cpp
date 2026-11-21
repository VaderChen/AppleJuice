#include "guest_icu_locale_catalog.hpp"
#include "icu_number_layout.hpp"
#include <unicode/ufieldpositer.h>
#include <unicode/uformattable.h>
#include <unicode/unum.h>

constexpr auto locale_count=static_cast<int32_t>(
    sizeof(x64arm::icu_locale::available_locales)/sizeof(*x64arm::icu_locale::available_locales));

extern "C" int32_t unum_countAvailable(){
    return locale_count;
}

extern "C" const char* unum_getAvailable(int32_t index){
    return index>=0 && index<locale_count?x64arm::icu_locale::available_locales[index]:nullptr;
}

extern "C" int32_t ufieldpositer_next(UFieldPositionIterator* iterator,int32_t* begin,int32_t* end){
    if(!iterator)return -1;
    using namespace x64arm::icu_number;
    auto& view=*reinterpret_cast<FieldIteratorView*>(iterator);
    if(view.cursor<0 || view.cursor>=view.count || !view.records)return -1;
    const auto& field=reinterpret_cast<const FieldPosition*>(view.records)[view.cursor++];
    if(begin)*begin=field.begin;
    if(end)*end=field.end;
    return field.field;
}

extern "C" UBool ufmt_isNumeric(const UFormattable* value){
    return value && reinterpret_cast<const x64arm::icu_number::FormattableView*>(value)->numeric!=0;
}

extern "C" UFormattableType ufmt_getType(const UFormattable* value,UErrorCode* status){
    if(!status || U_FAILURE(*status))return UFMT_COUNT;
    if(!value){*status=U_ILLEGAL_ARGUMENT_ERROR;return UFMT_COUNT;}
    return static_cast<UFormattableType>(reinterpret_cast<const x64arm::icu_number::FormattableView*>(value)->type);
}
