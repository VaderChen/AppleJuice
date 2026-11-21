#include "icu_formatted_layout.hpp"
#include "icu_modern_number_layout.hpp"

namespace {
using namespace x64arm::icu_formatted;

template<class View,class Handle>
auto checked(Handle* handle,UErrorCode* error){
    using Pointer=decltype(static_cast<View*>(nullptr));
    if(!error || U_FAILURE(*error))return Pointer{};
    auto* view=reinterpret_cast<Pointer>(handle);
    if(view && view->magic==View::signature)return view;
    *error=U_ILLEGAL_ARGUMENT_ERROR;
    return Pointer{};
}
}

extern "C" void ucfpos_reset(UConstrainedFieldPosition* position,UErrorCode* error){
    if(auto* view=checked<Position>(position,error))*view=Position{};
}

extern "C" void ucfpos_constrainCategory(UConstrainedFieldPosition* position,int32_t category,UErrorCode* error){
    if(auto* view=checked<Position>(position,error))view->select(category);
}

extern "C" void ucfpos_constrainField(UConstrainedFieldPosition* position,int32_t category,int32_t field,UErrorCode* error){
    if(auto* view=checked<Position>(position,error))view->select(category,field);
}

extern "C" int32_t ucfpos_getCategory(const UConstrainedFieldPosition* position,UErrorCode* error){
    const auto* view=checked<const Position>(position,error);
    return view?view->category:UFIELD_CATEGORY_UNDEFINED;
}

extern "C" int32_t ucfpos_getField(const UConstrainedFieldPosition* position,UErrorCode* error){
    const auto* view=checked<const Position>(position,error);
    return view?view->field:0;
}

extern "C" void ucfpos_getIndexes(const UConstrainedFieldPosition* position,int32_t* start,int32_t* limit,UErrorCode* error){
    if(const auto* view=checked<const Position>(position,error)){
        *start=view->start;
        *limit=view->limit;
    }
}

extern "C" int64_t ucfpos_getInt64IterationContext(const UConstrainedFieldPosition* position,UErrorCode* error){
    const auto* view=checked<const Position>(position,error);
    return view?view->context:0;
}

extern "C" void ucfpos_setInt64IterationContext(UConstrainedFieldPosition* position,int64_t context,UErrorCode* error){
    if(auto* view=checked<Position>(position,error))view->context=context;
}

extern "C" UBool ucfpos_matchesField(const UConstrainedFieldPosition* position,int32_t category,int32_t field,UErrorCode* error){
    const auto* view=checked<const Position>(position,error);
    return view && view->matches(category,field);
}

extern "C" void ucfpos_setState(UConstrainedFieldPosition* position,int32_t category,int32_t field,
        int32_t start,int32_t limit,UErrorCode* error){
    if(auto* view=checked<Position>(position,error))view->set(category,field,start,limit);
}

extern "C" const UChar* ufmtval_getString(const UFormattedValue* value,int32_t* length,UErrorCode* error){
    const auto* view=checked<const Value>(value,error);
    if(!view)return nullptr;
    if(view->error!=U_ZERO_ERROR)*error=view->error;
    if(U_FAILURE(*error))return nullptr;
    if(length)*length=view->length;
    return reinterpret_cast<const UChar*>(static_cast<uintptr_t>(view->text));
}

extern "C" UNumberRangeIdentityResult unumrf_resultGetIdentityResult(
        const UFormattedNumberRange* result,UErrorCode* error){
    const auto* view=checked<const x64arm::icu_modern_number::RangeView>(result,error);
    return view?view->identity:UNUM_IDENTITY_RESULT_COUNT;
}
