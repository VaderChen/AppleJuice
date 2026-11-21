#include "guest_icu_locale_catalog.hpp"
#include <unicode/uloc.h>

using namespace x64arm::icu_locale;
constexpr auto locale_count=static_cast<int32_t>(sizeof(available_locales)/sizeof(*available_locales));

extern "C" int32_t uloc_countAvailable(){
    return locale_count;
}

extern "C" const char* uloc_getAvailable(int32_t index){
    return index>=0 && index<locale_count?available_locales[index]:nullptr;
}

extern "C" const char* const* uloc_getISOCountries(){
    return iso_countries;
}

extern "C" const char* const* uloc_getISOLanguages(){
    return iso_languages;
}
