#include "x64arm/windows_services.hpp"
#include "icu_calendar_state.hpp"
#include "icu_guest_abi.hpp"
#include "icu_number_state.hpp"
#include <unicode/dtfmtsym.h>
#include <unicode/smpdtfmt.h>
#include <unicode/udat.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using OwnedFormatter=std::unique_ptr<UDateFormat,decltype(&udat_close)>;

struct Formatter {
    OwnedFormatter native{nullptr,udat_close};
    std::uint64_t calendar{};
    std::map<const UNumberFormat*,std::uint64_t> numbers;
};
}

struct IcuDateFormatState {
    std::map<std::uint64_t,Formatter> formatters;
};

std::span<const WindowsServiceDescriptor> icu_date_format_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_DATE_API(name,count,wide) {"icu.dll",#name,count,wide,0,0,true},
#define ICU_DATE_QUERY ICU_DATE_API
#include "icu_date_format_services.inc"
#undef ICU_DATE_QUERY
#undef ICU_DATE_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_date_format(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto i=[&](unsigned n){return signed_argument(call(n));};
    const auto finish=[&](std::uint64_t result){call.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto integer=[&](std::int32_t result){return finish(static_cast<std::uint32_t>(result));};
    const auto floating=[&](double result){windows_floating_return(state,x86_,result,false);return finish(0);};
    const auto flag=[&](unsigned n){return static_cast<UBool>(call(n));};
    const auto date=[&](unsigned n){return icu_double_argument(state,call,x86_,n);};
    if(name=="udat_countAvailable")return integer(udat_countAvailable());
    if(name=="udat_getAvailable"){
        const auto index=i(0);
        return finish(index>=0 && index<udat_countAvailable()?publish_icu_literal(memory,udat_getAvailable(index)):0);
    }
    if(name=="udat_toCalendarDateField"){
        return integer(i(0)>=0 && i(0)<UDAT_FIELD_COUNT?
            udat_toCalendarDateField(static_cast<UDateFormatField>(i(0))):UCAL_FIELD_COUNT);
    }
    if(!icu_date_format_)icu_date_format_=std::make_shared<IcuDateFormatState>();
    auto& store=icu_date_format_->formatters;
    const auto found=store.find(call(0));
    auto* object=found==store.end()?nullptr:&found->second;
    auto* native=object?object->native.get():nullptr;
    const auto drop_calendar=[&]{
        if(!object || !object->calendar || !icu_calendar_)return;
        icu_calendar_->calendars.erase(object->calendar);
        (void)free_heap(memory,std::exchange(object->calendar,0),state);
    };
    const auto active_number=[&](const UNumberFormat* provider){
        if(udat_getNumberFormat(native)==provider)return true;
        const auto* field=icu::DateFormatSymbols::getPatternUChars();
        for(;*field;++field)if(udat_getNumberFormatForField(native,*field)==provider)return true;
        return false;
    };
    const auto prune_numbers=[&](bool all=false){
        if(!object || !icu_number_)return;
        for(auto entry=object->numbers.begin();entry!=object->numbers.end();){
            if(!all && active_number(entry->first)){++entry;continue;}
            icu_number_->formats.erase(entry->second);
            (void)free_heap(memory,entry->second,state);
            entry=object->numbers.erase(entry);
        }
    };
    const auto refresh_calendar=[&]{
        if(object && object->calendar)(void)publish_icu_calendar_view(memory,object->calendar,udat_getCalendar(native));
    };
    const auto calendar_entry=[&](std::uint64_t handle)->IcuCalendarObject*{
        if(!icu_calendar_)return nullptr;
        const auto entry=icu_calendar_->calendars.find(handle);
        return entry==icu_calendar_->calendars.end()?nullptr:&entry->second;
    };
    const auto number_entry=[&](std::uint64_t handle)->IcuNumberObject*{
        if(!icu_number_)return nullptr;
        const auto entry=icu_number_->formats.find(handle);
        return entry==icu_number_->formats.end()?nullptr:&entry->second;
    };
    const auto remember_adopted=[&](std::uint64_t handle,IcuNumberObject& item,UNumberFormat* provider){
        if(active_number(provider)){
            item.borrowed=provider;
            object->numbers.emplace(provider,handle);
        }else{
            icu_number_->formats.erase(handle);
            (void)free_heap(memory,handle,state);
        }
        prune_numbers();
    };
    if(name=="udat_close"){
        if(object){
            drop_calendar();prune_numbers(true);
            store.erase(found);
            (void)free_heap(memory,call(0),state);
        }
        return finish(0);
    }
    if(name=="udat_isLenient")return finish(native && udat_isLenient(native)!=0);
    if(name=="udat_setLenient"){
        if(native){
            udat_setLenient(native,flag(1));
            memory.store(GuestAddress{call(0)},32,udat_isLenient(native)!=0);
            refresh_calendar();
        }
        return finish(0);
    }
    if(name=="udat_applyPattern"){
        UErrorCode status=U_ZERO_ERROR;
        const IcuTextInput<UChar> input(memory,call(2),i(3),status);
        if(native && U_SUCCESS(status))udat_applyPattern(native,flag(1),input.get(),input.length);
        return finish(0);
    }
    if(name=="udat_countSymbols"){
        return integer(native?udat_countSymbols(native,static_cast<UDateFormatSymbolType>(i(1))):0);
    }
    if(name=="udat_getCalendar"){
        if(!object)return finish(0);
        const auto* provider=udat_getCalendar(native);
        if(object->calendar){
            const auto* item=calendar_entry(object->calendar);
            if(item && item->get()==provider){refresh_calendar();return finish(object->calendar);}
            drop_calendar();
        }
        if(provider){
            IcuCalendarObject item;
            item.borrowed=provider;
            object->calendar=adopt_icu_calendar(state,memory,std::move(item));
        }
        return finish(object->calendar);
    }
    if(name=="udat_setCalendar"){
        const auto* item=calendar_entry(call(1));
        if(native && item){
            // setCalendar 先複製 provider；釋放客體引用不刪除來源日曆。
            const auto* provider=item->get();
            drop_calendar();
            udat_setCalendar(native,provider);
            memory.store(GuestAddress{call(0)},32,udat_isLenient(native)!=0);
        }
        return finish(0);
    }
    if(name=="udat_getNumberFormat" || name=="udat_getNumberFormatForField"){
        if(!object)return finish(0);
        const auto* provider=name=="udat_getNumberFormat"?udat_getNumberFormat(native):
            udat_getNumberFormatForField(native,static_cast<UChar>(call(1)));
        if(!provider)return finish(0);
        if(const auto entry=object->numbers.find(provider);entry!=object->numbers.end())return finish(entry->second);
        if(!icu_number_)icu_number_=std::make_shared<IcuNumberState>();
        const auto address=allocate_heap(memory,1);
        if(address){
            IcuNumberObject item;
            item.borrowed=provider;
            icu_number_->formats.emplace(address,std::move(item));
            object->numbers.emplace(provider,address);
        }
        return finish(address);
    }
    if(name=="udat_adoptNumberFormat" || name=="udat_setNumberFormat"){
        auto* item=number_entry(call(1));
        if(!object || !item)return finish(0);
        if(name=="udat_adoptNumberFormat"){
            if(!item->owned)return finish(0);
            auto* provider=item->owned.release();
            udat_adoptNumberFormat(native,provider);
            remember_adopted(call(1),*item,provider);
        }else{
            const auto* provider=item->get();
            // ICU 先複製內容再替換；客體借用 handle 僅在替換處失效。
            udat_setNumberFormat(native,provider);
            prune_numbers();
        }
        return finish(0);
    }

    const bool float_result=name=="udat_parse" || name=="udat_get2DigitYearStart";
    const auto zero=[&]{
        if(float_result)return floating(0.0);
        if(name.starts_with("udat_format"))return integer(-1);
        return finish(0);
    };
    IcuStatus status(memory,call(service.arguments-1));
    if(U_FAILURE(status.value))return zero();
    const auto illegal=[&]{status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return zero();};
    if(name=="udat_open" || name=="udat_clone"){
        Formatter selected;
        if(name=="udat_clone"){
            if(!native)return illegal();
            selected.native.reset(udat_clone(native,&status.value));
        }else{
            const auto style_valid=[](std::int32_t style){
                return (style>=UDAT_PATTERN && style<=UDAT_SHORT) || (style>=UDAT_FULL_RELATIVE && style<=UDAT_SHORT_RELATIVE);
            };
            if(!style_valid(i(0)) || !style_valid(i(1)))return illegal();
            const auto locale=call(2)?read_ansi(memory,call(2),INT32_MAX):std::string{};
            const IcuTextInput<UChar> zone(memory,call(3),call(3)?i(4):0,status.value);
            const IcuTextInput<UChar> pattern(memory,call(5),call(5)?i(6):0,status.value);
            if(U_FAILURE(status.value)){status.commit();return finish(0);}
            const auto* tz=zone.get()?zone.get():icu_default_time_zone();
            selected.native.reset(udat_open(static_cast<UDateFormatStyle>(i(0)),static_cast<UDateFormatStyle>(i(1)),
                call(2)?locale.c_str():icu_default_locale(),tz,zone.get()?zone.length:-1,pattern.get(),
                pattern.get()?pattern.length:0,&status.value));
        }
        std::uint64_t handle{};
        if(selected.native && U_SUCCESS(status.value)){
            handle=allocate_heap(memory,4);
            if(handle){
                memory.store(GuestAddress{handle},32,udat_isLenient(selected.native.get())!=0);
                store.emplace(handle,std::move(selected));
            }else status.value=U_MEMORY_ALLOCATION_ERROR;
        }
        status.commit();return finish(handle);
    }
    if(!native)return illegal();
    if(name=="udat_adoptNumberFormatForFields"){
        auto* item=number_entry(call(2));
        if(!item || !item->owned)return illegal();
        const IcuTextInput<UChar> fields(memory,call(1),call(1)?-1:0,status.value);
        if(U_SUCCESS(status.value)){
            auto* provider=item->owned.get();
            const auto* formatter=reinterpret_cast<const icu::DateFormat*>(native);
            const bool adopts=fields.get() && dynamic_cast<const icu::SimpleDateFormat*>(formatter);
            if(adopts)provider=item->owned.release();
            udat_adoptNumberFormatForFields(native,fields.get(),provider,&status.value);
            if(adopts)remember_adopted(call(2),*item,provider);
        }
        status.commit();return finish(0);
    }
    if(name=="udat_get2DigitYearStart"){
        const auto result=udat_get2DigitYearStart(native,&status.value);
        status.commit();return floating(result);
    }
    if(name=="udat_set2DigitYearStart"){
        udat_set2DigitYearStart(native,date(1),&status.value);
        status.commit();return finish(0);
    }
    if(name=="udat_getLocaleByType"){
        if(i(1)<ULOC_ACTUAL_LOCALE || i(1)>ULOC_VALID_LOCALE)return illegal();
        const auto* result=udat_getLocaleByType(native,static_cast<ULocDataLocaleType>(i(1)),&status.value);
        status.commit();return finish(publish_icu_literal(memory,result));
    }
    if(name=="udat_getBooleanAttribute" || name=="udat_setBooleanAttribute"){
        const auto attribute=static_cast<UDateFormatBooleanAttribute>(i(1));
        bool result=false;
        if(name=="udat_getBooleanAttribute")result=udat_getBooleanAttribute(native,attribute,&status.value)!=0;
        else udat_setBooleanAttribute(native,attribute,flag(2),&status.value);
        status.commit();return finish(result);
    }
    if(name=="udat_getContext" || name=="udat_setContext"){
        std::int32_t result{};
        if(name=="udat_getContext")result=udat_getContext(native,static_cast<UDisplayContextType>(i(1)),&status.value);
        else udat_setContext(native,static_cast<UDisplayContext>(i(1)),&status.value);
        status.commit();return integer(result);
    }
    if(name=="udat_getSymbols" || name=="udat_toPattern"){
        const bool symbols=name=="udat_getSymbols";
        return finish(icu_preserving_output<UChar>(memory,call(symbols?3U:2U),i(symbols?4U:3U),status,
            [&](auto* out,auto size,auto* error){
                if(symbols)return udat_getSymbols(native,static_cast<UDateFormatSymbolType>(i(1)),i(2),out,size,error);
                return udat_toPattern(native,flag(1),out,size,error);
            }));
    }
    if(name=="udat_setSymbols"){
        IcuTextInput<UChar> text(memory,call(3),i(4),status.value);
        if(U_SUCCESS(status.value))udat_setSymbols(native,static_cast<UDateFormatSymbolType>(i(1)),i(2),text.get(),text.length,&status.value);
        status.commit();return finish(0);
    }
    if(name.starts_with("udat_format")){
        if(i(3)<0 || (!call(2) && i(3)))return illegal();
        const bool calendar_input=name=="udat_formatCalendar" || name=="udat_formatCalendarForFields";
        const bool fields=name.ends_with("ForFields");
        auto* input=calendar_input?calendar_entry(call(1)):nullptr;
        if(calendar_input && !input)return illegal();
        auto* calendar=input?const_cast<UCalendar*>(input->get()):nullptr;
        const auto position=call(4);
        UFieldPosition field{};
        OwnedIcuFieldIterator positions(nullptr,ufieldpositer_close);
        if(fields && position){
            if(!icu_number_ || !icu_number_->positions.contains(position))return illegal();
            positions.reset(ufieldpositer_open(&status.value));
        }else if(!fields)read_icu_record(memory,position,field);
        const auto initial=field;
        const auto result=icu_preserving_output<UChar>(memory,call(2),i(3),status,[&](auto* out,auto size,auto* error){
            field=initial;
            auto* current=position?&field:nullptr;
            if(calendar_input && fields)return udat_formatCalendarForFields(native,calendar,out,size,positions.get(),error);
            if(calendar_input)return udat_formatCalendar(native,calendar,out,size,current,error);
            if(fields)return udat_formatForFields(native,date(1),out,size,positions.get(),error);
            return udat_format(native,date(1),out,size,current,error);
        });
        if(U_SUCCESS(status.value) || status.value==U_BUFFER_OVERFLOW_ERROR){
            if(positions){
                if(!publish_icu_field_positions(state,memory,position,*positions))status.value=U_MEMORY_ALLOCATION_ERROR;
            }else if(!fields)write_icu_record(memory,position,field);
        }
        status.commit();return finish(result);
    }
    if(name=="udat_parse" || name=="udat_parseCalendar"){
        const bool calendar_output=name=="udat_parseCalendar";
        const unsigned base=calendar_output?2U:1U;
        const IcuTextInput<UChar> text(memory,call(base),i(base+1),status.value);
        if(U_FAILURE(status.value)){status.commit();return zero();}
        const auto position=call(base+2);
        auto offset=position?signed_argument(memory.load(GuestAddress{position},32)):0;
        double result{};
        if(calendar_output){
            auto* target=calendar_entry(call(1));
            if(!target || !target->owned)return illegal();
            udat_parseCalendar(native,target->owned.get(),text.get(),text.length,position?&offset:nullptr,&status.value);
        }else result=udat_parse(native,text.get(),text.length,position?&offset:nullptr,&status.value);
        if(position)memory.store(GuestAddress{position},32,static_cast<std::uint32_t>(offset));
        status.commit();return calendar_output?finish(0):floating(result);
    }
    throw HostCallFault("ICU 日期格式未登錄的服務");
}
}
