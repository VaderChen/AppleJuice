#include "x64arm/windows_services.hpp"
#include "icu_date_pattern_layout.hpp"
#include "icu_guest_abi.hpp"
#include "service_callback_task.hpp"
#include <unicode/dtptngen.h>
#include <unicode/udatpg.h>
#include <unicode/uenum.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using namespace icu_date_pattern;
using OwnedGenerator=std::unique_ptr<UDateTimePatternGenerator,decltype(&udatpg_close)>;
using OwnedEnumeration=std::unique_ptr<UEnumeration,decltype(&uenum_close)>;

struct GuestPattern {
    std::uint64_t address{};
    std::int32_t length{};
};

struct PatternGenerator {
    OwnedGenerator native{nullptr,udatpg_close};
    std::array<GuestPattern,value_count> values{};
    std::map<std::u16string,std::uint64_t,std::less<>> patterns;
};

bool valid_field(std::int32_t field){
    return static_cast<unsigned>(field)<field_count;
}

unsigned value_index(std::string_view name,std::int32_t field){
    if(name.ends_with("Decimal"))return decimal_index;
    if(name.ends_with("DateTimeFormat"))return date_time_index;
    return (name.ends_with("Name")?name_index:format_index)+static_cast<unsigned>(field);
}

const UChar* native_value(const UDateTimePatternGenerator* generator,unsigned index,std::int32_t* length){
    if(index==decimal_index)return udatpg_getDecimal(generator,length);
    if(index==date_time_index)return udatpg_getDateTimeFormat(generator,length);
    if(index>=format_index)return udatpg_getAppendItemFormat(generator,static_cast<UDateTimePatternField>(index-format_index),length);
    return udatpg_getAppendItemName(generator,static_cast<UDateTimePatternField>(index-name_index),length);
}
}

struct IcuDatePatternState {
    std::map<std::uint64_t,PatternGenerator> generators;
};

