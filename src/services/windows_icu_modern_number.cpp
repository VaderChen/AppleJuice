#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_modern_number_layout.hpp"
#include "icu_number_state.hpp"
#include "service_callback_task.hpp"
#include <new>
#include <unicode/unumberformatter.h>
#include <unicode/unumberrangeformatter.h>
#include <unicode/upluralrules.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using icu_modern_number::RangeView;

template<class T, auto Close>
struct CloseIcuNumber {
    void operator()(T* object)const{Close(object);}
};
template<class T, auto Close>
using NativeHandle=std::unique_ptr<T,CloseIcuNumber<T,Close>>;

template<class T, auto Close>
struct NumberResult {
    NativeHandle<T,Close> native;
    std::uint64_t value{};
};

template<class Map>
auto object_at(Map& objects,std::uint64_t handle){
    const auto it=objects.find(handle);
    return it==objects.end()?nullptr:&it->second;
}
}

struct IcuModernNumberState {
    std::map<std::uint64_t,NativeHandle<UNumberFormatter,unumf_close>> formats;
    std::map<std::uint64_t,NativeHandle<UNumberRangeFormatter,unumrf_close>> ranges;
    std::map<std::uint64_t,NativeHandle<UPluralRules,uplrules_close>> plurals;
    std::map<std::uint64_t,NumberResult<UFormattedNumber,unumf_closeResult>> results;
    std::map<std::uint64_t,NumberResult<UFormattedNumberRange,unumrf_closeResult>> range_results;
};

