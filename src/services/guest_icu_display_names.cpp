#include "icu_display_names_layout.hpp"

using namespace x64arm::icu_display_names;
namespace {
const View* view(const ULocaleDisplayNames* names){
    const auto* record=reinterpret_cast<const View*>(names);
    return record && record->magic==View::signature?record:nullptr;
}
}

U_CAPI const char* U_EXPORT2 uldn_getLocale(const ULocaleDisplayNames* names){
    const auto* record=view(names);
    return record?reinterpret_cast<const char*>(record+1):nullptr;
}

U_CAPI UDialectHandling U_EXPORT2 uldn_getDialectHandling(const ULocaleDisplayNames* names){
    const auto* record=view(names);
    return record?static_cast<UDialectHandling>(record->contexts.get(UDISPCTX_TYPE_DIALECT_HANDLING)):
                  ULDN_STANDARD_NAMES;
}

U_CAPI UDisplayContext U_EXPORT2 uldn_getContext(const ULocaleDisplayNames* names,UDisplayContextType type,
                                                    UErrorCode* status){
    if(!status || U_FAILURE(*status))return UDISPCTX_STANDARD_NAMES;
    const auto* record=view(names);
    if(!record){
        *status=U_ILLEGAL_ARGUMENT_ERROR;
        return UDISPCTX_STANDARD_NAMES;
    }
    return record->contexts.get(static_cast<int32_t>(type));
}
