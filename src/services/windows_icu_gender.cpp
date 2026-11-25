#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_gender_guest.hpp"
#include "icu_gender_layout.hpp"

namespace x64arm {
std::span<const WindowsServiceDescriptor> icu_gender_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_GENDER_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#define ICU_GENDER_QUERY(name,count) {"icu.dll",#name,count,0,0,0,true},
#include "icu_gender_services.inc"
#undef ICU_GENDER_QUERY
#undef ICU_GENDER_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_gender(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    using namespace icu_abi;
    using namespace icu_gender;
    const WindowsCall call(state,memory,service);
    const auto open=service.symbol=="ugender_getInstance";
    IcuStatus status(memory,call(service.arguments-1));
    const auto before=status.value;
    const auto finish=[&](std::uint64_t value){
        if(status.value!=before)status.commit();
        call.return_to(state,value);
        return std::optional<std::uint32_t>{};
    };
    if(U_FAILURE(before))return finish(open?0U:UGENDER_OTHER);

    const auto base=image_address(x86_);
    const auto styles=base+runtime_.builtin_images.at(base).export_names.at("x64arm_gender_styles");
    if(open){
        const auto address=call(0);
        const auto locale=address?read_ansi(memory,address):std::string{};
        const auto* info=ugender_getInstance(address?locale.c_str():icu_default_locale(),&status.value);
        if(!info || U_FAILURE(status.value))return finish(0);

        // ICU 的物件對外不透明。用公開查詢辨別規則，客體只取得對應的常駐物件。
        constexpr UGender all_male[]{UGENDER_MALE,UGENDER_MALE};
        constexpr UGender mixed[]{UGENDER_MALE,UGENDER_FEMALE};
        auto style=Style::Neutral;
        if(ugender_getListGender(info,all_male,2,&status.value)==UGENDER_MALE)
            style=ugender_getListGender(info,mixed,2,&status.value)==UGENDER_MALE?
                Style::MaleTaints:Style::MixedNeutral;
        if(U_FAILURE(status.value))return finish(0);
        return finish(styles+static_cast<std::uint32_t>(style)*sizeof(Style));
    }

    const auto index=style_index(styles,call(0));
    const auto genders=call(1);
    const auto length=signed_argument(call(2));
    if(index==count || length<0 || (length>0 && !genders)){
        status.value=U_ILLEGAL_ARGUMENT_ERROR;
        return finish(UGENDER_OTHER);
    }
    const auto result=list_gender(static_cast<Style>(index),length,[&](std::int32_t i){
        return signed_argument(memory.load(GuestAddress{genders+4*static_cast<std::uint64_t>(i)},32));
    });
    return finish(static_cast<std::uint32_t>(result));
}
}
