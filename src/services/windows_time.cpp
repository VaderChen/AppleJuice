#include "x64arm/windows_services.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/system_performance.hpp"
#include "x64arm/unicode.hpp"
#include "crt_os_error.hpp"
#include "windows_locale_data.hpp"
#include "crt_time_format.hpp"
#include "crt_time_storage.hpp"
#include "monotonic_clock.hpp"
#include <algorithm>
#include <bit>
#include "x64arm/x87_fp.hpp"
extern "C" {
#include "softfloat.h"
}
#include <ctime>
#include <cstdio>
#include <chrono>
#include <locale.h>
#include <memory>

namespace x64arm {
std::span<const WindowsServiceDescriptor> time_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"crt","clock",0},
        {"crt","_ftime",1},{"crt","_ftime32",1},{"crt","_ftime64",1},
        {"crt","_ftime_s",1},{"crt","_ftime32_s",1},{"crt","_ftime64_s",1},
        {"crt","_utime",2},{"crt","_utime32",2},{"crt","_utime64",2},
        {"crt","_wutime",2},{"crt","_wutime32",2},{"crt","_wutime64",2},
        {"crt","_futime",2},{"crt","_futime32",2},{"crt","_futime64",2},
        {"crt","_strdate",1},{"crt","_strdate_s",2},{"crt","_wstrdate",1},{"crt","_wstrdate_s",2},
        {"crt","_strtime",1},{"crt","_strtime_s",2},{"crt","_wstrtime",1},{"crt","_wstrtime_s",2},
        {"crt","asctime",1},{"crt","_wasctime",1},{"crt","asctime_s",3},{"crt","_wasctime_s",3},
        {"crt","ctime",1},{"crt","_ctime32",1},{"crt","_ctime64",1},{"crt","ctime_s",3},{"crt","_ctime32_s",3},{"crt","_ctime64_s",3},
        {"crt","_wctime",1},{"crt","_wctime32",1},{"crt","_wctime64",1},{"crt","_wctime_s",3},{"crt","_wctime32_s",3},{"crt","_wctime64_s",3},
        {"msacm32.dll","acmStreamOpen",8},{"msacm32.dll","acmStreamClose",2},{"msacm32.dll","acmStreamSize",4},
        {"msacm32.dll","acmStreamPrepareHeader",3},{"msacm32.dll","acmStreamUnprepareHeader",3},{"msacm32.dll","acmStreamConvert",3},
        {"msacm32.dll","acmStreamReset",2},{"msacm32.dll","acmStreamMessage",4},{"msacm32.dll","acmFormatSuggest",5},
        {"msacm32.dll","acmGetVersion",0},{"msacm32.dll","acmMetrics",3},
        {"crt","_gmtime64",1},{"crt","_localtime64",1},{"crt","_gmtime64_s",2},{"crt","_localtime64_s",2},
        {"crt","_mkgmtime64",1},{"crt","_mktime64",1},{"crt","_time64",1},{"crt","_difftime64",2,3},
        {"crt","time",1},{"crt","_time32",1},{"crt","gmtime",1},{"crt","localtime",1},
        {"crt","gmtime_s",2},{"crt","localtime_s",2},{"crt","_gmtime32",1},{"crt","_localtime32",1},
        {"crt","_gmtime32_s",2},{"crt","_localtime32_s",2},{"crt","mktime",1},{"crt","_mkgmtime",1},
        {"crt","_mktime32",1},{"crt","_mkgmtime32",1},{"crt","difftime",2,3},{"crt","_difftime32",2},
        {"crt","strftime",4},{"crt","_tzset",0},{"crt","__timezone",0},{"crt","__daylight",0},{"crt","__tzname",0},
        {"crt","wcsftime",4},{"crt","_Strftime",5},{"crt","_Wcsftime",5},
        {"crt","_strftime_l",5},{"crt","_wcsftime_l",5},{"crt","_Strftime_l",6},{"crt","_Wcsftime_l",6},
        {"crt","_Getdays",0},{"crt","_Getmonths",0},{"crt","_W_Getdays",0},{"crt","_W_Getmonths",0},
        {"crt","_Gettnames",0},{"crt","_W_Gettnames",0},
        {"kernel32.dll","SystemTimeToTzSpecificLocalTime",3},{"kernel32.dll","TzSpecificLocalTimeToSystemTime",3},
        {"kernel32.dll","GetSystemTimes",3},{"kernel32.dll","GetDynamicTimeZoneInformation",1},{"kernel32.dll","GetTimeZoneInformation",1},{"kernel32.dll","GetSystemTime",1},{"kernel32.dll","GetLocalTime",1}
        ,{"kernel32.dll","SetSystemTime",1},{"kernel32.dll","GetSystemTimeAdjustment",3},{"kernel32.dll","SetSystemTimeAdjustment",2}
        ,{"kernel32.dll","FileTimeToLocalFileTime",2},{"kernel32.dll","LocalFileTimeToFileTime",2}
        ,{"kernel32.dll","FileTimeToSystemTime",2},{"kernel32.dll","SystemTimeToFileTime",2},{"kernel32.dll","CompareFileTime",2}
        ,{"kernel32.dll","FileTimeToDosDateTime",3},{"kernel32.dll","DosDateTimeToFileTime",3}
        ,{"kernel32.dll","QueryUnbiasedInterruptTime",1},{"kernel32.dll","QueryUnbiasedInterruptTimePrecise",1},{"kernel32.dll","GetTickCount64",0},{"kernel32.dll","GetSystemTimePreciseAsFileTime",1}
        ,{"winmm.dll","waveOutGetErrorTextA",3},{"winmm.dll","waveOutGetErrorTextW",3},{"winmm.dll","waveInGetErrorTextA",3},{"winmm.dll","waveInGetErrorTextW",3}
        ,{"winmm.dll","midiOutGetErrorTextA",3},{"winmm.dll","midiOutGetErrorTextW",3},{"winmm.dll","midiInGetErrorTextA",3},{"winmm.dll","midiInGetErrorTextW",3}
        ,{"winmm.dll","waveOutGetNumDevs",0},{"winmm.dll","waveOutGetDevCapsA",3},{"winmm.dll","waveOutGetDevCapsW",3}
        ,{"winmm.dll","waveOutOpen",6},{"winmm.dll","waveOutClose",1},{"winmm.dll","waveOutGetID",2}
        ,{"winmm.dll","waveOutPrepareHeader",3},{"winmm.dll","waveOutUnprepareHeader",3},{"winmm.dll","waveOutWrite",3}
        ,{"winmm.dll","waveOutPause",1},{"winmm.dll","waveOutRestart",1},{"winmm.dll","waveOutReset",1}
        ,{"winmm.dll","waveOutGetPosition",3},{"winmm.dll","waveOutGetVolume",2},{"winmm.dll","waveOutSetVolume",2}
        ,{"winmm.dll","waveOutMessage",4,0,0,2}
        ,{"winmm.dll","PlaySoundA",3,0,0,4},{"winmm.dll","PlaySoundW",3,0,0,4}
        ,{"winmm.dll","sndPlaySoundA",2,0,0,2},{"winmm.dll","sndPlaySoundW",2,0,0,2}
        ,{"winmm.dll","waveInGetNumDevs",0},{"winmm.dll","waveInGetDevCapsA",3},{"winmm.dll","waveInGetDevCapsW",3}
        ,{"winmm.dll","waveInOpen",6},{"winmm.dll","waveInClose",1},{"winmm.dll","waveInGetID",2}
        ,{"winmm.dll","waveInPrepareHeader",3},{"winmm.dll","waveInUnprepareHeader",3},{"winmm.dll","waveInAddBuffer",3}
        ,{"winmm.dll","waveInStart",1},{"winmm.dll","waveInStop",1},{"winmm.dll","waveInReset",1},{"winmm.dll","waveInGetPosition",3}
        ,{"winmm.dll","waveInMessage",4,0,0,2}
        ,{"winmm.dll","mixerGetNumDevs",0},{"winmm.dll","mixerGetDevCapsA",3,0,0,4},{"winmm.dll","mixerGetDevCapsW",3,0,0,4}
        ,{"winmm.dll","mixerOpen",5,0,0,18},{"winmm.dll","mixerClose",1},{"winmm.dll","mixerGetID",3,0,0,4},{"winmm.dll","mixerMessage",4,0,0,2}
        ,{"winmm.dll","mixerGetLineInfoA",3,0,0,4},{"winmm.dll","mixerGetLineInfoW",3,0,0,4}
        ,{"winmm.dll","mixerGetLineControlsA",3,0,0,4},{"winmm.dll","mixerGetLineControlsW",3,0,0,4}
        ,{"winmm.dll","mixerGetControlDetailsA",3,0,0,4},{"winmm.dll","mixerGetControlDetailsW",3,0,0,4},{"winmm.dll","mixerSetControlDetails",3,0,0,4}
        ,{"winmm.dll","midiOutGetNumDevs",0},{"winmm.dll","midiOutGetDevCapsA",3},{"winmm.dll","midiOutGetDevCapsW",3}
        ,{"winmm.dll","midiOutOpen",5},{"winmm.dll","midiOutClose",1},{"winmm.dll","midiOutGetID",2}
        ,{"winmm.dll","midiOutShortMsg",2},{"winmm.dll","midiOutLongMsg",3},{"winmm.dll","midiOutReset",1}
        ,{"winmm.dll","midiOutPrepareHeader",3},{"winmm.dll","midiOutUnprepareHeader",3}
        ,{"winmm.dll","midiInGetNumDevs",0},{"winmm.dll","midiInGetDevCapsA",3},{"winmm.dll","midiInGetDevCapsW",3}
        ,{"winmm.dll","midiInOpen",5},{"winmm.dll","midiInClose",1},{"winmm.dll","midiInGetID",2}
        ,{"winmm.dll","midiInPrepareHeader",3},{"winmm.dll","midiInUnprepareHeader",3},{"winmm.dll","midiInAddBuffer",3}
        ,{"winmm.dll","midiInStart",1},{"winmm.dll","midiInStop",1},{"winmm.dll","midiInReset",1}
        ,{"winmm.dll","timeGetTime",0},{"winmm.dll","timeGetDevCaps",2}
        ,{"winmm.dll","timeSetEvent",5},{"winmm.dll","timeKillEvent",1}
        ,{"winmm.dll","timeBeginPeriod",1},{"winmm.dll","timeEndPeriod",1}
        ,{"winmm.dll","mmioOpenA",3},{"winmm.dll","mmioOpenW",3},{"winmm.dll","mmioClose",2}
        ,{"winmm.dll","mmioGetInfo",3},{"winmm.dll","mmioSetInfo",3},{"winmm.dll","mmioAdvance",3}
        ,{"winmm.dll","mmioRead",3},{"winmm.dll","mmioWrite",3},{"winmm.dll","mmioSeek",3}
        ,{"winmm.dll","mmioDescend",4},{"winmm.dll","mmioAscend",3},{"winmm.dll","mmioCreateChunk",3}
    };
    return table;
}
namespace {
constexpr std::array<int std::tm::*,9> fields{&std::tm::tm_sec,&std::tm::tm_min,&std::tm::tm_hour,&std::tm::tm_mday,
    &std::tm::tm_mon,&std::tm::tm_year,&std::tm::tm_wday,&std::tm::tm_yday,&std::tm::tm_isdst};
std::tm read_tm(const GuestMemory& memory,std::uint64_t address) {
    memory.check(GuestAddress{address},36,Access::Read); std::tm result{};
    for (std::size_t i=0;i<fields.size();++i) result.*fields[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{address+i*4},32)));
    return result;
}
void write_tm(GuestMemory& memory,std::uint64_t address,const std::tm& value) {
    memory.check(GuestAddress{address},36,Access::Write);
    for (std::size_t i=0;i<fields.size();++i) memory.store(GuestAddress{address+i*4},32,static_cast<std::uint32_t>(value.*fields[i]));
}
std::int64_t read_time_value(const GuestMemory& memory,std::uint64_t address,bool narrow){
    if(!address)return -1;
    return narrow?static_cast<std::int64_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{address},32)))):std::bit_cast<std::int64_t>(memory.load(GuestAddress{address},64));
}
bool crt_calendar(std::int64_t seconds,std::int64_t maximum,std::tm& value){
    const auto stamp=static_cast<std::time_t>(seconds);return seconds>=0 && seconds<=maximum && ::gmtime_r(&stamp,&value);
}
bool valid_date(unsigned year,unsigned month,unsigned day,unsigned hour,unsigned minute,unsigned second,unsigned millisecond=0){
    if(year<1601 || year>30827 || !month || month>12 || !day || hour>23 || minute>59 || second>59 || millisecond>999)return false;
    constexpr std::array<unsigned,12> days{31,28,31,30,31,30,31,31,30,31,30,31};
    return day<=days[month-1]+(month==2 && year%4==0 && (year%100!=0 || year%400==0)?1U:0U);
}
using SystemTime=std::array<unsigned,8>;
SystemTime system_time(const GuestMemory& memory,std::uint64_t address){SystemTime result{};for(unsigned i=0;i<8;++i)result[i]=static_cast<unsigned>(memory.load(GuestAddress{address+i*2},16));return result;}
std::optional<std::int64_t> system_seconds(const SystemTime& f){
    if(!valid_date(f[0],f[1],f[3],f[4],f[5],f[6],f[7]))return {};
    std::tm value{};value.tm_year=static_cast<int>(f[0])-1900;value.tm_mon=static_cast<int>(f[1])-1;value.tm_mday=static_cast<int>(f[3]);value.tm_hour=static_cast<int>(f[4]);value.tm_min=static_cast<int>(f[5]);value.tm_sec=static_cast<int>(f[6]);return static_cast<std::int64_t>(::timegm(&value));
}
struct Zone {std::int64_t bias{},standard_bias{},daylight_bias{};SystemTime standard{},daylight{};};
std::optional<std::int64_t> transition(SystemTime date,unsigned year){
    if(!date[0]){
        if(date[1]<1 || date[1]>12 || date[2]>6 || date[3]<1 || date[3]>5)return {};
        const auto ordinal=date[3];date[0]=year;date[3]=1;const auto first=system_seconds(date);if(!first)return {};
        const auto stamp=static_cast<std::time_t>(*first);std::tm value{};if(!::gmtime_r(&stamp,&value))return {};
        date[3]=1+(date[2]+7-static_cast<unsigned>(value.tm_wday))%7+7*(ordinal-1);
        while(!valid_date(year,date[1],date[3],0,0,0) && date[3]>7)date[3]-=7;
    }
    return system_seconds(date);
}
std::optional<std::int64_t> zone_local(std::int64_t utc,const Zone& zone){
    const auto standard=utc-(zone.bias+zone.standard_bias)*60;
    if(!zone.standard[1] && !zone.daylight[1])return utc-zone.bias*60;
    if(!zone.standard[1] || !zone.daylight[1])return {};
    const auto stamp=static_cast<std::time_t>(standard);std::tm date{};if(!::gmtime_r(&stamp,&date))return {};
    const auto year=static_cast<unsigned>(date.tm_year+1900);if(year<1601 || year>30827)return {};
    const auto local_start=transition(zone.daylight,year),local_end=transition(zone.standard,year);if(!local_start || !local_end)return {};
    const auto start=*local_start+(zone.bias+zone.standard_bias)*60,end=*local_end+(zone.bias+zone.daylight_bias)*60;
    const bool daylight=start<end?(utc>=start && utc<end):(utc>=start || utc<end);
    return utc-(zone.bias+(daylight?zone.daylight_bias:zone.standard_bias))*60;
}
void write_system_time(GuestMemory& memory,std::uint64_t address,const std::tm& value,unsigned milliseconds){
    const std::array<unsigned,8> out{static_cast<unsigned>(value.tm_year+1900),static_cast<unsigned>(value.tm_mon+1),static_cast<unsigned>(value.tm_wday),static_cast<unsigned>(value.tm_mday),static_cast<unsigned>(value.tm_hour),static_cast<unsigned>(value.tm_min),static_cast<unsigned>(value.tm_sec),milliseconds};
    memory.check(GuestAddress{address},16,Access::Write);for(unsigned i=0;i<8;++i)memory.store(GuestAddress{address+i*2},16,out[i]);
}
}
std::optional<std::uint32_t> WindowsServices::invoke_time(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(service.symbol=="SetSystemTime" || service.symbol=="GetSystemTimeAdjustment" || service.symbol=="SetSystemTimeAdjustment"){
        const WindowsCall call(state,memory,service);
        const auto finish=[&](unsigned value,unsigned error=0){if(error)last_error_=error;call.return_to(state,value);return std::optional<std::uint32_t>{};};
        try{
        if(service.symbol=="GetSystemTimeAdjustment"){
            for(unsigned i=0;i<3;++i)if(!call(i))return finish(0,87);
            for(unsigned i=0;i<3;++i)memory.check(GuestAddress{call(i)},4,Access::Write);
            const auto time=runtime_.system_time_state();memory.store(GuestAddress{call(0)},32,time.adjustment);
            memory.store(GuestAddress{call(1)},32,time.increment);
            memory.store(GuestAddress{call(2)},32,time.disabled);return finish(1);
        }
        refresh_file_security_context();
        const auto& token=runtime_.token_objects.at(effective_token_id());
        const bool setting=service.symbol=="SetSystemTime";
        if(!token.has_privilege(12,!setting))return finish(0,1314);
        auto now=runtime_.system_filetime();
        if(setting){
            if(!call(0))return finish(0,87);memory.check(GuestAddress{call(0)},16,Access::Read);
            const auto time_fields=system_time(memory,call(0));const auto seconds=system_seconds(time_fields);
            if(!seconds || *seconds < -INT64_C(11644473600))return finish(0,87);
            const auto value=static_cast<std::uint64_t>(*seconds+INT64_C(11644473600));
            if(value>INT64_MAX/10000000U)return finish(0,87);
            now=value*10000000U+time_fields[7]*10000U;if(now>INT64_MAX)return finish(0,87);
            if(runtime_.machine_clock)runtime_.machine_clock->set_time(runtime_.clock_ticks,now);
        }else{
            const auto adjustment=static_cast<std::uint32_t>(call(0));const bool disabled=static_cast<std::uint32_t>(call(1))!=0;
            if(runtime_.machine_clock)runtime_.machine_clock->set_adjustment(runtime_.clock_ticks,adjustment,disabled);
            runtime_.system_time_adjustment_disabled=disabled;
            runtime_.system_time_adjustment=disabled?runtime_.system_time_increment:adjustment;
        }
        if(runtime_.machine_clock){const auto time=runtime_.system_time_state();now=time.filetime;runtime_.system_time_increment=time.increment;runtime_.system_time_adjustment=time.adjustment;runtime_.system_time_adjustment_disabled=time.disabled;}
        // 僅修改 Windows 虛擬 UTC，單調時計、QPC、CPU 計時及宿主時鐘均保留。
        runtime_.filetime_base=now-runtime_.clock_ticks;
        runtime_.system_time_anchor_ticks=runtime_.clock_ticks;
        runtime_.system_time_anchor_filetime=now;
        return finish(1);
        }catch(const SecurityDescriptorError& error){return finish(0,error.code);}catch(const MemoryFault&){return finish(0,998);}catch(const std::bad_alloc&){return finish(0,1450);}
    }
    if(service.symbol=="clock"){
        const WindowsCall arg(state,memory,service);
        const auto origin=runtime_.process_creation_clock_ticks;
        const auto elapsed=runtime_.clock_ticks>=origin?(runtime_.clock_ticks-origin)/10000:0;
        arg.return_to(state,elapsed>INT32_MAX?UINT32_MAX:elapsed);return std::nullopt;
    }
    if(service.family=="msacm32.dll")return invoke_acm(state,memory,service);
    if(service.symbol=="timeSetEvent" || service.symbol=="timeKillEvent")return invoke_timer_queue(state,memory,service);
    if(service.family=="winmm.dll" && service.symbol.find("GetErrorText")!=service.symbol.npos) {
        const WindowsCall arg(state,memory,service);const auto code=static_cast<std::uint32_t>(arg(0)),capacity=static_cast<std::uint32_t>(arg(2));
        static constexpr std::pair<unsigned,std::u16string_view> errors[]{
            {0,u"No error."},{1,u"Unspecified multimedia error."},{2,u"The device identifier is out of range."},
            {3,u"The driver failed to enable."},{4,u"The device is already in use."},{5,u"The device handle is invalid."},
            {6,u"No driver is installed."},{7,u"Not enough memory is available."},{8,u"The function is not supported."},
            {9,u"The error number is invalid."},{10,u"The specified flag is invalid."},{11,u"The specified parameter is invalid."},
            {12,u"The handle is busy."},{13,u"The specified alias was not found."},{14,u"The registry database is invalid."},
            {15,u"The registry key was not found."},{16,u"The registry key could not be opened."},
            {17,u"The registry key could not be read."},{18,u"The registry key could not be written."},
            {19,u"The registry key could not be deleted."},{20,u"The registry value was not found."},
            {21,u"The driver does not call DriverCallback."},{32,u"The waveform format is not supported."},
            {33,u"The audio device is still playing."},{34,u"The audio buffer has not been prepared."},
            {35,u"The audio device is synchronous."},{64,u"The MIDI buffer has not been prepared."},
            {65,u"The MIDI device is still playing."},{66,u"The MIDI map was not found."},
            {67,u"The MIDI device is not ready."},{68,u"No synthesizer is installed."},
            {69,u"The synthesizer cannot play the required note."},{70,u"The MIDI port is in use."},
            {71,u"The synthesizer may play only part of the required notes."}};
        const auto found=std::find_if(std::begin(errors),std::end(errors),[&](const auto& item){return item.first==code;});
        std::uint32_t result=0;
        if(found==std::end(errors))result=9;
        else if(!arg(1) || !capacity)result=11;
        else {
            const bool wide=service.symbol.ends_with('W');const auto count=std::min<std::size_t>(found->second.size(),capacity-1);
            const unsigned unit=wide?2U:1U;memory.check(GuestAddress{arg(1)},(count+1)*unit,Access::Write);
            for(std::size_t i=0;i<count;++i)memory.store(GuestAddress{arg(1)+i*unit},static_cast<std::uint8_t>(unit*8),found->second[i]);
            memory.store(GuestAddress{arg(1)+count*unit},static_cast<std::uint8_t>(unit*8),0);
        }
        arg.return_to(state,result);return std::nullopt;
    }
    if(service.symbol.starts_with("mixer"))return invoke_winmm_mixer(state,memory,service);
    if(service.symbol.starts_with("mmio"))return invoke_mmio(state,memory,service);
    if(service.symbol.starts_with("waveOut"))return invoke_wave_output(state,memory,service);
    if(service.symbol.starts_with("PlaySound") || service.symbol.starts_with("sndPlaySound"))return invoke_play_sound(state,memory,service);
    if(service.symbol.starts_with("waveIn"))return invoke_wave_input(state,memory,service);
    if(service.symbol.starts_with("midiIn"))return invoke_midi_input(state,memory,service);
    if(service.symbol.starts_with("midiOut"))return invoke_winmm_midi(state,memory,service);
    const WindowsCall arg(state,memory,service); const auto symbol=service.symbol;
    if(symbol=="_Getdays" || symbol=="_W_Getdays" || symbol=="_Getmonths" || symbol=="_W_Getmonths" || symbol=="_Gettnames" || symbol=="_W_Gettnames"){
        const auto& category=current_crt_locale(state,memory).categories[5];const auto data=windows_crt_time_data(category);std::vector<std::uint8_t> bytes;
        crt_time::Storage snapshot;
        if(symbol.ends_with("tnames")){snapshot=crt_time::encode(data,category.codepage,x86_?4:8);bytes=snapshot.bytes;}
        else{std::u16string text;const bool days=symbol.ends_with("days");for(unsigned i=0;i<(days?7U:12U);++i){text+=u':';text+=data.fields[(days?0:14)+i];text+=u':';text+=data.fields[(days?7:26)+i];}
            if(symbol.starts_with("_W_"))bytes=utf16_bytes(text);else{const auto value=windows_crt_encode(text,category.codepage);if(!value){memory.store(GuestAddress{runtime_.errno_cell},32,42);arg.return_to(state,0);return std::nullopt;}bytes=*value;bytes.push_back(0);}}
        const auto pointer=allocate_heap(memory,bytes.size());if(pointer){try{if(symbol.ends_with("tnames")){snapshot.relocate(pointer);memory.write(GuestAddress{pointer},snapshot.bytes);}else memory.write(GuestAddress{pointer},bytes);}catch(...){(void)free_heap(memory,pointer,state);throw;}}
        else memory.store(GuestAddress{runtime_.errno_cell},32,12);arg.return_to(state,pointer);return std::nullopt;
    }
    if(symbol=="GetSystemTimes"){
        SystemCpuTime times;
        if(runtime_.host_clock)times=native_system_cpu_time();
        else {
            // 虛擬時鐘一個 retired instruction 為一個 tick；所有客體執行緒共用此計數。
            for(const auto& [id,thread]:threads_.items){(void)id;times.user+=thread.cycles;}
            times.user=std::min(times.user,runtime_.clock_ticks);
            times.idle=runtime_.clock_ticks-times.user;times.kernel=times.idle;
        }
        if(times.error){last_error_=times.error;arg.return_to(state,0);return std::nullopt;}
        const std::array values{times.idle,times.kernel,times.user};
        for(unsigned i=0;i<3;++i)if(arg(i))memory.check(GuestAddress{arg(i)},8,Access::Write);
        for(unsigned i=0;i<3;++i)if(arg(i))memory.store(GuestAddress{arg(i)},64,values[i]);
        arg.return_to(state,1);return std::nullopt;
    }
    if(symbol=="QueryUnbiasedInterruptTime" || symbol=="QueryUnbiasedInterruptTimePrecise") {
        if(!arg(0)){last_error_=87;arg.return_to(state,0);return std::nullopt;}
        // 宿主時鐘直接讀不含睡眠期間的硬體時間；virtual 時鐘沿用指令推進的時間軸。
        const auto ticks=runtime_.host_clock?detail::unbiased_ticks():runtime_.clock_ticks;
        memory.store(GuestAddress{arg(0)},64,ticks);arg.return_to(state,symbol=="QueryUnbiasedInterruptTime"?1:0);return std::nullopt;
    }
    if (service.family=="winmm.dll" || symbol=="GetTickCount64" || symbol=="GetSystemTimePreciseAsFileTime") {
        std::uint64_t result=0;
        if (symbol=="timeGetTime") result=static_cast<std::uint32_t>(runtime_.clock_ticks/10000);
        else if (symbol=="GetTickCount64") result=runtime_.clock_ticks/10000;
        else if (symbol=="GetSystemTimePreciseAsFileTime") memory.store(GuestAddress{arg(0)},64,runtime_.system_filetime());
        else if (symbol=="timeGetDevCaps") {
            if (!arg(0) || static_cast<std::uint32_t>(arg(1))<8) result=97;
            else { memory.check(GuestAddress{arg(0)},8,Access::Write);memory.store(GuestAddress{arg(0)},32,1);memory.store(GuestAddress{arg(0)+4},32,65535); }
        } else {
            const auto period=static_cast<std::uint32_t>(arg(0));
            if (!period || period>65535) result=97;
            else if (symbol=="timeBeginPeriod") {
                auto& count=runtime_.timer_periods[period];
                if(count==UINT32_MAX)result=97;else ++count;
            } else {
                const auto found=runtime_.timer_periods.find(period);
                if(found==runtime_.timer_periods.end())result=97;
                else if(!--found->second)runtime_.timer_periods.erase(found);
            }
        }
        arg.return_to(state,result,symbol=="GetTickCount64");return std::nullopt;
    }
    const std::uint64_t timezone_cell=runtime_base_.value+80, daylight_cell=runtime_base_.value+84, names=runtime_base_.value+88;
    if (!runtime_.timezone_name) {
        runtime_.timezone_name=runtime_allocate(memory,std::array<std::uint8_t,4>{'U','T','C',0});
        memory.store(GuestAddress{names},x86_?32:64,runtime_.timezone_name);memory.store(GuestAddress{names+(x86_?4U:8U)},x86_?32:64,runtime_.timezone_name);
    }
    const bool narrow_time=symbol.find("32")!=symbol.npos;
    const auto maximum=narrow_time?INT64_C(2147483647):INT64_C(32535215999);
    bool invalid_parameter=false,wide_result=false;
    std::uint64_t result=0;
    const auto error=[&](std::uint64_t value) {memory.store(GuestAddress{runtime_.errno_cell},32,22);result=value;};
    // 同一執行緒的 tm、窄字串及寬字串共用配置與退出時釋放流程；各自保留結果。
    const auto time_buffer=[&](){auto& buffer=runtime_.tm_buffers[threads_.current];if(!buffer)buffer=allocate_heap(memory,36+26+52);return buffer;};
    if(symbol.starts_with("_ftime")){
        const bool secure=symbol.ends_with("_s");const auto width=narrow_time?4U:8U;
        const auto ticks=runtime_.system_filetime();constexpr auto epoch=UINT64_C(116444736000000000);
        if(!arg(0)){error(secure?22:0);invalid_parameter=true;}
        else if(ticks<epoch || (ticks-epoch)/10000000>static_cast<std::uint64_t>(maximum))error(secure?22:0);
        else {
            memory.check(GuestAddress{arg(0)},width+6U,Access::Write);
            memory.store(GuestAddress{arg(0)},static_cast<std::uint8_t>(width*8),(ticks-epoch)/10000000);
            memory.store(GuestAddress{arg(0)+width},16,((ticks-epoch)/10000)%1000);
            const auto timezone=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{timezone_cell},32)));
            memory.store(GuestAddress{arg(0)+width+2},16,static_cast<std::uint16_t>(timezone/60));
            memory.store(GuestAddress{arg(0)+width+4},16,0); // 共用既有客體 UTC 時區，沒有夏令時間。
        }
    }else if(symbol.starts_with("_utime") || symbol.starts_with("_wutime") || symbol.starts_with("_futime")){
        const bool descriptor=symbol.starts_with("_f"),wide=symbol.starts_with("_w");
        std::int64_t accessed{},modified{};const auto ticks=runtime_.system_filetime();
        if(arg(1)){
            const auto width=narrow_time?4U:8U;memory.check(GuestAddress{arg(1)},width*2U,Access::Read);
            accessed=read_time_value(memory,arg(1),narrow_time);modified=read_time_value(memory,arg(1)+width,narrow_time);
        }else accessed=modified=ticks>=UINT64_C(116444736000000000)?static_cast<std::int64_t>((ticks-UINT64_C(116444736000000000))/10000000):-1;
        if(accessed<0 || modified<0 || accessed>maximum || modified>maximum){error(UINT32_MAX);invalid_parameter=true;}
        else {
            const auto filetime=[](std::int64_t seconds){return UINT64_C(116444736000000000)+static_cast<std::uint64_t>(seconds)*10000000;};
            std::shared_ptr<HostFile> native;std::uint32_t code=0;bool bad=false;
            if(descriptor){
                auto* file=crt_file(crt_descriptor_stream(static_cast<std::uint32_t>(arg(0))));
                if(!file || !file->native){code=6;bad=true;}else if(!file->writable)code=5;else native=file->native;
            }else if(!arg(0)){code=87;bad=true;}
            else if(runtime_.descriptors.size()+3U>=8192)code=4;
            else {
                const auto path=wide?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
                const auto info=filesystem_.information(path);
                if(info.error)code=info.error;
                else if((info.metadata.attributes&0x11U))code=5;
                else {const auto opened=filesystem_.open_host(path,0x40000000,3,0x80,7);native=opened.file;code=opened.error;}
            }
            if(!code)code=native->set_times(0,filetime(accessed),filetime(modified));
            if(code){crt_os_error::store(memory,runtime_.errno_cell,code);result=UINT32_MAX;invalid_parameter=bad;}
        }
    }else if(symbol.starts_with("_strdate") || symbol.starts_with("_wstrdate") || symbol.starts_with("_strtime") || symbol.starts_with("_wstrtime")){
        const bool secure=symbol.ends_with("_s"),wide=symbol.starts_with("_w"),date=symbol.find("date")!=symbol.npos;
        const auto capacity=secure?arg(1):UINT64_C(9);const auto unit=wide?2U:1U;
        if(secure && arg(0) && capacity)memory.store(GuestAddress{arg(0)},static_cast<std::uint8_t>(unit*8),0);
        if(!arg(0) || capacity<9){const auto code=!arg(0) || !capacity?22U:34U;memory.store(GuestAddress{runtime_.errno_cell},32,code);result=secure?code:0;invalid_parameter=true;}
        else {
            const auto ticks=runtime_.system_filetime();
            const auto stamp=static_cast<std::time_t>(static_cast<std::int64_t>(ticks/10000000)-INT64_C(11644473600));std::tm value{};
            if(!::gmtime_r(&stamp,&value))error(secure?22:0);
            else {
                std::array<char,9> text{};const auto count=date?std::snprintf(text.data(),text.size(),"%02d/%02d/%02d",value.tm_mon+1,value.tm_mday,(value.tm_year+1900)%100):std::snprintf(text.data(),text.size(),"%02d:%02d:%02d",value.tm_hour,value.tm_min,value.tm_sec);
                if(count!=8)error(secure?22:0);
                else {memory.check(GuestAddress{arg(0)},unit*9U,Access::Write);for(unsigned i=0;i<9;++i)memory.store(GuestAddress{arg(0)+i*unit},static_cast<std::uint8_t>(unit*8),static_cast<unsigned char>(text[i]));result=secure?0:arg(0);}
            }
        }
    }else if(symbol=="asctime" || symbol=="asctime_s" || symbol.starts_with("_wasctime") || symbol=="ctime" || symbol=="ctime_s" || symbol.starts_with("_ctime") || symbol.starts_with("_wctime")){
        const bool secure=symbol.ends_with("_s"),wide=symbol.starts_with("_w"),ascii=symbol.find("asctime")!=symbol.npos;
        const auto source=arg(secure?2:0);std::tm value{};bool valid=source!=0,invoke_invalid=!source;
        auto destination=secure?arg(0):0;
        if(secure && destination && arg(1))memory.store(GuestAddress{destination},wide?16:8,0);
        if(secure && (!destination || arg(1)<26)){valid=false;invoke_invalid=true;}
        if(valid && ascii){
            value=read_tm(memory,source);
            valid=value.tm_year>=0 && value.tm_year<=8099 && value.tm_mon>=0 && value.tm_mon<12 && value.tm_mday>=1 && value.tm_mday<=31 && value.tm_wday>=0 && value.tm_wday<7 && value.tm_hour>=0 && value.tm_hour<24 && value.tm_min>=0 && value.tm_min<60 && value.tm_sec>=0 && value.tm_sec<60;
            invoke_invalid=!valid;
        }else if(valid){
            const auto seconds=read_time_value(memory,source,narrow_time);valid=crt_calendar(seconds,maximum,value);invoke_invalid=seconds<0;
        }
        if(!valid){error(secure?22:0);invalid_parameter=invoke_invalid;}
        else {
            if(!secure){const auto buffer=time_buffer();if(buffer){destination=buffer+36+(wide?26:0);if(!ascii)write_tm(memory,buffer,value);}}
            if(!destination){memory.store(GuestAddress{runtime_.errno_cell},32,12);result=secure?12:0;}
            else {
                constexpr std::array days{"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
                constexpr std::array months{"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
                std::array<char,26> text{};
                std::snprintf(text.data(),text.size(),"%s %s %2d %02d:%02d:%02d %04d\n",days[static_cast<unsigned>(value.tm_wday)],months[static_cast<unsigned>(value.tm_mon)],value.tm_mday,value.tm_hour,value.tm_min,value.tm_sec,value.tm_year+1900);
                if(wide)memory.write(GuestAddress{destination},utf16_bytes(utf8_to_utf16(std::string_view(text.data(),25))));
                else memory.write(GuestAddress{destination},std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
                result=secure?0:destination;
            }
        }
    }else if(symbol=="SystemTimeToTzSpecificLocalTime" || symbol=="TzSpecificLocalTimeToSystemTime"){
        if(!arg(1) || !arg(2))last_error_=87;
        else {
            const auto input=system_time(memory,arg(1));const auto seconds=system_seconds(input);Zone zone;
            if(arg(0)){
                memory.check(GuestAddress{arg(0)},172,Access::Read);
                const auto bias=[&](unsigned offset){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+offset},32)));};
                zone.bias=bias(0);zone.standard_bias=bias(84);zone.daylight_bias=bias(168);zone.standard=system_time(memory,arg(0)+68);zone.daylight=system_time(memory,arg(0)+152);
            } // 未指定時區時，使用與 GetTimeZoneInformation 相同的客體 UTC。
            std::optional<std::int64_t> converted;
            if(seconds){
                if(symbol=="SystemTimeToTzSpecificLocalTime")converted=zone_local(*seconds,zone);
                else {
                    // 秋季重複時間優先標準時間；春季不存在的牆鐘時間必須失敗。
                    for(auto bias:{zone.bias+(zone.standard[1]?zone.standard_bias:0),zone.bias+zone.daylight_bias}){
                        const auto utc=*seconds+bias*60;const auto local=zone_local(utc,zone);if(local && *local==*seconds){converted=utc;break;}
                    }
                }
            }
            const auto stamp=static_cast<std::time_t>(converted.value_or(0));std::tm value{};
            if(!converted || !::gmtime_r(&stamp,&value) || value.tm_year<1601-1900 || value.tm_year>30827-1900)last_error_=87;
            else {write_system_time(memory,arg(2),value,input[7]);result=1;}
        }
    }else if(symbol=="CompareFileTime"){
        const auto left=memory.load(GuestAddress{arg(0)},64),right=memory.load(GuestAddress{arg(1)},64);result=left==right?0:left<right?UINT32_MAX:1;
    }else if(symbol=="FileTimeToLocalFileTime" || symbol=="LocalFileTimeToFileTime"){
        const auto ticks=memory.load(GuestAddress{arg(0)},64);if(ticks>>63)last_error_=87;else {memory.store(GuestAddress{arg(1)},64,ticks);result=1;}
    }else if(symbol=="FileTimeToSystemTime" || symbol=="FileTimeToDosDateTime"){
        const auto ticks=memory.load(GuestAddress{arg(0)},64);const auto seconds=static_cast<std::time_t>(static_cast<std::int64_t>(ticks/10000000)-INT64_C(11644473600));std::tm value{};
        if(ticks>>63 || !::gmtime_r(&seconds,&value))last_error_=87;
        else if(symbol=="FileTimeToSystemTime"){write_system_time(memory,arg(1),value,static_cast<unsigned>((ticks/10000)%1000));result=1;}
        else if(value.tm_year<80 || value.tm_year>207)last_error_=87;
        else {memory.check(GuestAddress{arg(1)},2,Access::Write);memory.check(GuestAddress{arg(2)},2,Access::Write);
            const auto date=(static_cast<unsigned>(value.tm_year-80)<<9)|(static_cast<unsigned>(value.tm_mon+1)<<5)|static_cast<unsigned>(value.tm_mday);
            const auto time=(static_cast<unsigned>(value.tm_hour)<<11)|(static_cast<unsigned>(value.tm_min)<<5)|static_cast<unsigned>(value.tm_sec/2);
            memory.store(GuestAddress{arg(1)},16,date);memory.store(GuestAddress{arg(2)},16,time);result=1;}
    }else if(symbol=="SystemTimeToFileTime" || symbol=="DosDateTimeToFileTime"){
        std::array<unsigned,8> fields_in{};const bool system=symbol=="SystemTimeToFileTime";
        if(system)for(unsigned i=0;i<8;++i)fields_in[i]=static_cast<unsigned>(memory.load(GuestAddress{arg(0)+i*2},16));
        else {const auto date=static_cast<std::uint16_t>(arg(0)),time=static_cast<std::uint16_t>(arg(1));fields_in={1980U+(date>>9),static_cast<unsigned>((date>>5)&15),0,static_cast<unsigned>(date&31),static_cast<unsigned>(time>>11),static_cast<unsigned>((time>>5)&63),static_cast<unsigned>((time&31)*2),0};}
        if(!valid_date(fields_in[0],fields_in[1],fields_in[3],fields_in[4],fields_in[5],fields_in[6],fields_in[7]))last_error_=87;
        else {std::tm value{};value.tm_year=static_cast<int>(fields_in[0])-1900;value.tm_mon=static_cast<int>(fields_in[1])-1;value.tm_mday=static_cast<int>(fields_in[3]);value.tm_hour=static_cast<int>(fields_in[4]);value.tm_min=static_cast<int>(fields_in[5]);value.tm_sec=static_cast<int>(fields_in[6]);
            const auto seconds=static_cast<std::int64_t>(::timegm(&value))+INT64_C(11644473600);const auto ticks=static_cast<std::uint64_t>(seconds)*10000000+fields_in[7]*10000;
            if(seconds<0 || ticks>>63)last_error_=87;else {memory.store(GuestAddress{arg(system?1:2)},64,ticks);result=1;}
        }
    }else if (symbol=="__timezone") result=timezone_cell;
    else if (symbol=="__daylight") result=daylight_cell;
    else if (symbol=="__tzname") result=names;
    else if (symbol=="_tzset") { memory.store(GuestAddress{timezone_cell},32,0); memory.store(GuestAddress{daylight_cell},32,0); }
    else if (symbol=="GetTimeZoneInformation" || symbol=="GetDynamicTimeZoneInformation") {
        if(!arg(0)){last_error_=87;arg.return_to(state,UINT32_MAX);return std::nullopt;}
        const bool dynamic=symbol=="GetDynamicTimeZoneInformation";
        std::vector<std::uint8_t> bytes(dynamic?432U:172U);
        for (unsigned offset : {4U,88U}) {bytes[offset]='U';bytes[offset+2]='T';bytes[offset+4]='C';}
        // 與既有客體 UTC 時區一致：無轉換日期，動態規則沒有停用。
        if(dynamic){bytes[172]='U';bytes[174]='T';bytes[176]='C';}
        memory.write(GuestAddress{arg(0)},bytes);
    } else if (symbol=="GetSystemTime" || symbol=="GetLocalTime") {
        // 客體時區為 UTC；使用與 FILETIME／CRT 相同的虛擬時鐘。
        const auto ticks=runtime_.system_filetime();
        const auto stamp=static_cast<std::time_t>(static_cast<std::int64_t>(ticks/10000000)-INT64_C(11644473600));std::tm value{};
        if(!::gmtime_r(&stamp,&value))throw HostCallFault("SYSTEMTIME 超出宿主可表示的日期");
        const std::array<unsigned,8> fields_out{static_cast<unsigned>(value.tm_year+1900),static_cast<unsigned>(value.tm_mon+1),static_cast<unsigned>(value.tm_wday),static_cast<unsigned>(value.tm_mday),static_cast<unsigned>(value.tm_hour),static_cast<unsigned>(value.tm_min),static_cast<unsigned>(value.tm_sec),static_cast<unsigned>((ticks/10000)%1000)};
        memory.check(GuestAddress{arg(0)},16,Access::Write);
        for(unsigned i=0;i<fields_out.size();++i)memory.store(GuestAddress{arg(0)+i*2},16,fields_out[i]);
    } else if (symbol=="_time64" || symbol=="_time32" || symbol=="time") {
        const auto ticks=runtime_.system_filetime();
        if(ticks<UINT64_C(116444736000000000))error(UINT64_MAX);
        else {result=(ticks-UINT64_C(116444736000000000))/10000000;if(result>static_cast<std::uint64_t>(maximum))error(UINT64_MAX);}
        if(arg(0))memory.store(GuestAddress{arg(0)},narrow_time?32:64,result);wide_result=!narrow_time;
    } else if (symbol=="_difftime64" || symbol=="_difftime32" || symbol=="difftime") {
        const auto seconds=[&](unsigned i){return narrow_time?static_cast<std::int64_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(i)))):std::bit_cast<std::int64_t>(arg(i));};
        const auto end=seconds(0),start=seconds(1);double value=0;
        if(end<=0 || start<=0 || end>maximum || start>maximum){error(0);invalid_parameter=true;}
        else value=static_cast<double>(end-start);
        const auto raw=std::bit_cast<std::uint64_t>(value);
        if(x86_){const auto top=((state.x87_status>>11)+7U)&7U;const auto out=f64_to_extF80({raw});state.x87[top]={out.signif,out.signExp};state.x87_status=static_cast<std::uint16_t>((state.x87_status&~0x3800U)|(top<<11));state.x87_tag=static_cast<std::uint16_t>((state.x87_tag&~(3U<<(top*2)))|(x87_tag(state.x87[top])<<(top*2)));}
        else for(unsigned i=0;i<8;++i)state.xmm[0][i]=static_cast<std::uint8_t>(raw>>(i*8));
    } else if (symbol.starts_with("_mkgmtime") || symbol.starts_with("_mktime") || symbol=="mktime") {
        wide_result=!narrow_time;
        if(!arg(0)){error(UINT64_MAX);invalid_parameter=true;}
        else {
            auto value=read_tm(memory,arg(0));memory.check(GuestAddress{arg(0)},36,Access::Write);
            const auto seconds=::timegm(&value); // 客體預設時區 UTC，與宿主 TZ 無關。
            if(seconds<0 || seconds>maximum)error(UINT64_MAX);
            else {write_tm(memory,arg(0),value);result=static_cast<std::uint64_t>(seconds);}
        }
    } else if (symbol=="strftime" || symbol=="wcsftime" || symbol=="_Strftime" || symbol=="_Wcsftime" || symbol=="_strftime_l" || symbol=="_wcsftime_l" || symbol=="_Strftime_l" || symbol=="_Wcsftime_l") {
        const bool wide=symbol.find("wcsftime")!=symbol.npos || symbol.find("Wcsftime")!=symbol.npos;
        const bool private_snapshot=symbol=="_Strftime" || symbol=="_Wcsftime" || symbol=="_Strftime_l" || symbol=="_Wcsftime_l";
        if (arg(1)>output_limit/(wide?4U:1U)) throw HostCallFault("strftime buffer 超過上限");
        if(arg(0) && arg(1))memory.store(GuestAddress{arg(0)},wide?16:8,0);
        if (!arg(0) || !arg(1) || !arg(2) || !arg(3)){error(0);invalid_parameter=true;}
        else {
            const auto value=read_tm(memory,arg(3));auto category=current_crt_locale(state,memory).categories[5];
            if(symbol.ends_with("_l") && arg(private_snapshot?5:4)){
                const auto locale=runtime_.crt_locales.find(arg(private_snapshot?5:4));
                if(locale==runtime_.crt_locales.end()){error(0);invalid_parameter=true;}else category=locale->second.categories[5];
            }
            if(!invalid_parameter){
                const auto data=private_snapshot && arg(4)?crt_time::read(memory,arg(4),x86_?4:8):windows_crt_time_data(category);
                const auto format=wide?std::optional<std::u16string>{read_utf16(memory,GuestAddress{arg(2)})}:crt_time::decode(read_ansi(memory,arg(2)),category.codepage);
                if(!format)memory.store(GuestAddress{runtime_.errno_cell},32,42);
                else{
                    const auto zone_pointer=memory.load(GuestAddress{names+(value.tm_isdst>0?(x86_?4:8):0)},x86_?32:64);
                    const auto zone=zone_pointer?crt_time::decode(read_ansi(memory,zone_pointer,256),category.codepage):std::optional<std::u16string>{std::u16string{}};
                    const auto west=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{timezone_cell},32)));
                    const auto offset=-static_cast<std::int64_t>(west)+(value.tm_isdst>0 && memory.load(GuestAddress{daylight_cell},32)?3600:0);
                    crt_time::Formatter formatter(data,value,zone.value_or(std::u16string{}),offset,static_cast<std::size_t>(arg(1)));auto converted=formatter.run(*format);
                    if(converted.error==crt_time::Error::Invalid){error(0);invalid_parameter=true;}
                    else if(converted.error==crt_time::Error::None){
                        if(wide){result=converted.text.size();memory.write(GuestAddress{arg(0)},utf16_bytes(converted.text));}
                        else{auto output=windows_crt_encode(converted.text,category.codepage);if(!output)memory.store(GuestAddress{runtime_.errno_cell},32,42);
                            else if(output->size()<arg(1)){result=output->size();output->push_back(0);memory.write(GuestAddress{arg(0)},*output);}}
                    }
                }
            }
        }
    } else {
        const bool secure=symbol.ends_with("_s");
        auto destination=secure?arg(0):time_buffer();
        if(!destination){error(secure?22:0);invalid_parameter=secure;}
        else {
            memory.check(GuestAddress{destination},36,Access::Write);
            const auto source=arg(secure ? 1 : 0);
            const auto seconds=read_time_value(memory,source,narrow_time);std::tm value{};
            if (!crt_calendar(seconds,maximum,value)) {
                for (const auto member : fields) value.*member=-1;
                write_tm(memory,destination,value);error(secure?22:0);invalid_parameter=!source || (!secure && seconds<0);
            } else {write_tm(memory,destination,value);result=secure ? 0 : destination;}
        }
    }
    if(x86_ && wide_result)state.reg(Gpr::Rdx)=static_cast<std::uint32_t>(result>>32);
    if(invalid_parameter && current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,result);return std::nullopt;}
    arg.return_to(state,result,wide_result);return std::nullopt;
}
}
