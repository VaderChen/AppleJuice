#include "guest_icu_calendar_version.hpp"
#include "guest_icu_locale_catalog.hpp"
#include "icu_calendar_layout.hpp"
#include <unicode/ucal.h>

static_assert(UCAL_LENIENT==0 && UCAL_SKIPPED_WALL_TIME+1==x64arm::icu_calendar::attribute_count);
constexpr auto locale_count=static_cast<int32_t>(
    sizeof(x64arm::icu_locale::available_locales)/sizeof(*x64arm::icu_locale::available_locales));

extern "C" int32_t ucal_countAvailable(){
    return locale_count;
}

extern "C" const char* ucal_getAvailable(int32_t index){
    return index>=0 && index<locale_count?x64arm::icu_locale::available_locales[index]:nullptr;
}

extern "C" const char* ucal_getTZDataVersion(UErrorCode* status){
    return status && U_SUCCESS(*status)?x64arm::icu_calendar::zone_data_version:nullptr;
}

extern "C" int32_t ucal_getAttribute(const UCalendar* calendar,UCalendarAttribute attribute){
    using namespace x64arm::icu_calendar;
    if(!calendar || static_cast<unsigned>(attribute)>=attribute_count)return -1;
    return reinterpret_cast<const GuestCalendarView*>(calendar)->attributes[attribute];
}

extern "C" const char* ucal_getType(const UCalendar* calendar,UErrorCode* status){
    if(!status || U_FAILURE(*status))return nullptr;
    if(!calendar){*status=U_ILLEGAL_ARGUMENT_ERROR;return nullptr;}
    const auto& view=*reinterpret_cast<const x64arm::icu_calendar::GuestCalendarView*>(calendar);
    return reinterpret_cast<const char*>(view.type);
}
