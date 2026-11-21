#include "x64arm/windows_services.hpp"
#include "icu_formatted_layout.hpp"
#include "icu_guest_abi.hpp"
#include "icu_number_state.hpp"
#include <new>
#include <utility>
#include <unicode/udateintervalformat.h>
#include <unicode/ulistformatter.h>
#include <unicode/ureldatefmt.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using namespace icu_formatted;

template<class T, auto Close>
struct IcuCloser {
    void operator()(T* object)const{Close(object);}
};
template<class T, auto Close>
using IcuHandle=std::unique_ptr<T,IcuCloser<T,Close>>;

struct IcuProjectedValue {
    const UFormattedValue* provider{};
    std::uint64_t storage{};
    std::size_t capacity{};
};

template<class T, auto Close>
struct IcuResult {
    IcuHandle<T,Close> native;
    std::uint64_t value{};
};
}

struct IcuFormattedState {
    std::map<std::uint64_t,IcuHandle<UConstrainedFieldPosition,ucfpos_close>> positions;
    std::map<std::uint64_t,IcuProjectedValue> values;
    std::map<std::uint64_t,IcuHandle<UDateIntervalFormat,udtitvfmt_close>> intervals;
    std::map<std::uint64_t,IcuHandle<URelativeDateTimeFormatter,ureldatefmt_close>> relatives;
    std::map<std::uint64_t,IcuHandle<UListFormatter,ulistfmt_close>> lists;
    std::map<std::uint64_t,IcuResult<UFormattedDateInterval,udtitvfmt_closeResult>> interval_results;
    std::map<std::uint64_t,IcuResult<UFormattedRelativeDateTime,ureldatefmt_closeResult>> relative_results;
    std::map<std::uint64_t,IcuResult<UFormattedList,ulistfmt_closeResult>> list_results;
};

std::span<const WindowsServiceDescriptor> icu_formatted_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_FMT_API(name,count,wide,legacy) {"icu.dll",#name,count,wide,0,0,true},
#define ICU_FMT_QUERY ICU_FMT_API
#include "icu_formatted_services.inc"
#include "icu_list_services.inc"
#undef ICU_FMT_QUERY
#undef ICU_FMT_API
    };
    return entries;
}

void WindowsServices::release_icu_formatted_value(GuestState& state,GuestMemory& memory,std::uint64_t handle){
    if(!handle || !icu_formatted_)return;
    auto& values=icu_formatted_->values;
    const auto found=values.find(handle);
    if(found==values.end())return;
    if(found->second.storage)(void)free_heap(memory,found->second.storage,state);
    values.erase(found);
    (void)free_heap(memory,handle,state);
}

void WindowsServices::refresh_icu_formatted_value(GuestState& state,GuestMemory& memory,
        std::uint64_t handle,IcuStatus& status){
    if(!handle)return;
    auto& allocation=icu_formatted_->values.at(handle);
    Value view;
    view.error=U_ZERO_ERROR;
    const auto* text=ufmtval_getString(allocation.provider,&view.length,&view.error);
    if(text && U_SUCCESS(view.error)){
        const auto units=static_cast<std::size_t>(view.length)+1U;
        const auto bytes=units*sizeof(UChar);
        if(bytes>allocation.capacity){
            const auto address=allocate_heap(memory,bytes);
            if(address){
                if(allocation.storage)(void)free_heap(memory,allocation.storage,state);
                allocation.storage=address;
                allocation.capacity=bytes;
            }else view.error=U_MEMORY_ALLOCATION_ERROR;
        }
        if(U_SUCCESS(view.error)){
            view.text=allocation.storage;
            memory.write(GuestAddress{view.text},icu_bytes(text,units));
        }
    }
    write_icu_record(memory,handle,view);
    if(view.error==U_MEMORY_ALLOCATION_ERROR && U_SUCCESS(status.value))status.value=view.error;
}

