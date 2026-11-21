#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "icu_number_state.hpp"
#include <unicode/parseerr.h>

namespace x64arm {
namespace {
using namespace icu_abi;
}

std::span<const WindowsServiceDescriptor> icu_number_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_NUM_API(name,count,wide) {"icu.dll",#name,count,wide,0,0,true},
#define ICU_NUM_QUERY ICU_NUM_API
#include "icu_number_services.inc"
#undef ICU_NUM_QUERY
#undef ICU_NUM_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_number(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(!name.starts_with("unum_"))return invoke_icu_number_object(state,memory,service);
    const WindowsCall call(state,memory,service);
    const auto i=[&](unsigned n){return signed_argument(call(n));};
    const auto finish=[&](std::uint64_t result,bool wide=false){
        call.return_to(state,result,wide);return std::optional<std::uint32_t>{};
    };
    const auto integral=[&](std::int32_t result){return finish(static_cast<std::uint32_t>(result));};
    const auto real=[&](double result){windows_floating_return(state,x86_,result,false);return finish(0);};
    const auto double_at=[&](unsigned n){return icu_double_argument(state,call,x86_,n);};
    const auto boolean=[&](unsigned n){return static_cast<UBool>(call(n));};
    if(name=="unum_countAvailable")return integral(unum_countAvailable());
    if(name=="unum_getAvailable"){
        const auto index=i(0);
        return finish(index>=0 && index<unum_countAvailable()?publish_icu_literal(memory,unum_getAvailable(index)):0);
    }
    if(!icu_number_)icu_number_=std::make_shared<IcuNumberState>();
    auto& number=*icu_number_;
    const auto found=number.formats.find(call(0));
    const auto* native=found==number.formats.end()?nullptr:found->second.get();
    auto* mutable_format=found==number.formats.end()?nullptr:found->second.owned.get();
    if(name=="unum_close"){
        if(mutable_format){
            number.formats.erase(found);
            (void)free_heap(memory,call(0),state);
        }
        return finish(0);
    }
    const auto attribute=[&]{return static_cast<UNumberFormatAttribute>(i(1));};
    if(name=="unum_getAttribute")return integral(native?unum_getAttribute(native,attribute()):-1);
    if(name=="unum_getDoubleAttribute")return real(native?unum_getDoubleAttribute(native,attribute()):-1.0);
    if(name=="unum_setAttribute"){
        if(mutable_format)unum_setAttribute(mutable_format,attribute(),i(2));
        return finish(0);
    }
    if(name=="unum_setDoubleAttribute"){
        if(mutable_format)unum_setDoubleAttribute(mutable_format,attribute(),double_at(2));
        return finish(0);
    }

    const bool return_wide=name=="unum_parseInt64";
    const bool return_float=name=="unum_parseDouble" || name=="unum_parseDoubleCurrency";
    const bool new_value=name=="unum_parseToUFormattable";
    const bool negative_failure=name=="unum_parseDecimal" ||
        (name.starts_with("unum_format") && name!="unum_formatUFormattable");
    const auto zero=[&]{
        if(return_float)return real(0.0);
        if(negative_failure)return finish(UINT32_MAX);
        if(new_value)return finish(call(1));
        return finish(0,return_wide);
    };
    IcuStatus status(memory,call(service.arguments-1));
    if(name=="unum_parseDoubleCurrency" && call(4))memory.store(GuestAddress{call(4)},16,0);
    const auto illegal=[&]{status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return zero();};
    if(U_FAILURE(status.value))return zero();
    if(name=="unum_open" || name=="unum_clone"){
        IcuNumberObject object;
        if(name=="unum_open"){
            const auto raw=i(0);
            if(raw<UNUM_PATTERN_DECIMAL || raw>=UNUM_FORMAT_STYLE_COUNT)return illegal();
            const IcuTextInput<UChar> pattern(memory,call(1),i(2),status.value);
            if(U_FAILURE(status.value)){status.commit();return finish(0);}
            const auto locale=call(3)?read_ansi(memory,call(3),INT32_MAX):std::string{};
            UParseError parse{};
            read_icu_record(memory,call(4),parse);
            object.owned.reset(unum_open(static_cast<UNumberFormatStyle>(raw),pattern.get(),pattern.length,
                call(3)?locale.c_str():icu_default_locale(),call(4)?&parse:nullptr,&status.value));
            write_icu_record(memory,call(4),parse);
        }else{
            if(!native)return illegal();
            object.owned.reset(unum_clone(native,&status.value));
        }
        std::uint64_t handle{};
        if(object.owned && U_SUCCESS(status.value)){
            handle=allocate_heap(memory,1);
            if(handle)number.formats.emplace(handle,std::move(object));
            else status.value=U_MEMORY_ALLOCATION_ERROR;
        }
        status.commit();return finish(handle);
    }
    if(!native)return illegal();
    if(name=="unum_applyPattern"){
        if(!mutable_format)return illegal();
        const IcuTextInput<UChar> pattern(memory,call(2),i(3),status.value);
        if(U_SUCCESS(status.value)){
            UParseError parse{};
            read_icu_record(memory,call(4),parse);
            unum_applyPattern(mutable_format,boolean(1),pattern.get(),pattern.length,call(4)?&parse:nullptr,&status.value);
            write_icu_record(memory,call(4),parse);
        }
        status.commit();return finish(0);
    }
    if(name=="unum_getContext" || name=="unum_setContext"){
        std::int32_t result{};
        if(name=="unum_getContext")result=unum_getContext(native,static_cast<UDisplayContextType>(i(1)),&status.value);
        else if(mutable_format)unum_setContext(mutable_format,static_cast<UDisplayContext>(i(1)),&status.value);
        else return illegal();
        status.commit();return integral(result);
    }
    if(name=="unum_getLocaleByType"){
        if(i(1)<ULOC_ACTUAL_LOCALE || i(1)>ULOC_VALID_LOCALE)return illegal();
        const auto* result=unum_getLocaleByType(native,static_cast<ULocDataLocaleType>(i(1)),&status.value);
        status.commit();return finish(publish_icu_literal(memory,result));
    }
    if(name=="unum_getSymbol" || name=="unum_getTextAttribute" || name=="unum_toPattern"){
        return finish(icu_preserving_output<UChar>(memory,call(2),i(3),status,[&](auto* out,auto size,auto* error){
            if(name=="unum_getSymbol")return unum_getSymbol(native,static_cast<UNumberFormatSymbol>(i(1)),out,size,error);
            if(name=="unum_toPattern")return unum_toPattern(native,boolean(1),out,size,error);
            return unum_getTextAttribute(native,static_cast<UNumberFormatTextAttribute>(i(1)),out,size,error);
        }));
    }
    if(name=="unum_setSymbol" || name=="unum_setTextAttribute"){
        if(!mutable_format)return illegal();
        const IcuTextInput<UChar> input(memory,call(2),i(3),status.value);
        if(U_SUCCESS(status.value)){
            if(name=="unum_setSymbol")unum_setSymbol(mutable_format,static_cast<UNumberFormatSymbol>(i(1)),
                input.get(),input.length,&status.value);
            else unum_setTextAttribute(mutable_format,static_cast<UNumberFormatTextAttribute>(i(1)),
                input.get(),input.length,&status.value);
        }
        status.commit();return finish(0);
    }
    if(name.starts_with("unum_format")){
        const bool decimal=name=="unum_formatDecimal";
        const bool currency=name=="unum_formatDoubleCurrency";
        const bool fields=name=="unum_formatDoubleForFields";
        const bool formattable=name=="unum_formatUFormattable";
        const unsigned out_index=decimal || currency?3U:2U;
        if((decimal || fields) && (i(out_index+1)<0 || (!call(out_index) && i(out_index+1))))return illegal();
        const auto field_address=call(out_index+2);
        UFieldPosition field{};
        OwnedIcuFieldIterator positions(nullptr,ufieldpositer_close);
        if(fields && field_address){
            if(!number.positions.contains(field_address))return illegal();
            positions.reset(ufieldpositer_open(&status.value));
        }else if(!fields)read_icu_record(memory,field_address,field);
        const auto original=field;
        const IcuTextInput<char> decimal_text(memory,decimal?call(1):0,decimal?i(2):0,status.value);
        IcuTextInput<UChar> currency_text(memory,currency?call(2):0,currency?-1:0,status.value);
        if(U_FAILURE(status.value)){status.commit();return zero();}
        const UFormattable* value=nullptr;
        if(formattable){
            const auto entry=number.values.find(call(1));
            if(entry==number.values.end())return illegal();
            value=entry->second.get();
        }
        const auto result=icu_preserving_output<UChar>(memory,call(out_index),i(out_index+1),status,[&](auto* out,auto size,auto* error){
            field=original;
            auto* field_arg=field_address?&field:nullptr;
            if(decimal)return unum_formatDecimal(native,decimal_text.get(),decimal_text.length,out,size,field_arg,error);
            if(currency)return unum_formatDoubleCurrency(native,double_at(1),currency_text.get(),out,size,field_arg,error);
            if(fields)return unum_formatDoubleForFields(native,double_at(1),out,size,positions.get(),error);
            if(formattable)return unum_formatUFormattable(native,value,out,size,field_arg,error);
            if(name=="unum_formatDouble")return unum_formatDouble(native,double_at(1),out,size,field_arg,error);
            if(name=="unum_formatInt64")return unum_formatInt64(native,std::bit_cast<std::int64_t>(call(1)),out,size,field_arg,error);
            return unum_format(native,i(1),out,size,field_arg,error);
        });
        if(U_SUCCESS(status.value) || status.value==U_BUFFER_OVERFLOW_ERROR){
            if(positions){
                if(!publish_icu_field_positions(state,memory,field_address,*positions))status.value=U_MEMORY_ALLOCATION_ERROR;
            }else if(!fields)write_icu_record(memory,field_address,field);
        }
        status.commit();return finish(result);
    }

    const unsigned base=new_value?2U:1U;
    const IcuTextInput<UChar> input(memory,call(base),i(base+1),status.value);
    if(U_FAILURE(status.value)){status.commit();return zero();}
    if(!input.get())return illegal();
    const auto position=call(base+2);
    auto offset=position?signed_argument(memory.load(GuestAddress{position},32)):0;
    const auto original=offset;
    auto* cursor=position?&offset:nullptr;
    const auto publish_position=[&]{if(position)memory.store(GuestAddress{position},32,static_cast<std::uint32_t>(offset));};
    if(new_value){
        auto destination=number.values.end();
        if(call(1)){
            destination=number.values.find(call(1));
            if(destination==number.values.end() || !destination->second.owned)return illegal();
        }
        IcuFormattableObject parsed;
        parsed.owned.reset(unum_parseToUFormattable(native,nullptr,input.get(),input.length,cursor,&status.value));
        std::uint64_t handle=destination!=number.values.end()?destination->first:0;
        if(parsed.owned && U_SUCCESS(status.value)){
            if(destination!=number.values.end()){
                clear_icu_formattable_references(state,memory,destination->second);
                destination->second=std::move(parsed);
                publish_icu_formattable(memory,handle,destination->second.get());
            }else handle=adopt_icu_formattable(state,memory,std::move(parsed));
            if(!handle)status.value=U_MEMORY_ALLOCATION_ERROR;
        }
        publish_position();status.commit();return finish(handle);
    }
    if(name=="unum_parseDecimal"){
        if(i(5)<0 || (!call(4) && i(5)))return illegal();
        const auto length=icu_preserving_output<char>(memory,call(4),i(5),status,[&](auto* out,auto size,auto* error){
            offset=original;
            return unum_parseDecimal(native,input.get(),input.length,cursor,out,size,error);
        });
        publish_position();return finish(length);
    }
    if(return_float){
        std::array<UChar,4> currency{};
        const auto result=name=="unum_parseDoubleCurrency"?
            unum_parseDoubleCurrency(native,input.get(),input.length,cursor,currency.data(),&status.value):
            unum_parseDouble(native,input.get(),input.length,cursor,&status.value);
        if(U_SUCCESS(status.value) && name=="unum_parseDoubleCurrency" && call(4))
            memory.write(GuestAddress{call(4)},icu_bytes(currency.data(),currency.size()));
        publish_position();status.commit();return real(result);
    }
    if(return_wide){
        const auto result=unum_parseInt64(native,input.get(),input.length,cursor,&status.value);
        publish_position();status.commit();return finish(static_cast<std::uint64_t>(result),true);
    }
    if(name!="unum_parse")throw HostCallFault("ICU 數字格式未登錄的服務");
    const auto result=unum_parse(native,input.get(),input.length,cursor,&status.value);
    publish_position();status.commit();return integral(result);
}
}
