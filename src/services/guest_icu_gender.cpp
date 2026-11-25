#include "icu_gender_layout.hpp"
using namespace x64arm::icu_gender;

extern "C" const Style x64arm_gender_styles[count]={
    Style::Neutral,Style::MixedNeutral,Style::MaleTaints
};

U_CAPI UGender U_EXPORT2 ugender_getListGender(const UGenderInfo* info,const UGender* genders,
                                                 int32_t length,UErrorCode* status){
    if(!status || U_FAILURE(*status))return UGENDER_OTHER;
    const auto style=style_index(reinterpret_cast<uintptr_t>(x64arm_gender_styles),reinterpret_cast<uintptr_t>(info));
    if(style==count || length<0 || (length>0 && !genders)){
        *status=U_ILLEGAL_ARGUMENT_ERROR;
        return UGENDER_OTHER;
    }
    return static_cast<UGender>(list_gender(static_cast<Style>(style),length,
                                        [&](int32_t i){return static_cast<int32_t>(genders[i]);}));
}
