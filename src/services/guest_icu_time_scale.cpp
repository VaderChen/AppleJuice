#include "icu_time_scale_math.hpp"

using namespace x64arm::icu_time_scale;

U_CAPI int64_t U_EXPORT2 utmscale_getTimeScaleValue(UDateTimeScale scale,UTimeScaleValue field,UErrorCode* status){
    return value(static_cast<int>(scale),static_cast<int>(field),status);
}
U_CAPI int64_t U_EXPORT2 utmscale_fromInt64(int64_t time,UDateTimeScale scale,UErrorCode* status){
    return convert(time,static_cast<int>(scale),status,false);
}
U_CAPI int64_t U_EXPORT2 utmscale_toInt64(int64_t time,UDateTimeScale scale,UErrorCode* status){
    return convert(time,static_cast<int>(scale),status,true);
}
