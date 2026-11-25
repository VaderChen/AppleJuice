#include "icu_numbering_system_layout.hpp"
#include <unicode/unumsys.h>

using namespace x64arm::icu_numbering_system;
namespace {
const View* view(const UNumberingSystem* system){
    const auto* record=reinterpret_cast<const View*>(system);
    return record && record->magic==View::signature?record:nullptr;
}
}

U_CAPI const char* U_EXPORT2 unumsys_getName(const UNumberingSystem* system){
    const auto* record=view(system);
    return record?reinterpret_cast<const char*>(record+1):nullptr;
}

U_CAPI int32_t U_EXPORT2 unumsys_getRadix(const UNumberingSystem* system){
    const auto* record=view(system);
    return record?record->radix:0;
}

U_CAPI UBool U_EXPORT2 unumsys_isAlgorithmic(const UNumberingSystem* system){
    const auto* record=view(system);
    return record && record->algorithmic!=0;
}

U_CAPI int32_t U_EXPORT2 unumsys_getDescription(const UNumberingSystem* system,char16_t* output,
                                                  int32_t capacity,UErrorCode* status){
    if(!status || U_FAILURE(*status))return -1;
    const auto* record=view(system);
    if(!record){
        *status=U_ILLEGAL_ARGUMENT_ERROR;
        return -1;
    }
    const auto* text=reinterpret_cast<const char16_t*>(
        reinterpret_cast<const unsigned char*>(record)+record->description_offset);
    const auto length=record->description_length;
    const auto before=*status;
    auto updated=before;
    const auto plan=plan_description(length,capacity,output!=nullptr,updated);
    for(int32_t i=0;i<plan.units;++i)output[i]=text[i];
    if(plan.terminate)output[plan.units]=0;
    if(updated!=before)*status=updated;
    return length;
}