std::span<const WindowsServiceDescriptor> icu_modern_number_service_descriptors(){
    static constexpr WindowsServiceDescriptor services[]{
#define ICU_MODERN_NUM_API(name,args,wide,legacy) {"icu.dll",#name,args,wide,0,0,true},
#define ICU_MODERN_NUM_QUERY ICU_MODERN_NUM_API
#include "icu_modern_number_services.inc"
#undef ICU_MODERN_NUM_QUERY
#undef ICU_MODERN_NUM_API
    };
    return services;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_modern_number(
        GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value=0){
        call.return_to(state,value);
        return std::optional<std::uint32_t>{};
    };
    const auto i=[&](unsigned n){return signed_argument(call(n));};
    const auto double_at=[&](unsigned n){return icu_double_argument(state,call,x86_,n);};
    if(!icu_modern_number_)icu_modern_number_=std::make_shared<IcuModernNumberState>();
    auto& objects=*icu_modern_number_;
    const auto erase=[&](auto& map){
        if(map.erase(call(0)))(void)free_heap(memory,call(0),state);
        return finish();
    };
    const auto close_result=[&](auto& map){
        if(const auto* item=object_at(map,call(0)))release_icu_formatted_value(state,memory,item->value);
        return erase(map);
    };
    if(name=="unumf_close")return erase(objects.formats);
    if(name=="unumrf_close")return erase(objects.ranges);
    if(name=="uplrules_close")return erase(objects.plurals);
    if(name=="unumf_closeResult")return close_result(objects.results);
    if(name=="unumrf_closeResult")return close_result(objects.range_results);

    IcuStatus status(memory,call(service.arguments-1));
    const auto result=[&](std::uint64_t value=0){status.commit();return finish(value);};
    const auto invalid_return=name=="unumrf_resultGetIdentityResult"?
        static_cast<std::uint32_t>(UNUM_IDENTITY_RESULT_COUNT):0U;
    const auto illegal=[&]{status.value=U_ILLEGAL_ARGUMENT_ERROR;return result(invalid_return);};
    const bool opening_number=name.starts_with("unumf_openForSkeletonAndLocale");
    const bool opening_range=name=="unumrf_openForSkeletonWithCollapseAndIdentityFallback";
    if(U_FAILURE(status.value) && (!status.address.value || (!opening_number && !opening_range)))
        return finish(invalid_return);
    const auto adopt=[&](auto& map,auto item,std::size_t bytes=1U){
        const auto handle=allocate_heap(memory,bytes);
        if(handle){
            try{
                map.emplace(handle,std::move(item));
                return handle;
            }catch(const std::bad_alloc&){
                (void)free_heap(memory,handle,state);
            }
        }
        status.value=U_MEMORY_ALLOCATION_ERROR;
        return std::uint64_t{};
    };
    const auto as_value=[&](auto& item,auto convert){
        if(!item.value){
            const auto* provider=convert(item.native.get(),&status.value);
            if(provider && U_SUCCESS(status.value))
                item.value=adopt_icu_formatted_value(state,memory,provider,status);
        }
        return result(item.value);
    };
    const auto bridge_locale=[&](unsigned argument){
        return call(argument)?read_ansi(memory,call(argument),INT32_MAX):std::string{};
    };
    if(name=="unumf_openResult"){
        NumberResult<UFormattedNumber,unumf_closeResult> item;
        item.native.reset(unumf_openResult(&status.value));
        return result(item.native?adopt(objects.results,std::move(item)):0);
    }
    if(name=="unumrf_openResult"){
        NumberResult<UFormattedNumberRange,unumrf_closeResult> item;
        item.native.reset(unumrf_openResult(&status.value));
        const auto handle=item.native?adopt(objects.range_results,std::move(item),sizeof(RangeView)):0;
        if(handle)write_icu_record(memory,handle,RangeView{});
        return result(handle);
    }
    if(opening_number){
        auto argument_status=U_ZERO_ERROR;
        const IcuTextInput<UChar> skeleton(memory,call(0),i(1),argument_status);
        if(U_FAILURE(argument_status)){
            if(U_SUCCESS(status.value))status.value=argument_status;
            return result();
        }
        const auto locale=bridge_locale(2);
        const auto* locale_id=call(2)?locale.c_str():icu_default_locale();
        UParseError parse{};
        const auto parse_ptr=name.ends_with("WithError")?call(3):0;
        read_icu_record(memory,parse_ptr,parse);
        NativeHandle<UNumberFormatter,unumf_close> native(parse_ptr?
            unumf_openForSkeletonAndLocaleWithError(skeleton.get(),skeleton.length,locale_id,&parse,&status.value):
            unumf_openForSkeletonAndLocale(skeleton.get(),skeleton.length,locale_id,&status.value));
        write_icu_record(memory,parse_ptr,parse);
        return result(native?adopt(objects.formats,std::move(native)):0);
    }
    if(opening_range){
        auto argument_status=U_ZERO_ERROR;
        const IcuTextInput<UChar> skeleton(memory,call(0),i(1),argument_status);
        if(U_FAILURE(argument_status)){
            if(U_SUCCESS(status.value))status.value=argument_status;
            return result();
        }
        const auto locale=bridge_locale(4);
        const auto* locale_id=call(4)?locale.c_str():icu_default_locale();
        UParseError parse{};
        read_icu_record(memory,call(5),parse);
        NativeHandle<UNumberRangeFormatter,unumrf_close> native(unumrf_openForSkeletonWithCollapseAndIdentityFallback(
            skeleton.get(),skeleton.length,static_cast<UNumberRangeCollapse>(i(2)),
            static_cast<UNumberRangeIdentityFallback>(i(3)),locale_id,call(5)?&parse:nullptr,&status.value));
        write_icu_record(memory,call(5),parse);
        return result(native?adopt(objects.ranges,std::move(native)):0);
    }
    if(name=="uplrules_open" || name=="uplrules_openForType"){
        const auto locale=bridge_locale(0);
        const auto* locale_id=call(0)?locale.c_str():icu_default_locale();
        NativeHandle<UPluralRules,uplrules_close> native(name=="uplrules_open"?uplrules_open(locale_id,&status.value):
            uplrules_openForType(locale_id,static_cast<UPluralType>(i(1)),&status.value));
        return result(native?adopt(objects.plurals,std::move(native)):0);
    }
    if(name.starts_with("unumf_format")){
        const auto* format=object_at(objects.formats,call(0));
        const auto target_idx=name=="unumf_formatDecimal"?3U:2U;
        const auto* target=object_at(objects.results,call(target_idx));
        if(!format || !target)return illegal();
        if(name=="unumf_formatInt")
            unumf_formatInt(format->get(),std::bit_cast<std::int64_t>(call(1)),target->native.get(),&status.value);
        else if(name=="unumf_formatDouble")
            unumf_formatDouble(format->get(),double_at(1),target->native.get(),&status.value);
        else{
            const IcuTextInput<char> number(memory,call(1),i(2),status.value);
            if(U_FAILURE(status.value))return result();
            unumf_formatDecimal(format->get(),number.get(),number.length,target->native.get(),&status.value);
        }
        refresh_icu_formatted_value(state,memory,target->value,status);
        return result();
    }
    if(name.starts_with("unumrf_format")){
        const auto* format=object_at(objects.ranges,call(0));
        const auto target_idx=name=="unumrf_formatDecimalRange"?5U:3U;
        const auto* target=object_at(objects.range_results,call(target_idx));
        if(!format || !target)return illegal();
        if(name=="unumrf_formatDoubleRange")
            unumrf_formatDoubleRange(format->get(),double_at(1),double_at(2),target->native.get(),&status.value);
        else{
            const IcuTextInput<char> first(memory,call(1),i(2),status.value);
            const IcuTextInput<char> second(memory,call(3),i(4),status.value);
            if(U_FAILURE(status.value))return result();
            const char* first_data=first.get();
            const char* second_data=second.get();
            if(call(1)==call(3)){
                // ICU 使用來源指標相等判定捨入前相等。共用位址時沿用涵蓋較長輸入的副本。
                const auto extent=[](const IcuTextInput<char>& input){
                    return input.length<0?std::char_traits<char>::length(input.get()):static_cast<std::size_t>(input.length);
                };
                first_data=second_data=extent(first)<extent(second)?second_data:first_data;
            }
            unumrf_formatDecimalRange(format->get(),first_data,first.length,second_data,second.length,
                                      target->native.get(),&status.value);
        }
        auto cached_status=U_ZERO_ERROR;
        const auto identity=unumrf_resultGetIdentityResult(target->native.get(),&cached_status);
        write_icu_record(memory,call(target_idx),RangeView{RangeView::signature,identity});
        refresh_icu_formatted_value(state,memory,target->value,status);
        return result();
    }
    if(name.starts_with("unumf_result")){
        auto* item=object_at(objects.results,call(0));
        if(!item)return illegal();
        const auto* native=item->native.get();
        if(name=="unumf_resultAsValue")return as_value(*item,unumf_resultAsValue);
        if(name=="unumf_resultToString"){
            return finish(icu_preserving_output<UChar>(memory,call(1),i(2),status,[&](auto* out,auto length,auto* error){
                return unumf_resultToString(native,out,length,error);
            }));
        }
        if(name=="unumf_resultToDecimalNumber"){
            return finish(icu_preserving_output<char>(memory,call(1),i(2),status,[&](auto* out,auto length,auto* error){
                return unumf_resultToDecimalNumber(native,out,length,error);
            }));
        }
        if(!call(1))return illegal();
        if(name=="unumf_resultNextFieldPosition"){
            UFieldPosition position{};
            read_icu_record(memory,call(1),position);
            const auto more=unumf_resultNextFieldPosition(native,&position,&status.value);
            if(U_SUCCESS(status.value))write_icu_record(memory,call(1),position);
            return result(more!=0);
        }
        if(name=="unumf_resultGetAllFieldPositions"){
            if(!icu_number_ || !icu_number_->positions.contains(call(1)))return illegal();
            OwnedIcuFieldIterator positions(ufieldpositer_open(&status.value),ufieldpositer_close);
            if(positions){
                unumf_resultGetAllFieldPositions(native,positions.get(),&status.value);
                if(U_SUCCESS(status.value) && !publish_icu_field_positions(state,memory,call(1),*positions))
                    status.value=U_MEMORY_ALLOCATION_ERROR;
            }
            return result();
        }
    }
    if(name.starts_with("unumrf_result")){
        auto* item=object_at(objects.range_results,call(0));
        if(!item)return illegal();
        const auto* native=item->native.get();
        if(name=="unumrf_resultAsValue")return as_value(*item,unumrf_resultAsValue);
        if(name=="unumrf_resultGetIdentityResult")
            return result(unumrf_resultGetIdentityResult(native,&status.value));
        const auto decimal=name=="unumrf_resultGetFirstDecimalNumber"?
            unumrf_resultGetFirstDecimalNumber:unumrf_resultGetSecondDecimalNumber;
        return finish(icu_preserving_output<char>(memory,call(1),i(2),status,[&](auto* out,auto length,auto* error){
            return decimal(native,out,length,error);
        }));
    }
    if(name=="uplrules_getKeywords"){
        return nested_continuation(state,memory,"ICU 複數規則列舉",
            [this,service](GuestState& current,GuestMemory& guest)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall request(current,guest,service);
                IcuStatus error(guest,request(1));
                const auto finish_request=[&](std::uint64_t value){
                    request.return_to(current,value);
                    return std::optional<std::uint32_t>{};
                };
                if(U_FAILURE(error.value))co_return finish_request(0);
                const auto* native=object_at(icu_modern_number_->plurals,request(0));
                if(!native){
                    error.value=U_ILLEGAL_ARGUMENT_ERROR;
                    error.commit();
                    co_return finish_request(0);
                }
                NativeHandle<UEnumeration,uenum_close> words(uplrules_getKeywords(native->get(),&error.value));
                if(!words || U_FAILURE(error.value)){
                    error.commit();
                    co_return finish_request(0);
                }
                const auto handle=co_await export_icu_enumeration(current,guest,*words,error);
                co_return finish_request(handle);
            });
    }
    if(name.starts_with("uplrules_")){
        const auto* native=object_at(objects.plurals,call(0));
        if(!native)return illegal();
        const bool formatted=name=="uplrules_selectFormatted";
        const auto* item=formatted?object_at(objects.results,call(1)):nullptr;
        if(formatted && !item)return illegal();
        return finish(icu_preserving_output<UChar>(memory,call(2),i(3),status,[&](auto* out,auto length,auto* error){
            return formatted?uplrules_selectFormatted(native->get(),item->native.get(),out,length,error):
                             uplrules_select(native->get(),double_at(1),out,length,error);
        }));
    }
    throw HostCallFault("ICU 數字格式、數值區間及複數規則服務未登錄");
}
}
