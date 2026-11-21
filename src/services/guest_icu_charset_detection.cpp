#include "icu_charset_detection_layout.hpp"

namespace {
using x64arm::icu_charset_detection::Detector;
using x64arm::icu_charset_detection::Match;

const Match* checked(const UCharsetMatch* handle,UErrorCode* status){
    if(!status || U_FAILURE(*status))return nullptr;
    const auto* match=reinterpret_cast<const Match*>(handle);
    if(match && match->magic==Match::signature)return match;
    *status=U_ILLEGAL_ARGUMENT_ERROR;
    return nullptr;
}
}

extern "C" const char* ucsdet_getName(const UCharsetMatch* handle,UErrorCode* status){
    const auto* match=checked(handle,status);
    return match?reinterpret_cast<const char*>(static_cast<uintptr_t>(match->name)):nullptr;
}

extern "C" const char* ucsdet_getLanguage(const UCharsetMatch* handle,UErrorCode* status){
    const auto* match=checked(handle,status);
    return match?reinterpret_cast<const char*>(static_cast<uintptr_t>(match->language)):nullptr;
}

extern "C" int32_t ucsdet_getConfidence(const UCharsetMatch* handle,UErrorCode* status){
    const auto* match=checked(handle,status);
    return match?match->confidence:0;
}

extern "C" UBool ucsdet_isInputFilterEnabled(const UCharsetDetector* handle){
    const auto* detector=reinterpret_cast<const Detector*>(handle);
    return detector && detector->magic==Detector::signature?detector->filter:0;
}

extern "C" UBool ucsdet_enableInputFilter(UCharsetDetector* handle,UBool enabled){
    auto* detector=reinterpret_cast<Detector*>(handle);
    return detector && detector->magic==Detector::signature?detector->enable_filter(enabled):0;
}