std::span<const WindowsServiceDescriptor> icu_date_pattern_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_DPG_API(name,count,legacy) {"icu.dll",#name,count,0,0,0,true},
#define ICU_DPG_QUERY ICU_DPG_API
#include "icu_date_pattern_services.inc"
#undef ICU_DPG_QUERY
#undef ICU_DPG_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_date_pattern(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name=="udatpg_openSkeletons" || name=="udatpg_openBaseSkeletons"){
        return nested_continuation(state,memory,"ICU 日期格式列舉",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                const auto finish=[&](std::uint64_t result){call.return_to(s,result);return std::optional<std::uint32_t>{};};
                IcuStatus status(m,call(1));
                if(U_FAILURE(status.value))co_return finish(0);
                if(!icu_date_pattern_){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();co_return finish(0);}
                const auto found=icu_date_pattern_->generators.find(call(0));
                if(found==icu_date_pattern_->generators.end()){
                    status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();co_return finish(0);
                }
                const auto function=service.symbol=="udatpg_openSkeletons"?udatpg_openSkeletons:udatpg_openBaseSkeletons;
                OwnedEnumeration items(function(found->second.native.get(),&status.value),uenum_close);
                if(!items){status.commit();co_return finish(0);}
                co_return finish(co_await export_icu_enumeration(s,m,*items,status));
            });
    }

    const WindowsCall call(state,memory,service);
    const auto i=[&](unsigned n){return signed_argument(call(n));};
    const auto finish=[&](std::uint64_t result){call.return_to(state,result);return std::optional<std::uint32_t>{};};
    if(!icu_date_pattern_)icu_date_pattern_=std::make_shared<IcuDatePatternState>();
    auto& all=icu_date_pattern_->generators;
    const auto found=all.find(call(0));
    auto* generator=found==all.end()?nullptr:&found->second;
    auto* native=generator?generator->native.get():nullptr;
    const auto width=x86_?4U:8U;
    const auto bits=static_cast<std::uint8_t>(width*8);
    constexpr unsigned prefix=8;
    constexpr unsigned entry_prefix=8;
    const auto view_address=[width](std::uint64_t handle,unsigned index){return handle+prefix+index*(entry_prefix+width);};
    const auto copy_value=[&](const UChar* text,std::int32_t length)->GuestPattern{
        if(!text || length<0)return {};
        const auto size=static_cast<std::size_t>(length);
        const auto address=allocate_heap(memory,(size+1)*sizeof(UChar));
        if(!address)return {};
        if(size)memory.write(GuestAddress{address},icu_bytes(text,size));
        memory.store(GuestAddress{address+size*sizeof(UChar)},16,0);
        return {address,length};
    };
    const auto publish=[&](std::uint64_t handle,PatternGenerator& object,const UDateTimePatternGenerator* source,unsigned index){
        std::int32_t length{};
        const auto* text=native_value(source,index,&length);
        const auto next=copy_value(text,length);
        if(text && !next.address)return false;
        const auto offset=view_address(handle,index);
        memory.store(GuestAddress{offset},32,static_cast<std::uint32_t>(next.length));
        memory.store(GuestAddress{offset+entry_prefix},bits,next.address);
        if(const auto previous=std::exchange(object.values[index],next).address)(void)free_heap(memory,previous,state);
        return true;
    };
    const auto close_values=[&](PatternGenerator& object){
        for(const auto& item:object.values)if(item.address)(void)free_heap(memory,item.address,state);
        for(const auto& entry:object.patterns)(void)free_heap(memory,entry.second,state);
    };
    if(name=="udatpg_close"){
        if(generator){
            close_values(*generator);
            all.erase(found);
            (void)free_heap(memory,call(0),state);
        }
        return finish(0);
    }
    if(name.starts_with("udatpg_set")){
        if(!native)return finish(0);
        const bool append=name.starts_with("udatpg_setAppend");
        if(append && !valid_field(i(1)))return finish(0);
        UErrorCode status=U_ZERO_ERROR;
        const IcuTextInput<UChar> text(memory,call(append?2U:1U),i(append?3U:2U),status);
        if(U_FAILURE(status))return finish(0);
        OwnedGenerator updated(udatpg_clone(native,&status),udatpg_close);
        if(!updated || U_FAILURE(status))return finish(0);
        const auto index=value_index(name,append?i(1):0);
        if(index==decimal_index)udatpg_setDecimal(updated.get(),text.get(),text.length);
        else if(index==date_time_index)udatpg_setDateTimeFormat(updated.get(),text.get(),text.length);
        else if(index>=format_index)udatpg_setAppendItemFormat(updated.get(),static_cast<UDateTimePatternField>(i(1)),text.get(),text.length);
        else udatpg_setAppendItemName(updated.get(),static_cast<UDateTimePatternField>(i(1)),text.get(),text.length);
        if(publish(call(0),*generator,updated.get(),index))generator->native=std::move(updated);
        return finish(0);
    }
    if(name=="udatpg_getDecimal" || name=="udatpg_getDateTimeFormat" || name.starts_with("udatpg_getAppend")){
        if(!generator)return finish(0);
        const bool append=name.starts_with("udatpg_getAppend");
        if(append && !valid_field(i(1)))return finish(0);
        const auto& value=generator->values[value_index(name,append?i(1):0)];
        const auto length=call(append?2U:1U);
        if(length)memory.store(GuestAddress{length},32,static_cast<std::uint32_t>(value.length));
        return finish(value.address);
    }
    if(name=="udatpg_getPatternForSkeleton"){
        if(!native)return finish(0);
        UErrorCode status=U_ZERO_ERROR;
        const IcuTextInput<UChar> input(memory,call(1),i(2),status);
        if(U_FAILURE(status))return finish(0);
        std::int32_t length{};
        const auto* text=udatpg_getPatternForSkeleton(native,input.get(),input.length,&length);
        if(call(3))memory.store(GuestAddress{call(3)},32,static_cast<std::uint32_t>(length));
        if(!text || length<0)return finish(0);
        auto& cache=generator->patterns;
        const std::u16string_view key(text,static_cast<std::size_t>(length));
        if(const auto entry=cache.find(key);entry!=cache.end())return finish(entry->second);
        const auto value=copy_value(text,length);
        if(value.address)cache.emplace(key,value.address);
        return finish(value.address);
    }

    IcuStatus status(memory,call(service.arguments-1));
    if(name=="udatpg_getDefaultHourCycle"){
        if(U_FAILURE(status.value))return finish(UDAT_HOUR_CYCLE_23);
        if(!native){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(UDAT_HOUR_CYCLE_23);}
        const auto hour=udatpg_getDefaultHourCycle(native,&status.value);
        status.commit();return finish(static_cast<std::uint32_t>(hour));
    }
    if(U_FAILURE(status.value))return finish(name=="udatpg_getFieldDisplayName"?UINT32_MAX:0);
    if(name=="udatpg_open" || name=="udatpg_openEmpty" || name=="udatpg_clone"){
        OwnedGenerator selected(nullptr,udatpg_close);
        if(name=="udatpg_open"){
            const auto locale=call(0)?read_ansi(memory,call(0),INT32_MAX):std::string{};
            selected.reset(udatpg_open(call(0)?locale.c_str():icu_default_locale(),&status.value));
        }else if(name=="udatpg_openEmpty")selected.reset(udatpg_openEmpty(&status.value));
        else if(native)selected.reset(udatpg_clone(native,&status.value));
        else status.value=U_ILLEGAL_ARGUMENT_ERROR;
        std::uint64_t handle{};
        if(selected && U_SUCCESS(status.value)){
            handle=allocate_heap(memory,prefix+value_count*(entry_prefix+width));
            if(handle){
                PatternGenerator object;
                object.native=std::move(selected);
                UErrorCode hour_error=U_ZERO_ERROR;
                const auto hour=udatpg_getDefaultHourCycle(object.native.get(),&hour_error);
                memory.store(GuestAddress{handle},32,static_cast<std::uint32_t>(hour));
                memory.store(GuestAddress{handle+4},32,static_cast<std::uint32_t>(hour_error));
                for(unsigned index=0;index<value_count;++index){
                    if(!publish(handle,object,object.native.get(),index)){
                        close_values(object);
                        (void)free_heap(memory,handle,state);
                        handle=0;break;
                    }
                }
                if(handle)all.emplace(handle,std::move(object));
            }
            if(!handle)status.value=U_MEMORY_ALLOCATION_ERROR;
        }
        status.commit();return finish(handle);
    }
    const bool skeleton=name=="udatpg_getSkeleton" || name=="udatpg_getBaseSkeleton";
    if(!native && !skeleton){
        status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();
        return finish(name=="udatpg_getFieldDisplayName"?UINT32_MAX:0);
    }
    if(name=="udatpg_getFieldDisplayName"){
        if(!valid_field(i(1)) || i(2)<UDATPG_WIDE || i(2)>UDATPG_NARROW ||
           i(4)<0 || (!call(3) && i(4))){
            status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(UINT32_MAX);
        }
        UChar query_buffer{};
        return finish(icu_preserving_output<UChar>(memory,call(3),i(4),status,[&](auto* out,auto size,auto* error){
            return udatpg_getFieldDisplayName(native,static_cast<UDateTimePatternField>(i(1)),
                static_cast<UDateTimePGDisplayWidth>(i(2)),!out && call(3)?&query_buffer:out,size,error);
        }));
    }
    const IcuTextInput<UChar> input(memory,call(1),i(2),status.value);
    if(U_FAILURE(status.value)){status.commit();return finish(0);}
    if(name=="udatpg_addPattern"){
        auto* provider=reinterpret_cast<icu::DateTimePatternGenerator*>(native);
        const icu::UnicodeString pattern(input.length<0,input.get(),input.length);
        icu::UnicodeString conflicting;
        const auto result=provider->addPattern(pattern,static_cast<UBool>(call(3)),conflicting,status.value);
        const auto length=icu_preserving_output<UChar>(memory,call(4),i(5),status,
            [&](auto* out,auto size,auto* error){return conflicting.extract(out,size,*error);});
        status.commit();
        if(call(6))memory.store(GuestAddress{call(6)},32,length);
        return finish(static_cast<std::uint32_t>(result));
    }
    if(name=="udatpg_replaceFieldTypes" || name=="udatpg_replaceFieldTypesWithOptions"){
        const IcuTextInput<UChar> replacement(memory,call(3),i(4),status.value);
        if(U_FAILURE(status.value)){status.commit();return finish(0);}
        const bool options=name.ends_with("WithOptions");
        return finish(icu_preserving_output<UChar>(memory,call(options?6U:5U),i(options?7U:6U),status,
            [&](auto* out,auto size,auto* error){
                if(options)return udatpg_replaceFieldTypesWithOptions(native,input.get(),input.length,replacement.get(),
                    replacement.length,static_cast<UDateTimePatternMatchOptions>(call(5)),out,size,error);
                return udatpg_replaceFieldTypes(native,input.get(),input.length,replacement.get(),
                    replacement.length,out,size,error);
            }));
    }
    const bool options=name=="udatpg_getBestPatternWithOptions";
    return finish(icu_preserving_output<UChar>(memory,call(options?4U:3U),i(options?5U:4U),status,
        [&](auto* out,auto size,auto* error){
            if(options)return udatpg_getBestPatternWithOptions(native,input.get(),input.length,
                static_cast<UDateTimePatternMatchOptions>(call(3)),out,size,error);
            if(name=="udatpg_getSkeleton")return udatpg_getSkeleton(native,input.get(),input.length,out,size,error);
            if(name=="udatpg_getBaseSkeleton")return udatpg_getBaseSkeleton(native,input.get(),input.length,out,size,error);
            return udatpg_getBestPattern(native,input.get(),input.length,out,size,error);
        }));
}
}
