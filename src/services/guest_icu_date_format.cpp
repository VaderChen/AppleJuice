#include "guest_icu_date_format_fields.hpp"
#include "guest_icu_locale_catalog.hpp"
#include <stdint.h>
#include <unicode/udat.h>

constexpr auto locale_count=static_cast<int32_t>(
    sizeof(x64arm::icu_locale::available_locales)/sizeof(*x64arm::icu_locale::available_locales));

extern "C" int32_t udat_countAvailable(){
    return locale_count;
}

extern "C" const char* udat_getAvailable(int32_t index){
    return index>=0 && index<locale_count?x64arm::icu_locale::available_locales[index]:nullptr;
}

extern "C" UBool udat_isLenient(const UDateFormat* formatter){
    return formatter && *reinterpret_cast<const uint32_t*>(formatter)!=0;
}

extern "C" UCalendarDateFields udat_toCalendarDateField(UDateFormatField field){
    using namespace x64arm::icu_date_format;
    const auto index=static_cast<unsigned>(field);
    return index<sizeof(calendar_fields)/sizeof(*calendar_fields)?
        static_cast<UCalendarDateFields>(calendar_fields[index]):UCAL_FIELD_COUNT;
}
