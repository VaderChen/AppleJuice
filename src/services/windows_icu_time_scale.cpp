#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_time_scale_math.hpp"

namespace x64arm {
std::span<const WindowsServiceDescriptor> icu_time_scale_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_TIME_SCALE(name,wide) {"icu.dll",#name,3,wide,0,0,true},
#include "icu_time_scale_services.inc"
#undef ICU_TIME_SCALE
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_time_scale(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    using namespace icu_abi;
    const WindowsCall call(state,memory,service);
    IcuStatus status(memory,call(2));
    const auto original=status.value;
    const auto result=service.wide_arguments?
        icu_time_scale::convert(std::bit_cast<std::int64_t>(call(0)),signed_argument(call(1)),&status.value,
                                service.symbol=="utmscale_toInt64"):
        icu_time_scale::value(signed_argument(call(0)),signed_argument(call(1)),&status.value);
    if(status.value!=original)status.commit();
    call.return_to(state,std::bit_cast<std::uint64_t>(result),true);
    return std::nullopt;
}
}
