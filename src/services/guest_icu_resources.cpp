#include "icu_resources_layout.hpp"

using x64arm::icu_resources::View;
namespace {
const View* view(const UResourceBundle* bundle){
    const auto* record=reinterpret_cast<const View*>(bundle);
    return record && record->magic==View::signature?record:nullptr;
}
}

U_CAPI const char* U_EXPORT2 ures_getKey(const UResourceBundle* bundle){
    const auto* record=view(bundle);
    return record?reinterpret_cast<const char*>(static_cast<uintptr_t>(record->key)):nullptr;
}
U_CAPI int32_t U_EXPORT2 ures_getSize(const UResourceBundle* bundle){
    const auto* record=view(bundle);
    return record?record->size:0;
}
U_CAPI UResType U_EXPORT2 ures_getType(const UResourceBundle* bundle){
    const auto* record=view(bundle);
    return record?static_cast<UResType>(record->type):URES_NONE;
}
U_CAPI UBool U_EXPORT2 ures_hasNext(const UResourceBundle* bundle){
    const auto* record=view(bundle);
    return record && record->has_next!=0;
}