std::uint64_t WindowsServices::adopt_icu_formatted_value(GuestState& state,GuestMemory& memory,
        const UFormattedValue* provider,IcuStatus& status){
    if(!icu_formatted_)icu_formatted_=std::make_shared<IcuFormattedState>();
    const auto handle=allocate_heap(memory,sizeof(Value));
    if(!handle){
        status.value=U_MEMORY_ALLOCATION_ERROR;
        return 0;
    }
    try{
        icu_formatted_->values.emplace(handle,IcuProjectedValue{provider});
    }catch(const std::bad_alloc&){
        (void)free_heap(memory,handle,state);
        status.value=U_MEMORY_ALLOCATION_ERROR;
        return 0;
    }
    refresh_icu_formatted_value(state,memory,handle,status);
    if(U_FAILURE(status.value)){
        release_icu_formatted_value(state,memory,handle);
        return 0;
    }
    return handle;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_formatted(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto i=[&](unsigned n){return signed_argument(call(n));};
    const auto finish=[&](std::uint64_t result,bool wide=false){
        call.return_to(state,result,wide);
        return std::optional<std::uint32_t>{};
    };
    const auto date=[&](unsigned n){return icu_double_argument(state,call,x86_,n);};
    if(!icu_formatted_)icu_formatted_=std::make_shared<IcuFormattedState>();
    auto& objects=*icu_formatted_;
    const auto erase=[&](auto& map){
        if(map.erase(call(0)))(void)free_heap(memory,call(0),state);
        return finish(0);
    };
    const auto close_result=[&](auto& map){
        const auto found=map.find(call(0));
        if(found!=map.end())release_icu_formatted_value(state,memory,found->second.value);
        return erase(map);
    };
    if(name=="ucfpos_close")return erase(objects.positions);
    if(name=="udtitvfmt_close")return erase(objects.intervals);
    if(name=="ureldatefmt_close")return erase(objects.relatives);
    if(name=="ulistfmt_close")return erase(objects.lists);
    if(name=="udtitvfmt_closeResult")return close_result(objects.interval_results);
    if(name=="ureldatefmt_closeResult")return close_result(objects.relative_results);
    if(name=="ulistfmt_closeResult")return close_result(objects.list_results);

    IcuStatus status(memory,call(service.arguments-1));
    const auto result=[&](std::uint64_t value=0,bool wide=false){status.commit();return finish(value,wide);};
    const auto invalid=[&](std::uint64_t value=0){
        status.value=U_ILLEGAL_ARGUMENT_ERROR;
        return result(value,name=="ucfpos_getInt64IterationContext");
    };
    const auto adopt=[&](auto& map,auto object,std::size_t size=1U){
        const auto handle=allocate_heap(memory,size);
        if(handle){
            try{
                map.emplace(handle,std::move(object));
                return handle;
            }catch(const std::bad_alloc&){
                (void)free_heap(memory,handle,state);
            }
        }
        status.value=U_MEMORY_ALLOCATION_ERROR;
        return std::uint64_t{};
    };
    if(name=="ucfpos_open"){
        IcuHandle<UConstrainedFieldPosition,ucfpos_close> native(ucfpos_open(&status.value));
        const auto handle=native?adopt(objects.positions,std::move(native),sizeof(Position)):0;
        if(handle)write_icu_record(memory,handle,Position{});
        return result(handle);
    }
    if(U_FAILURE(status.value)){
        if(name=="udtitvfmt_format" || name=="ulistfmt_format")return finish(UINT32_MAX);
        return finish(0,name=="ucfpos_getInt64IterationContext");
    }
    if(name.starts_with("ucfpos_")){
        if(!objects.positions.contains(call(0)))return invalid();
        Position position;
        read_icu_record(memory,call(0),position);
        if(position.magic!=Position::signature)return invalid();
        if(name=="ucfpos_getCategory")return finish(static_cast<std::uint32_t>(position.category));
        if(name=="ucfpos_getField")return finish(static_cast<std::uint32_t>(position.field));
        if(name=="ucfpos_getInt64IterationContext")return finish(std::bit_cast<std::uint64_t>(position.context),true);
        if(name=="ucfpos_matchesField")return finish(position.matches(i(1),i(2)));
        if(name=="ucfpos_getIndexes"){
            memory.store(GuestAddress{call(1)},32,static_cast<std::uint32_t>(position.start));
            memory.store(GuestAddress{call(2)},32,static_cast<std::uint32_t>(position.limit));
            return finish(0);
        }
        if(name=="ucfpos_reset")position=Position{};
        else if(name=="ucfpos_constrainCategory")position.select(i(1));
        else if(name=="ucfpos_constrainField")position.select(i(1),i(2));
        else if(name=="ucfpos_setInt64IterationContext")position.context=std::bit_cast<std::int64_t>(call(1));
        else if(name=="ucfpos_setState")position.set(i(1),i(2),i(3),i(4));
        else throw HostCallFault("ICU 欄位狀態服務未登錄");
        write_icu_record(memory,call(0),position);
        return finish(0);
    }
    if(name=="ufmtval_getString"){
        if(!objects.values.contains(call(0)))return invalid();
        Value view;
        read_icu_record(memory,call(0),view);
        if(view.magic!=Value::signature)return invalid();
        if(view.error!=U_ZERO_ERROR)status.value=view.error;
        if(U_FAILURE(status.value))return result();
        if(call(1))memory.store(GuestAddress{call(1)},32,static_cast<std::uint32_t>(view.length));
        return result(view.text);
    }
    if(name=="ufmtval_nextPosition"){
        const auto value=objects.values.find(call(0));
        const auto found=objects.positions.find(call(1));
        if(value==objects.values.end() || found==objects.positions.end())return invalid();
        Position view;
        read_icu_record(memory,call(1),view);
        if(view.magic!=Position::signature)return invalid();
        auto* native=found->second.get();
        ucfpos_reset(native,&status.value);
        if(view.constraint==Constraint::Category)ucfpos_constrainCategory(native,view.category,&status.value);
        else if(view.constraint==Constraint::Field)ucfpos_constrainField(native,view.category,view.field,&status.value);
        ucfpos_setState(native,view.category,view.field,view.start,view.limit,&status.value);
        ucfpos_setInt64IterationContext(native,view.context,&status.value);
        const bool available=ufmtval_nextPosition(value->second.provider,native,&status.value)!=0;
        if(U_SUCCESS(status.value)){
            view.category=ucfpos_getCategory(native,&status.value);
            view.field=ucfpos_getField(native,&status.value);
            view.context=ucfpos_getInt64IterationContext(native,&status.value);
            ucfpos_getIndexes(native,&view.start,&view.limit,&status.value);
            write_icu_record(memory,call(1),view);
        }
        return result(available);
    }

    const auto as_value=[&](auto& map,auto conversion){
        const auto item=map.find(call(0));
        if(item==map.end())return invalid();
        auto& format=item->second;
        if(format.value)return result(format.value);
        const auto* native=conversion(format.native.get(),&status.value);
        if(!native || U_FAILURE(status.value))return result();
        format.value=adopt_icu_formatted_value(state,memory,native,status);
        return result(format.value);
    };
    if(name=="udtitvfmt_openResult"){
        IcuResult<UFormattedDateInterval,udtitvfmt_closeResult> item;
        item.native.reset(udtitvfmt_openResult(&status.value));
        return result(item.native?adopt(objects.interval_results,std::move(item)):0);
    }
    if(name=="ureldatefmt_openResult"){
        IcuResult<UFormattedRelativeDateTime,ureldatefmt_closeResult> item;
        item.native.reset(ureldatefmt_openResult(&status.value));
        return result(item.native?adopt(objects.relative_results,std::move(item)):0);
    }
    if(name=="ulistfmt_openResult"){
        IcuResult<UFormattedList,ulistfmt_closeResult> item;
        item.native.reset(ulistfmt_openResult(&status.value));
        return result(item.native?adopt(objects.list_results,std::move(item)):0);
    }
    if(name=="udtitvfmt_resultAsValue")return as_value(objects.interval_results,udtitvfmt_resultAsValue);
    if(name=="ureldatefmt_resultAsValue")return as_value(objects.relative_results,ureldatefmt_resultAsValue);
    if(name=="ulistfmt_resultAsValue")return as_value(objects.list_results,ulistfmt_resultAsValue);
    if(name=="ulistfmt_open" || name=="ulistfmt_openForType"){
        const auto locale=call(0)?read_ansi(memory,call(0),INT32_MAX):std::string{};
        const auto* locale_id=call(0)?locale.c_str():icu_default_locale();
        IcuHandle<UListFormatter,ulistfmt_close> native(name=="ulistfmt_open"?ulistfmt_open(locale_id,&status.value):
            ulistfmt_openForType(locale_id,static_cast<UListFormatterType>(i(1)),
                               static_cast<UListFormatterWidth>(i(2)),&status.value));
        return result(native?adopt(objects.lists,std::move(native)):0);
    }
    if(name=="ulistfmt_format" || name=="ulistfmt_formatStringsToResult"){
        const bool direct=name=="ulistfmt_format";
        const auto count=i(3);
        const auto failure=direct?UINT32_MAX:0U;
        const auto format=objects.lists.find(call(0));
        if(format==objects.lists.end() || count<0 || (count && !call(1)))return invalid(failure);
        const auto target=direct?objects.list_results.end():objects.list_results.find(call(4));
        if(direct?(i(5)<0 || (!call(4) && i(5))):target==objects.list_results.end())return invalid(failure);
        const auto bits=static_cast<std::uint8_t>(x86_?32:64);
        const auto width=bits/8U;
        const auto size=static_cast<std::size_t>(count);
        if(count)memory.check(GuestAddress{call(1)},size*width,Access::Read);
        if(count && call(2))memory.check(GuestAddress{call(2)},size*sizeof(std::int32_t),Access::Read);
        try{
            std::vector<IcuTextInput<UChar>> strings;
            strings.reserve(size);
            std::vector<const UChar*> pointers(size);
            std::vector<std::int32_t> lengths(call(2)?size:0);
            for(std::size_t n=0;n<size;++n){
                const auto address=memory.load(GuestAddress{call(1)+n*width},bits);
                const auto length=call(2)?signed_argument(memory.load(GuestAddress{call(2)+n*4},32)):-1;
                if(call(2))lengths[n]=length;
                // 保留 NULL 項目與顯式長度；ICU 會依 UnicodeString 的規則判定空字串或無效長度。
                strings.emplace_back(memory,address,address && length>=-1?length:0,status.value);
                pointers[n]=strings.back().get();
            }
            const auto* input_lengths=call(2)?lengths.data():nullptr;
            const auto* native=format->second.get();
            if(direct)return finish(icu_preserving_output<UChar>(memory,call(4),i(5),status,
                [&](auto* output,auto capacity,auto* error){
                    return ulistfmt_format(native,pointers.data(),input_lengths,count,output,capacity,error);
                }));
            ulistfmt_formatStringsToResult(native,pointers.data(),input_lengths,count,target->second.native.get(),&status.value);
        }catch(const std::bad_alloc&){
            status.value=U_MEMORY_ALLOCATION_ERROR;
            return result(failure);
        }
        refresh_icu_formatted_value(state,memory,target->second.value,status);
        return result();
    }
    if(name=="udtitvfmt_open" || name=="ureldatefmt_open"){
        const auto locale=call(0)?read_ansi(memory,call(0),INT32_MAX):std::string{};
        const auto* locale_id=call(0)?locale.c_str():icu_default_locale();
        if(name=="udtitvfmt_open"){
            const IcuTextInput<UChar> skeleton(memory,call(1),i(2),status.value);
            const IcuTextInput<UChar> zone(memory,call(3),i(4),status.value);
            if(U_FAILURE(status.value))return result();
            const auto* tz=zone.get()?zone.get():icu_default_time_zone();
            IcuHandle<UDateIntervalFormat,udtitvfmt_close> native(udtitvfmt_open(locale_id,skeleton.get(),
                skeleton.length,tz,zone.get()?zone.length:tz?-1:0,&status.value));
            return result(native?adopt(objects.intervals,std::move(native)):0);
        }
        const auto style=static_cast<UDateRelativeDateTimeFormatterStyle>(i(2));
        const auto capitalization=static_cast<UDisplayContext>(i(3));
        if(i(2)<0 || style>=UDAT_STYLE_COUNT || (i(3)>>8)!=UDISPCTX_TYPE_CAPITALIZATION)return invalid();
        OwnedIcuNumber number(nullptr,unum_close);
        if(call(1)){
            if(!icu_number_)return invalid();
            const auto found=icu_number_->formats.find(call(1));
            if(found==icu_number_->formats.end() || !found->second.owned)return invalid();
            number=std::move(found->second.owned);
            icu_number_->formats.erase(found);
            (void)free_heap(memory,call(1),state);
        }
        IcuHandle<URelativeDateTimeFormatter,ureldatefmt_close> native(ureldatefmt_open(locale_id,number.release(),
            style,capitalization,&status.value));
        return result(native?adopt(objects.relatives,std::move(native)):0);
    }

    if(name.starts_with("udtitvfmt_")){
        const auto format=objects.intervals.find(call(0));
        if(format==objects.intervals.end())return invalid(name=="udtitvfmt_format"?UINT32_MAX:0);
        auto* native=format->second.get();
        if(name=="udtitvfmt_formatToResult"){
            const auto target=objects.interval_results.find(call(3));
            if(target==objects.interval_results.end())return invalid();
            udtitvfmt_formatToResult(native,date(1),date(2),target->second.native.get(),&status.value);
            refresh_icu_formatted_value(state,memory,target->second.value,status);
            return result();
        }
        if(name=="udtitvfmt_setContext"){
            udtitvfmt_setContext(native,static_cast<UDisplayContext>(i(1)),&status.value);
            return result();
        }
        if(name=="udtitvfmt_getContext"){
            const auto value=udtitvfmt_getContext(native,static_cast<UDisplayContextType>(i(1)),&status.value);
            return result(static_cast<std::uint32_t>(value));
        }
        if(name=="udtitvfmt_format"){
            if(i(4)<0 || (!call(3) && i(4)))return invalid();
            UFieldPosition position{};
            read_icu_record(memory,call(5),position);
            const auto initial=position;
            const auto length=icu_preserving_output<UChar>(memory,call(3),i(4),status,[&](auto* out,auto capacity,auto* error){
                position=initial;
                return udtitvfmt_format(native,date(1),date(2),out,capacity,call(5)?&position:nullptr,error);
            });
            if(U_SUCCESS(status.value) || status.value==U_BUFFER_OVERFLOW_ERROR)
                write_icu_record(memory,call(5),position);
            return finish(length);
        }
    }
    if(name.starts_with("ureldatefmt_")){
        const auto format=objects.relatives.find(call(0));
        if(format==objects.relatives.end())return invalid();
        auto* native=format->second.get();
        if(name=="ureldatefmt_combineDateAndTime"){
            const IcuTextInput<UChar> day(memory,call(1),i(2),status.value);
            const IcuTextInput<UChar> time(memory,call(3),i(4),status.value);
            if(U_FAILURE(status.value))return result();
            return finish(icu_preserving_output<UChar>(memory,call(5),i(6),status,[&](auto* out,auto capacity,auto* error){
                return ureldatefmt_combineDateAndTime(native,day.get(),day.length,time.get(),time.length,out,capacity,error);
            }));
        }
        const auto unit=static_cast<URelativeDateTimeUnit>(i(2));
        const bool numeric=name.starts_with("ureldatefmt_formatNumeric");
        if(name.ends_with("ToResult")){
            const auto target=objects.relative_results.find(call(3));
            if(target==objects.relative_results.end())return invalid();
            if(numeric)ureldatefmt_formatNumericToResult(native,date(1),unit,target->second.native.get(),&status.value);
            else ureldatefmt_formatToResult(native,date(1),unit,target->second.native.get(),&status.value);
            refresh_icu_formatted_value(state,memory,target->second.value,status);
            return result();
        }
        if(name=="ureldatefmt_format" || name=="ureldatefmt_formatNumeric"){
            return finish(icu_preserving_output<UChar>(memory,call(3),i(4),status,[&](auto* out,auto capacity,auto* error){
                if(numeric)return ureldatefmt_formatNumeric(native,date(1),unit,out,capacity,error);
                return ureldatefmt_format(native,date(1),unit,out,capacity,error);
            }));
        }
    }
    throw HostCallFault("ICU 格式化值及日期區間服務未登錄");
}
}
