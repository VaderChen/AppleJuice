#include "x64arm/windows_services.hpp"
#include "icu_bidi_guest.hpp"
#include "icu_enumeration_guest.hpp"
#include "icu_guest_abi.hpp"
#include "icu_string_guest.hpp"
#include "icu_text_guest.hpp"
#include "service_callback_task.hpp"
#include <unicode/uenum.h>

namespace x64arm {
std::span<const WindowsServiceDescriptor> icu_bidi_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_BIDI_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#include "icu_bidi_services.inc"
#include "icu_bidi_transform_services.inc"
#undef ICU_BIDI_API
    };
    return entries;
}

std::span<const WindowsServiceDescriptor> icu_string_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_STRING_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#include "icu_string_services.inc"
#undef ICU_STRING_API
    };
    return entries;
}

std::span<const WindowsServiceDescriptor> icu_text_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_TEXT_API(name,count,wide) {"icu.dll",#name,count,wide,0,0,true},
#include "icu_text_services.inc"
#undef ICU_TEXT_API
    };
    return entries;
}

std::span<const WindowsServiceDescriptor> icu_enumeration_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_ENUM_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#include "icu_enumeration_services.inc"
#undef ICU_ENUM_API
    };
    return entries;
}

service_callback::Task<std::uint64_t> WindowsServices::export_icu_enumeration(
        GuestState& state,GuestMemory& memory,UEnumeration& input,icu_abi::IcuStatus& status){
    std::string values;
    std::int32_t count{};
    std::int32_t length{};
    while(const auto* value=uenum_next(&input,&length,&status.value)){
        values.append(value,static_cast<std::size_t>(length));
        values.push_back('\0');
        ++count;
    }
    if(U_FAILURE(status.value)){status.commit();co_return 0;}
    if(values.size()>UINT32_MAX){status.value=U_MEMORY_ALLOCATION_ERROR;status.commit();co_return 0;}
    GuestScratch scratch(*this,memory,state);
    const auto bytes=values.empty()?0:scratch.allocate(values.size());
    if(!bytes && !values.empty()){
        status.value=U_MEMORY_ALLOCATION_ERROR;status.commit();co_return 0;
    }
    if(bytes)memory.write(GuestAddress{bytes},icu_abi::icu_bytes(values.data(),values.size()));
    const auto base=icu_enumeration::image_address(x86_);
    const auto target=base+runtime_.builtin_images.at(base).export_names.at("x64arm_uenum_open_owned");
    status.commit();
    co_return co_await service_callback::call_value(target,{bytes,values.size(),static_cast<std::uint64_t>(count),status.address.value});
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_guest_text(GuestState& state,GuestMemory&,
        const WindowsServiceDescriptor& service){
    auto base=icu_string::image_address(x86_);
    if(service.symbol.starts_with("ubidi") || service.symbol=="u_shapeArabic")base=icu_bidi::image_address(x86_);
    else if(service.symbol.starts_with("uenum_"))base=icu_enumeration::image_address(x86_);
    else if(service.symbol.starts_with("utext_"))base=icu_text::image_address(x86_);
    const auto& image=runtime_.builtin_images.at(base);
    // 服務追蹤沿用原客體呼叫框架，包含寬索引、指標別名與 provider 回呼。
    state.rip=GuestAddress{base+image.export_names.at(std::string(service.symbol))};
    return {};
}
}
