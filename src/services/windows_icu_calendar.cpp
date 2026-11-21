#include "x64arm/windows_services.hpp"
#include "icu_calendar_layout.hpp"
#include "icu_calendar_state.hpp"
#include "icu_guest_abi.hpp"
#include "service_callback_task.hpp"
#include <unicode/timezone.h>
#include <unicode/ucal.h>
#include <unicode/uenum.h>
#include <unicode/unistr.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using OwnedEnumeration=std::unique_ptr<UEnumeration,decltype(&uenum_close)>;

// ICU 的 locale、時區與名稱介面共用客體 UTF-16 指標及明確長度。
struct ZoneText {
    std::u16string units;
    bool present{};
    ZoneText(const GuestMemory& memory,std::uint64_t address,std::int32_t length,UErrorCode& status){
        if(U_FAILURE(status) || !address)return;
        if(length<-1){status=U_ILLEGAL_ARGUMENT_ERROR;return;}
        present=true;
        if(length<0)units=read_utf16(memory,GuestAddress{address},INT32_MAX);
        else if(length){
            const auto size=static_cast<std::size_t>(length)*sizeof(UChar);
            memory.check(GuestAddress{address},size,Access::Read);
            units.resize(static_cast<std::size_t>(length));
            memory.read(GuestAddress{address},{reinterpret_cast<std::uint8_t*>(units.data()),size});
        }
    }
    const UChar* get()const{return present?units.c_str():nullptr;}
};

bool date_field(std::int32_t raw){
    return raw>=0 && raw<UCAL_FIELD_COUNT;
}

bool local_option(std::int32_t raw){
    switch(raw){
    case UCAL_TZ_LOCAL_FORMER:
    case UCAL_TZ_LOCAL_LATTER:
    case UCAL_TZ_LOCAL_STANDARD_FORMER:
    case UCAL_TZ_LOCAL_STANDARD_LATTER:
    case UCAL_TZ_LOCAL_DAYLIGHT_FORMER:
    case UCAL_TZ_LOCAL_DAYLIGHT_LATTER:return true;
    default:return false;
    }
}
}

const char16_t* WindowsServices::icu_default_time_zone() const {
    return icu_calendar_ && icu_calendar_->default_zone?icu_calendar_->default_zone->c_str():nullptr;
}

bool WindowsServices::publish_icu_calendar_view(GuestMemory& memory,std::uint64_t handle,const void* provider){
    const auto* calendar=static_cast<const UCalendar*>(provider);
    if(!calendar)return false;
    UErrorCode error=U_ZERO_ERROR;
    const auto* type=ucal_getType(calendar,&error);
    if(!type || U_FAILURE(error))return false;
    const auto type_address=publish_icu_literal(memory,type);
    if(!type_address)return false;
    for(std::int32_t attr=UCAL_LENIENT;attr<=UCAL_SKIPPED_WALL_TIME;++attr)
        memory.store(GuestAddress{handle+static_cast<std::uint64_t>(attr)*4},32,
            static_cast<std::uint32_t>(ucal_getAttribute(calendar,static_cast<UCalendarAttribute>(attr))));
    constexpr auto offset=offsetof(icu_calendar::GuestCalendarView,type);
    memory.store(GuestAddress{handle+offset},x86_?32:64,type_address);
    return true;
}

std::uint64_t WindowsServices::adopt_icu_calendar(GuestState& state,GuestMemory& memory,IcuCalendarObject calendar){
    if(!icu_calendar_)icu_calendar_=std::make_shared<IcuCalendarState>();
    constexpr auto offset=offsetof(icu_calendar::GuestCalendarView,type);
    const auto handle=allocate_heap(memory,offset+(x86_?4U:8U));
    if(!handle)return 0;
    if(!publish_icu_calendar_view(memory,handle,calendar.get())){
        (void)free_heap(memory,handle,state);
        return 0;
    }
    icu_calendar_->calendars.emplace(handle,std::move(calendar));
    return handle;
}

