#include "icu_date_pattern_layout.hpp"
#include <unicode/udatpg.h>

namespace {
using namespace x64arm::icu_date_pattern;
static_assert(UDATPG_ZONE_FIELD+1==field_count);

const GuestPatternView& view(const UDateTimePatternGenerator* generator){
    return *reinterpret_cast<const GuestPatternView*>(generator);
}

const UChar* value(const UDateTimePatternGenerator* generator,unsigned index,int32_t* length){
    if(!generator)return nullptr;
    const auto& result=view(generator).values[index];
    if(length)*length=result.length;
    return reinterpret_cast<const UChar*>(result.address);
}

unsigned field_index(UDateTimePatternField field){
    return static_cast<unsigned>(field);
}
}

extern "C" const UChar* udatpg_getDecimal(const UDateTimePatternGenerator* generator,int32_t* length){
    return value(generator,decimal_index,length);
}

extern "C" const UChar* udatpg_getDateTimeFormat(const UDateTimePatternGenerator* generator,int32_t* length){
    return value(generator,date_time_index,length);
}

extern "C" const UChar* udatpg_getAppendItemName(const UDateTimePatternGenerator* generator,UDateTimePatternField field,int32_t* length){
    const auto offset=field_index(field);
    return offset<field_count?value(generator,name_index+offset,length):nullptr;
}

extern "C" const UChar* udatpg_getAppendItemFormat(const UDateTimePatternGenerator* generator,UDateTimePatternField field,int32_t* length){
    const auto offset=field_index(field);
    return offset<field_count?value(generator,format_index+offset,length):nullptr;
}

extern "C" UDateFormatHourCycle udatpg_getDefaultHourCycle(const UDateTimePatternGenerator* generator,UErrorCode* status){
    if(!status || U_FAILURE(*status))return UDAT_HOUR_CYCLE_23;
    if(!generator){*status=U_ILLEGAL_ARGUMENT_ERROR;return UDAT_HOUR_CYCLE_23;}
    const auto& data=view(generator);
    if(data.hour_status)*status=static_cast<UErrorCode>(data.hour_status);
    return static_cast<UDateFormatHourCycle>(data.hour_cycle);
}
