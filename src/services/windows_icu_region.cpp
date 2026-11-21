#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_region_guest.hpp"
#include "icu_region_layout.hpp"
#include "service_callback_task.hpp"
#include <unicode/uregion.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using namespace icu_region;

constexpr std::array<std::string_view,count> canonical_codes{
#define ICU_REGION_LAYOUT(...)
#define ICU_REGION_ENTRY(code,...) code,
#define ICU_REGION_CODE(...)
#define ICU_REGION_NUMBER(...)
#include "guest_icu_region_catalog.inc"
#undef ICU_REGION_NUMBER
#undef ICU_REGION_CODE
#undef ICU_REGION_ENTRY
#undef ICU_REGION_LAYOUT
};

bool region_type_valid(int32_t type){return type>=URGN_UNKNOWN && type<=URGN_DEPRECATED;}

const URegion* region_provider(std::uint64_t base,std::uint64_t address){
    if(address<base || (address-base)%sizeof(View))return nullptr;
    const auto index=(address-base)/sizeof(View);
    if(index>=canonical_codes.size())return nullptr;
    UErrorCode status=U_ZERO_ERROR;
    const auto* region=uregion_getRegionFromCode(canonical_codes[index].data(),&status);
    return U_SUCCESS(status)?region:nullptr;
}

std::uint64_t region_handle(std::uint64_t base,const URegion* region){
    if(!region)return 0;
    const std::string_view code=uregion_getRegionCode(region);
    const auto item=std::lower_bound(canonical_codes.begin(),canonical_codes.end(),code);
    if(item==canonical_codes.end() || *item!=code)throw HostCallFault("ICU 地區目錄與宿主資料不符");
    return base+static_cast<std::uint64_t>(item-canonical_codes.begin())*sizeof(View);
}
}

std::span<const WindowsServiceDescriptor> icu_region_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_REGION_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#define ICU_REGION_QUERY ICU_REGION_API
#include "icu_region_services.inc"
#undef ICU_REGION_QUERY
#undef ICU_REGION_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_region(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const auto base=icu_region::image_address(x86_);
    const auto catalog=base+runtime_.builtin_images.at(base).export_names.at("x64arm_region_catalog");
    const auto name=service.symbol;
    if(name=="uregion_getAvailable" || name=="uregion_getContainedRegions" ||
       name=="uregion_getContainedRegionsOfType" || name=="uregion_getPreferredValues")
        return nested_continuation(state,memory,"ICU 地區列舉",
            [this,service,catalog](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                IcuStatus status(m,call(service.arguments-1));
                std::unique_ptr<UEnumeration,decltype(&uenum_close)> regions(nullptr,uenum_close);
                if(U_SUCCESS(status.value)){
                    if(service.symbol=="uregion_getAvailable"){
                        const auto type=signed_argument(call(0));
                        if(region_type_valid(type))regions.reset(uregion_getAvailable(static_cast<URegionType>(type),&status.value));
                        else status.value=U_ILLEGAL_ARGUMENT_ERROR;
                    }else if(const auto* region=region_provider(catalog,call(0))){
                        if(service.symbol=="uregion_getContainedRegions")regions.reset(uregion_getContainedRegions(region,&status.value));
                        else if(service.symbol=="uregion_getPreferredValues")regions.reset(uregion_getPreferredValues(region,&status.value));
                        else{
                            const auto type=signed_argument(call(1));
                            if(region_type_valid(type))regions.reset(uregion_getContainedRegionsOfType(region,static_cast<URegionType>(type),&status.value));
                            else status.value=U_ILLEGAL_ARGUMENT_ERROR;
                        }
                    }else status.value=U_ILLEGAL_ARGUMENT_ERROR;
                }
                const auto handle=regions?co_await export_icu_enumeration(s,m,*regions,status):0;
                status.commit();
                call.return_to(s,handle);
                co_return std::nullopt;
            });

    const WindowsCall call(state,memory,service);
    const auto finish=[&](std::uint64_t result){
        call.return_to(state,result);
        return std::optional<std::uint32_t>{};
    };
    if(name=="uregion_getRegionFromCode" || name=="uregion_getRegionFromNumericCode"){
        IcuStatus status(memory,call(1));
        if(U_FAILURE(status.value))return finish(0);
        const URegion* region=nullptr;
        if(name=="uregion_getRegionFromCode"){
            char code[4]{};
            if(call(0))for(std::size_t n=0;n<sizeof(code);++n){
                code[n]=static_cast<char>(memory.load(GuestAddress{call(0)+n},8));
                if(!code[n])break;
            }
            if(code[3])status.value=U_ILLEGAL_ARGUMENT_ERROR;
            else region=uregion_getRegionFromCode(call(0)?code:nullptr,&status.value);
        }else region=uregion_getRegionFromNumericCode(signed_argument(call(0)),&status.value);
        status.commit();
        return finish(U_SUCCESS(status.value)?region_handle(catalog,region):0);
    }
    if(name=="uregion_areEqual")return finish(call(0)==call(1));
    const auto* region=region_provider(catalog,call(0));
    if(!region)return finish(name=="uregion_getNumericCode"?UINT32_MAX:0);
    if(name=="uregion_getRegionCode")return finish(call(0)+offsetof(icu_region::View,code));
    if(name=="uregion_getNumericCode")return finish(static_cast<std::uint32_t>(uregion_getNumericCode(region)));
    if(name=="uregion_getType")return finish(uregion_getType(region));
    if(name=="uregion_getContainingRegion")return finish(region_handle(catalog,uregion_getContainingRegion(region)));
    if(name=="uregion_getContainingRegionOfType"){
        const auto type=signed_argument(call(1));
        return finish(region_type_valid(type)?region_handle(catalog,uregion_getContainingRegionOfType(region,static_cast<URegionType>(type))):0);
    }
    if(name=="uregion_contains"){
        const auto* other=region_provider(catalog,call(1));
        return finish(other && uregion_contains(region,other));
    }
    throw HostCallFault("ICU 地區資料服務未登錄");
}
}