std::span<const WindowsServiceDescriptor> icu_calendar_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_CAL_API(name,count,wide,legacy) {"icu.dll",#name,count,wide,0,0,true},
#define ICU_CAL_QUERY ICU_CAL_API
#include "icu_calendar_services.inc"
#undef ICU_CAL_QUERY
#undef ICU_CAL_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_calendar(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name=="ucal_openTimeZoneIDEnumeration" || name=="ucal_openTimeZones" ||
       name=="ucal_openCountryTimeZones" || name=="ucal_getKeywordValuesForLocale"){
        return nested_continuation(state,memory,"ICU 日曆及時區列舉",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                const auto finish=[&](std::uint64_t result){call.return_to(s,result);return std::optional<std::uint32_t>{};};
                IcuStatus status(m,call(service.arguments-1));
                if(U_FAILURE(status.value))co_return finish(0);
                const auto symbol=service.symbol;
                OwnedEnumeration choice(nullptr,uenum_close);
                if(symbol=="ucal_openTimeZones")choice.reset(ucal_openTimeZones(&status.value));
                else if(symbol=="ucal_openCountryTimeZones"){
                    const auto country=call(0)?read_ansi(m,call(0),INT32_MAX):std::string{};
                    choice.reset(ucal_openCountryTimeZones(call(0)?country.c_str():nullptr,&status.value));
                }else if(symbol=="ucal_openTimeZoneIDEnumeration"){
                    const auto kind=signed_argument(call(0));
                    if(kind<UCAL_ZONE_TYPE_ANY || kind>UCAL_ZONE_TYPE_CANONICAL_LOCATION)status.value=U_ILLEGAL_ARGUMENT_ERROR;
                    else{
                        const auto region=call(1)?read_ansi(m,call(1),INT32_MAX):std::string{};
                        auto offset=call(2)?signed_argument(m.load(GuestAddress{call(2)},32)):0;
                        choice.reset(ucal_openTimeZoneIDEnumeration(static_cast<USystemTimeZoneType>(kind),
                            call(1)?region.c_str():nullptr,call(2)?&offset:nullptr,&status.value));
                    }
                }else if(call(0)){
                    const auto key=read_ansi(m,call(0),INT32_MAX);
                    const auto locale=call(1)?read_ansi(m,call(1),INT32_MAX):std::string{};
                    choice.reset(ucal_getKeywordValuesForLocale(key.c_str(),call(1)?locale.c_str():icu_default_locale(),
                        static_cast<UBool>(call(2)),&status.value));
                }else status.value=U_ILLEGAL_ARGUMENT_ERROR;
                if(!choice){status.commit();co_return finish(0);}
                co_return finish(co_await export_icu_enumeration(s,m,*choice,status));
            });
    }

    const WindowsCall call(state,memory,service);
    const auto i=[&](unsigned n){return signed_argument(call(n));};
    const auto finish=[&](std::uint64_t result){call.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto integer=[&](std::int32_t result){return finish(static_cast<std::uint32_t>(result));};
    const auto real=[&](double result){windows_floating_return(state,x86_,result,false);return finish(0);};
    const auto now=[&]{
        constexpr std::int64_t unix_epoch=116444736000000000LL;
        return static_cast<double>((static_cast<std::int64_t>(runtime_.system_filetime())-unix_epoch)/10000);
    };
    if(name=="ucal_getNow")return real(now());
    if(name=="ucal_countAvailable")return integer(ucal_countAvailable());
    if(name=="ucal_getAvailable"){
        const auto index=i(0);
        return finish(index>=0 && index<ucal_countAvailable()?publish_icu_literal(memory,ucal_getAvailable(index)):0);
    }

    if(!icu_calendar_)icu_calendar_=std::make_shared<IcuCalendarState>();
    auto& calendar_state=*icu_calendar_;
    const auto found=calendar_state.calendars.find(call(0));
    const auto* calendar=found==calendar_state.calendars.end()?nullptr:found->second.get();
    auto* mutable_calendar=calendar?found->second.owned.get():nullptr;
    auto& default_zone=calendar_state.default_zone;
    const auto open_calendar=[&](OwnedIcuCalendar item,IcuStatus& status){
        std::uint64_t handle{};
        if(item && U_SUCCESS(status.value)){
            handle=adopt_icu_calendar(state,memory,IcuCalendarObject{std::move(item),nullptr});
            if(!handle)status.value=U_MEMORY_ALLOCATION_ERROR;
        }
        status.commit();return finish(handle);
    };

    if(name=="ucal_close"){
        if(mutable_calendar){
            calendar_state.calendars.erase(found);
            (void)free_heap(memory,call(0),state);
        }
        return finish(0);
    }
    if(name=="ucal_getAttribute")
        return integer(calendar && i(1)>=0 && i(1)<=UCAL_SKIPPED_WALL_TIME?
            ucal_getAttribute(calendar,static_cast<UCalendarAttribute>(i(1))):-1);
    if(name=="ucal_setAttribute"){
        const auto attr=i(1),value=i(2);
        if(mutable_calendar && attr>=UCAL_LENIENT && attr<=UCAL_SKIPPED_WALL_TIME &&
           (attr!=UCAL_FIRST_DAY_OF_WEEK || (value>=UCAL_SUNDAY && value<=UCAL_SATURDAY)) &&
           (attr<UCAL_REPEATED_WALL_TIME || (value>=UCAL_WALLTIME_LAST && value<=UCAL_WALLTIME_NEXT_VALID))){
            ucal_setAttribute(mutable_calendar,static_cast<UCalendarAttribute>(attr),value);
            memory.store(GuestAddress{call(0)+static_cast<std::uint64_t>(attr)*4},32,
                static_cast<std::uint32_t>(ucal_getAttribute(calendar,static_cast<UCalendarAttribute>(attr))));
        }
        return finish(0);
    }
    if(name=="ucal_equivalentTo"){
        const auto other=calendar_state.calendars.find(call(1));
        return finish(calendar && other!=calendar_state.calendars.end() &&
            ucal_equivalentTo(calendar,other->second.get())!=0);
    }
    if(name=="ucal_isSet" || name=="ucal_set" || name=="ucal_clearField" || name=="ucal_clear"){
        if(!calendar)return finish(0);
        if(name=="ucal_clear"){
            if(mutable_calendar)ucal_clear(mutable_calendar);
        }else if(date_field(i(1))){
            const auto field=static_cast<UCalendarDateFields>(i(1));
            if(name=="ucal_isSet")return finish(ucal_isSet(calendar,field)!=0);
            if(mutable_calendar){
                if(name=="ucal_set")ucal_set(mutable_calendar,field,i(2));
                else ucal_clearField(mutable_calendar,field);
            }
        }
        return finish(0);
    }

    IcuStatus status(memory,call(service.arguments-1));
    const bool floating=name=="ucal_getMillis" || name=="ucal_getGregorianChange";
    const auto fail=[&]{
        status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return floating?real(0.0):finish(0);
    };
    if(U_FAILURE(status.value))return floating?real(0.0):finish(0);
    if(name=="ucal_getTZDataVersion"){
        const auto* version=ucal_getTZDataVersion(&status.value);
        status.commit();return finish(publish_icu_literal(memory,version));
    }
    if(name=="ucal_setDefaultTimeZone"){
        if(!call(0))return fail();
        const ZoneText zone(memory,call(0),-1,status.value);
        std::unique_ptr<icu::TimeZone> selected(icu::TimeZone::createTimeZone(icu::UnicodeString(zone.get())));
        if(selected){
            icu::UnicodeString id;
            selected->getID(id);
            default_zone.emplace(id.getBuffer(),static_cast<std::size_t>(id.length()));
        }else status.value=U_MEMORY_ALLOCATION_ERROR;
        status.commit();return finish(0);
    }
    if(name=="ucal_getDefaultTimeZone" || name=="ucal_getHostTimeZone"){
        return finish(icu_preserving_output<UChar>(memory,call(0),i(1),status,[&](auto* out,auto size,auto* error){
            if(name=="ucal_getHostTimeZone")return ucal_getHostTimeZone(out,size,error);
            if(!default_zone)return ucal_getDefaultTimeZone(out,size,error);
            const auto& zone=*default_zone;
            return icu::UnicodeString(zone.data(),static_cast<std::int32_t>(zone.size())).extract(out,size,*error);
        }));
    }
    if(name=="ucal_getDSTSavings"){
        const ZoneText zone(memory,call(0),-1,status.value);
        if(!zone.present)return fail();
        const auto result=ucal_getDSTSavings(zone.get(),&status.value);
        status.commit();return integer(result);
    }
    if(name=="ucal_open"){
        const ZoneText zone(memory,call(0),i(1),status.value);
        if(U_FAILURE(status.value)){status.commit();return finish(0);}
        const auto locale=call(2)?read_ansi(memory,call(2),INT32_MAX):std::string{};
        const auto* zone_id=zone.present?zone.get():(default_zone?default_zone->c_str():nullptr);
        const auto length=zone.present?i(1):-1;
        const auto type=i(3)==UCAL_GREGORIAN?UCAL_GREGORIAN:UCAL_DEFAULT;
        OwnedIcuCalendar opened(ucal_open(zone_id,length,call(2)?locale.c_str():icu_default_locale(),type,&status.value),ucal_close);
        if(opened && U_SUCCESS(status.value))ucal_setMillis(opened.get(),now(),&status.value);
        return open_calendar(std::move(opened),status);
    }
    if(name=="ucal_getCanonicalTimeZoneID" || name=="ucal_getWindowsTimeZoneID" || name=="ucal_getTimeZoneIDForWindowsID"){
        const ZoneText zone(memory,call(0),i(1),status.value);
        if(!zone.present || U_FAILURE(status.value))return fail();
        const bool from_windows=name=="ucal_getTimeZoneIDForWindowsID";
        const bool canonical=name=="ucal_getCanonicalTimeZoneID";
        const auto region=from_windows && call(2)?read_ansi(memory,call(2),INT32_MAX):std::string{};
        UBool system{};
        const auto result=icu_preserving_output<UChar>(memory,call(from_windows?3U:2U),i(from_windows?4U:3U),status,
            [&](auto* out,auto size,auto* error){
                if(canonical)return ucal_getCanonicalTimeZoneID(zone.get(),i(1),out,size,&system,error);
                if(from_windows)return ucal_getTimeZoneIDForWindowsID(zone.get(),i(1),call(2)?region.c_str():nullptr,out,size,error);
                return ucal_getWindowsTimeZoneID(zone.get(),i(1),out,size,error);
            });
        if(canonical && call(4) && (U_SUCCESS(status.value) || status.value==U_BUFFER_OVERFLOW_ERROR))
            memory.store(GuestAddress{call(4)},8,system!=0);
        return finish(result);
    }
    if(!calendar)return fail();
    if(name=="ucal_clone")return open_calendar(OwnedIcuCalendar(ucal_clone(calendar,&status.value),ucal_close),status);
    if(name=="ucal_getType" || name=="ucal_getLocaleByType"){
        const char* result=nullptr;
        if(name=="ucal_getType")result=ucal_getType(calendar,&status.value);
        else if(i(1)>=ULOC_ACTUAL_LOCALE && i(1)<=ULOC_VALID_LOCALE)
            result=ucal_getLocaleByType(calendar,static_cast<ULocDataLocaleType>(i(1)),&status.value);
        else return fail();
        status.commit();return finish(publish_icu_literal(memory,result));
    }
    if(name=="ucal_getTimeZoneID" || name=="ucal_getTimeZoneDisplayName"){
        const bool display=name=="ucal_getTimeZoneDisplayName";
        if(display && (i(1)<UCAL_STANDARD || i(1)>UCAL_SHORT_DST))return fail();
        const auto locale=display && call(2)?read_ansi(memory,call(2),INT32_MAX):std::string{};
        return finish(icu_preserving_output<UChar>(memory,call(display?3U:1U),i(display?4U:2U),status,[&](auto* out,auto size,auto* error){
            if(display)return ucal_getTimeZoneDisplayName(calendar,static_cast<UCalendarDisplayNameType>(i(1)),
                call(2)?locale.c_str():icu_default_locale(),out,size,error);
            return ucal_getTimeZoneID(calendar,out,size,error);
        }));
    }
    if(name=="ucal_setTimeZone"){
        if(!mutable_calendar)return fail();
        const ZoneText zone(memory,call(1),i(2),status.value);
        if(U_SUCCESS(status.value))ucal_setTimeZone(mutable_calendar,zone.present?zone.get():(default_zone?default_zone->c_str():nullptr),
            zone.present?i(2):-1,&status.value);
        status.commit();return finish(0);
    }
    if(name=="ucal_getTimeZoneTransitionDate"){
        if(!call(2) || i(1)<UCAL_TZ_TRANSITION_NEXT || i(1)>UCAL_TZ_TRANSITION_PREVIOUS_INCLUSIVE)return fail();
        UDate date{};
        const bool result=ucal_getTimeZoneTransitionDate(calendar,static_cast<UTimeZoneTransitionType>(i(1)),&date,&status.value)!=0;
        if(result && U_SUCCESS(status.value))memory.store(GuestAddress{call(2)},64,std::bit_cast<std::uint64_t>(date));
        status.commit();return finish(result);
    }
    if(name=="ucal_getTimeZoneOffsetFromLocal"){
        if(!call(3) || !call(4) || !local_option(i(1)) || !local_option(i(2)))return fail();
        std::int32_t raw{},dst{};
        ucal_getTimeZoneOffsetFromLocal(calendar,static_cast<UTimeZoneLocalOption>(i(1)),
            static_cast<UTimeZoneLocalOption>(i(2)),&raw,&dst,&status.value);
        if(U_SUCCESS(status.value)){
            memory.store(GuestAddress{call(3)},32,static_cast<std::uint32_t>(raw));
            memory.store(GuestAddress{call(4)},32,static_cast<std::uint32_t>(dst));
        }
        status.commit();return finish(0);
    }
    if(floating){
        const auto result=name=="ucal_getMillis"?ucal_getMillis(calendar,&status.value):ucal_getGregorianChange(calendar,&status.value);
        status.commit();return real(result);
    }
    if(name=="ucal_setMillis" || name=="ucal_setGregorianChange" || name=="ucal_isWeekend"){
        const auto date=icu_double_argument(state,call,x86_,1);
        if(name!="ucal_isWeekend" && !mutable_calendar)return fail();
        bool result=false;
        if(name=="ucal_setMillis")ucal_setMillis(mutable_calendar,date,&status.value);
        else if(name=="ucal_setGregorianChange")ucal_setGregorianChange(mutable_calendar,date,&status.value);
        else result=ucal_isWeekend(calendar,date,&status.value)!=0;
        status.commit();return finish(result);
    }
    if(name=="ucal_setDate" || name=="ucal_setDateTime"){
        if(!mutable_calendar)return fail();
        if(name=="ucal_setDate")ucal_setDate(mutable_calendar,i(1),i(2),i(3),&status.value);
        else ucal_setDateTime(mutable_calendar,i(1),i(2),i(3),i(4),i(5),i(6),&status.value);
        status.commit();return finish(0);
    }
    if(name=="ucal_inDaylightTime"){
        const bool result=ucal_inDaylightTime(calendar,&status.value)!=0;
        status.commit();return finish(result);
    }
    if(name=="ucal_getDayOfWeekType" || name=="ucal_getWeekendTransition"){
        if(i(1)<UCAL_SUNDAY || i(1)>UCAL_SATURDAY)return fail();
        const auto day=static_cast<UCalendarDaysOfWeek>(i(1));
        const auto result=name=="ucal_getDayOfWeekType"?static_cast<std::int32_t>(ucal_getDayOfWeekType(calendar,day,&status.value)):
            ucal_getWeekendTransition(calendar,day,&status.value);
        status.commit();return integer(result);
    }
    if(name=="ucal_getFieldDifference"){
        if(!mutable_calendar)return fail();
        if(!date_field(i(2)))return fail();
        const auto result=ucal_getFieldDifference(mutable_calendar,icu_double_argument(state,call,x86_,1),
            static_cast<UCalendarDateFields>(i(2)),&status.value);
        status.commit();return integer(result);
    }
    if(!date_field(i(1)))return fail();
    const auto field=static_cast<UCalendarDateFields>(i(1));
    std::int32_t result{};
    if(name=="ucal_add" || name=="ucal_roll"){
        if(!mutable_calendar)return fail();
        if(name=="ucal_add")ucal_add(mutable_calendar,field,i(2),&status.value);
        else ucal_roll(mutable_calendar,field,i(2),&status.value);
    }
    else if(name=="ucal_get")result=ucal_get(calendar,field,&status.value);
    else if(name=="ucal_getLimit"){
        if(i(2)<UCAL_MINIMUM || i(2)>UCAL_ACTUAL_MAXIMUM)return fail();
        result=ucal_getLimit(calendar,field,static_cast<UCalendarLimitType>(i(2)),&status.value);
    }else throw HostCallFault("ICU 日曆未登錄的服務");
    status.commit();return integer(result);
}
}
