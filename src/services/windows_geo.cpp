// 地理位置（GEOID）家族：GetGeoInfoA／W／Ex、GetUserGeoID／SetUserGeoID、GetUserDefaultGeoName／SetUserGeoName、
// EnumSystemGeoID／EnumSystemGeoNames。識別碼與英文名稱來自 windows_geo_data.hpp；ISO3、UN M.49、上層區域、
// 貨幣、Windows 時區識別碼與其他語言的名稱由 ICU 依 ISO／M.49 代碼推得。使用者位置保存在
// HKCU\Control Panel\International\Geo（Nation 為十進位 GEOID、Name 為代碼），未設定時取使用者預設地區設定的區域。
// 沒有資料來源的經緯度、國際電話碼與官方語言回 ERROR_NOT_SUPPORTED，不以固定值代替。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include "windows_geo_data.hpp"
#include <unicode/ucal.h>
#include <unicode/ucurr.h>
#include <unicode/uenum.h>
#include <unicode/uloc.h>
#include <unicode/uregion.h>
#include <algorithm>
#include <charconv>

namespace x64arm {
namespace {
constexpr std::uint32_t geo_not_available=0xffffffffU;
std::string icu_locale(const geo::Entry& entry){return "und_"+std::string(entry.code);}
std::u16string widen(std::string_view text){return std::u16string(text.begin(),text.end());}
std::u16string decimal(std::uint64_t value){return widen(std::to_string(value));}
std::optional<std::u16string> iso3(const geo::Entry& entry){
    if(geo::region(entry))return widen(entry.code);if(entry.code=="XX")return std::nullopt;
    const auto* value=uloc_getISO3Country(icu_locale(entry).c_str());if(!value || !*value)return std::nullopt;return widen(value);
}
std::optional<int> un_number(const geo::Entry& entry){
    if(geo::region(entry)){int value=0;std::from_chars(entry.code.data(),entry.code.data()+entry.code.size(),value);return value;}
    if(entry.code=="XX")return std::nullopt;
    UErrorCode error=U_ZERO_ERROR;const auto* region=uregion_getRegionFromCode(std::string(entry.code).c_str(),&error);if(U_FAILURE(error) || !region)return std::nullopt;
    const auto value=uregion_getNumericCode(region);return value>0?std::optional{value}:std::nullopt;
}
const geo::Entry* parent(const geo::Entry& entry){
    if(entry.code=="XX")return nullptr;
    UErrorCode error=U_ZERO_ERROR;const auto* region=uregion_getRegionFromCode(std::string(entry.code).c_str(),&error);if(U_FAILURE(error) || !region)return nullptr;
    const auto* container=uregion_getContainingRegion(region);if(!container)return nullptr;
    return geo::find_code(widen(uregion_getRegionCode(container)));
}
std::string likely_locale(const geo::Entry& entry){
    UErrorCode error=U_ZERO_ERROR;char out[ULOC_FULLNAME_CAPACITY]{};uloc_addLikelySubtags(icu_locale(entry).c_str(),out,sizeof out,&error);return U_SUCCESS(error)?std::string(out):std::string{};
}
std::optional<std::u16string> language_tag(const geo::Entry& entry){
    if(geo::region(entry) || entry.code=="XX")return std::nullopt;
    const auto likely=likely_locale(entry);UErrorCode error=U_ZERO_ERROR;char language[ULOC_LANG_CAPACITY]{};
    uloc_getLanguage(likely.c_str(),language,sizeof language,&error);if(U_FAILURE(error) || !*language || std::string_view(language)=="und")return std::nullopt;
    return widen(language)+u"-"+widen(entry.code);
}
std::optional<std::u16string> currency(const geo::Entry& entry,bool symbol){
    if(geo::region(entry) || entry.code=="XX")return std::nullopt;
    UErrorCode error=U_ZERO_ERROR;UChar code[4]{};const auto length=ucurr_forLocale(icu_locale(entry).c_str(),code,4,&error);
    if(U_FAILURE(error) || length!=3)return std::nullopt;
    if(!symbol)return std::u16string(reinterpret_cast<const char16_t*>(code),3);
    UBool choice=false;int32_t size=0;error=U_ZERO_ERROR;const auto* name=ucurr_getName(code,likely_locale(entry).c_str(),UCURR_SYMBOL_NAME,&choice,&size,&error);
    if(U_FAILURE(error) || !name || size<=0)return std::nullopt;return std::u16string(reinterpret_cast<const char16_t*>(name),static_cast<std::size_t>(size));
}
std::optional<std::u16string> time_zones(const geo::Entry& entry){
    // ICU 的 IANA 時區對應 Windows 時區識別碼，去重後以分號串接。
    if(geo::region(entry) || entry.code=="XX")return std::nullopt;
    UErrorCode error=U_ZERO_ERROR;auto* zones=ucal_openCountryTimeZones(std::string(entry.code).c_str(),&error);if(U_FAILURE(error) || !zones)return std::nullopt;
    std::vector<std::u16string> names;
    for(;;){int32_t length=0;const auto* id=uenum_unext(zones,&length,&error);if(U_FAILURE(error) || !id)break;
        UChar windows[128]{};UErrorCode convert=U_ZERO_ERROR;const auto size=ucal_getWindowsTimeZoneID(id,length,windows,128,&convert);
        if(U_SUCCESS(convert) && size>0){std::u16string name(reinterpret_cast<const char16_t*>(windows),static_cast<std::size_t>(size));if(std::find(names.begin(),names.end(),name)==names.end())names.push_back(std::move(name));}}
    uenum_close(zones);if(names.empty())return std::nullopt;
    std::u16string joined;for(const auto& name:names){if(!joined.empty())joined.push_back(u';');joined+=name;}return joined;
}
std::optional<std::u16string> display_name(const geo::Entry& entry,const std::string& display_locale){
    UErrorCode error=U_ZERO_ERROR;UChar buffer[256]{};const auto length=uloc_getDisplayCountry(icu_locale(entry).c_str(),display_locale.c_str(),buffer,256,&error);
    if(U_FAILURE(error) || length<=0)return std::nullopt;std::u16string name(reinterpret_cast<const char16_t*>(buffer),static_cast<std::size_t>(length));
    if(name==widen(entry.code))return std::nullopt;return name;
}
}
std::span<const WindowsServiceDescriptor> geo_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","GetGeoInfoA",5,0,0,0x1b},{"kernel32.dll","GetGeoInfoW",5,0,0,0x1b},{"kernel32.dll","GetGeoInfoEx",4,0,0,0xa},
        {"kernel32.dll","GetUserGeoID",1,0,0,0x1},{"kernel32.dll","SetUserGeoID",1,0,0,0x1},{"kernel32.dll","GetUserDefaultGeoName",2,0,0,0x2},{"kernel32.dll","SetUserGeoName",1},
        {"kernel32.dll","EnumSystemGeoID",3,0,0,0x3},{"kernel32.dll","EnumSystemGeoNames",3,0,0,0x1}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_geo(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"地理位置",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
    const WindowsCall call(s,m,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error,std::uint64_t value=0){last_error_=error;return finish(value);};
    std::vector<std::uint64_t> blocks;
    struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(memory,block,state);}} release{*this,m,s,blocks};
    const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,2));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,2));blocks.push_back(block);return block;};
    const auto guest=[&](std::u16string_view value){const auto bytes=utf16_bytes(value);const auto block=scratch(bytes.size());m.write(GuestAddress{block},bytes);return block;};
    const auto api=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
        co_return co_await service_callback::call_value(resolve(dll,symbol).value,std::move(arguments));};
    const std::uint64_t current_user=x86_?0x80000001U:0xffffffff80000001ULL;
    const auto geo_key=u"Control Panel\\International\\Geo";
    // 使用者位置：登錄的 Nation，否則使用者預設地區設定的區域代碼，最後為美國。
    const auto user_geo=[&]()->service_callback::Task<const geo::Entry*>{
        const auto buffer=scratch(128),size=scratch(4);m.store(GuestAddress{size},32,128);
        if(!co_await api("advapi32.dll","RegGetValueW",{current_user,guest(geo_key),guest(u"Nation"),2,0,buffer,size})){
            std::uint32_t id=0;bool digits=false;for(const auto c:read_utf16(m,GuestAddress{buffer},64)){if(c<u'0' || c>u'9'){digits=false;break;}id=id*10+static_cast<std::uint32_t>(c-u'0');digits=true;}
            if(digits)if(const auto* entry=geo::find(id))co_return entry;
        }
        const auto locale=scratch(200);
        if(co_await api("kernel32.dll","GetUserDefaultLocaleName",{locale,85})){
            const auto text=read_utf16(m,GuestAddress{locale},85);const auto dash=text.find_last_of(u'-');
            if(dash!=text.npos)if(const auto* entry=geo::find_code(std::u16string_view(text).substr(dash+1)))co_return entry;
        }
        co_return geo::find(244);
    };
    const auto save=[&](const geo::Entry& entry)->service_callback::Task<bool>{
        const auto nation=decimal(entry.id),code=widen(entry.code);
        if(co_await api("advapi32.dll","RegSetKeyValueW",{current_user,guest(geo_key),guest(u"Nation"),1,guest(nation),(nation.size()+1)*2}))co_return false;
        co_return !co_await api("advapi32.dll","RegSetKeyValueW",{current_user,guest(geo_key),guest(u"Name"),1,guest(code),(code.size()+1)*2});
    };
    // 依 SYSGEOTYPE 取資料；extended 為 GetGeoInfoEx（以代碼指定、不支援 NATION／LCID／RFC1766）。回傳錯誤碼或字串。
    const auto info=[&](const geo::Entry& entry,std::uint32_t type,std::uint32_t language,bool extended)->service_callback::Task<std::pair<std::uint32_t,std::u16string>>{
        const auto none=[](std::uint32_t error){return std::pair<std::uint32_t,std::u16string>{error,{}};};
        const auto value=[](std::optional<std::u16string> text){return text?std::pair<std::uint32_t,std::u16string>{0,std::move(*text)}:std::pair<std::uint32_t,std::u16string>{87,{}};};
        switch(type){
        case 1:if(extended || geo::region(entry))co_return none(87);co_return value(decimal(entry.id));                 // GEO_NATION
        case 2:case 3:case 11:case 14:co_return none(50);                                                              // 經緯度、官方語言、電話碼
        case 4:co_return value(widen(entry.code));                                                                     // GEO_ISO2
        case 5:co_return value(iso3(entry));                                                                           // GEO_ISO3
        case 6:if(extended)co_return none(87);co_return value(language_tag(entry));                                    // GEO_RFC1766
        case 7:{if(extended)co_return none(87);const auto tag=language_tag(entry);if(!tag)co_return none(87);          // GEO_LCID
            const auto lcid=static_cast<std::uint32_t>(co_await api("kernel32.dll","LocaleNameToLCID",{guest(*tag),0}));if(!lcid)co_return none(87);
            char text[16]{};std::snprintf(text,sizeof text,"%08x",lcid);co_return value(widen(text));}
        case 8:case 9:{                                                                                               // GEO_FRIENDLYNAME／OFFICIALNAME
            std::string locale="en";
            if(language && (language&0x3ffU)!=0x09U){const auto buffer=scratch(200);if(co_await api("kernel32.dll","LCIDToLocaleName",{language,buffer,85,0})){
                auto text=utf16_to_utf8(read_utf16(m,GuestAddress{buffer},85));std::replace(text.begin(),text.end(),'-','_');locale=text;}}
            if(locale.starts_with("en"))co_return value(std::u16string(type==9 && !entry.long_name.empty()?entry.long_name:entry.short_name));
            const auto localized=display_name(entry,locale);co_return value(localized?localized:std::optional{std::u16string(entry.short_name)});}
        case 10:co_return value(time_zones(entry));                                                                    // GEO_TIMEZONES
        case 12:{const auto number=un_number(entry);if(!number)co_return none(87);char text[8]{};std::snprintf(text,sizeof text,"%03d",*number);co_return value(widen(text));} // GEO_ISO_UN_NUMBER
        case 13:{const auto* up=parent(entry);if(!up)co_return none(87);co_return value(extended?widen(up->code):decimal(up->id));} // GEO_PARENT
        case 15:co_return value(currency(entry,false));                                                                // GEO_CURRENCYCODE
        case 16:co_return value(currency(entry,true));                                                                 // GEO_CURRENCYSYMBOL
        case 17:{if(entry.code!="XX")co_return value(widen(entry.code));const auto number=un_number(entry);if(!number)co_return value(std::u16string(u"XX"));co_return value(decimal(static_cast<std::uint64_t>(*number)));} // GEO_NAME
        case 18:co_return value(decimal(entry.id));                                                                    // GEO_ID
        default:co_return none(1004);                                                                                  // ERROR_INVALID_FLAGS
        }
    };
    // 字串輸出：W 為 UTF-16 字元數、A 為位元組數（含 NUL）；容量 0 只回報長度，不足為 ERROR_INSUFFICIENT_BUFFER。
    const auto emit=[&](std::uint64_t buffer,std::int64_t capacity,const std::u16string& text,bool wide)->std::optional<std::uint32_t>{
        std::vector<std::uint8_t> bytes;if(wide)bytes=utf16_bytes(text);else {const auto s=utf16_to_utf8(text);bytes.assign(s.begin(),s.end());bytes.push_back(0);}
        const auto count=static_cast<std::uint32_t>(bytes.size()/(wide?2U:1U));
        if(capacity<0 || (capacity && !buffer))return fail(87);
        if(!capacity)return finish(count);
        if(static_cast<std::uint64_t>(capacity)<count)return fail(122);
        m.write(GuestAddress{buffer},bytes);return finish(count);
    };
    try {
        if(name=="GetGeoInfoA" || name=="GetGeoInfoW" || name=="GetGeoInfoEx"){
            const bool extended=name=="GetGeoInfoEx";const auto* entry=extended?(call(0)?geo::find_code(read_utf16(m,GuestAddress{call(0)},16)):nullptr):geo::find(static_cast<std::uint32_t>(call(0)));
            if(!entry)co_return fail(87);
            const auto [error,text]=co_await info(*entry,static_cast<std::uint32_t>(call(1)),extended?0U:static_cast<std::uint32_t>(call(4)),extended);
            if(error)co_return fail(error);
            co_return emit(call(2),static_cast<std::int32_t>(call(3)),text,name!="GetGeoInfoA");
        }
        if(name=="GetUserGeoID"){
            const auto geo_class=static_cast<std::uint32_t>(call(0));
            if(geo_class!=16 && geo_class!=14 && geo_class!=0)co_return fail(87,geo_not_available);
            const auto* entry=co_await user_geo();if(!entry)co_return finish(geo_not_available);
            if(geo_class==0)co_return finish(entry->id);
            if(geo_class==16)co_return finish(geo::region(*entry)?geo_not_available:entry->id);
            if(geo::region(*entry))co_return finish(entry->id);
            const auto* up=parent(*entry);co_return finish(up?up->id:geo_not_available);
        }
        if(name=="SetUserGeoID" || name=="SetUserGeoName"){
            const auto* entry=name=="SetUserGeoID"?geo::find(static_cast<std::uint32_t>(call(0))):(call(0)?geo::find_code(read_utf16(m,GuestAddress{call(0)},16)):nullptr);
            if(!entry)co_return fail(87);
            co_return (co_await save(*entry))?finish(1):fail(1009); // ERROR_BADDB
        }
        if(name=="GetUserDefaultGeoName"){
            const auto* entry=co_await user_geo();if(!entry)co_return fail(1009);
            const auto [error,text]=co_await info(*entry,17,0,true);if(error)co_return fail(error);
            co_return emit(call(0),static_cast<std::int32_t>(call(1)),text,true);
        }
        if(name=="EnumSystemGeoID" || name=="EnumSystemGeoNames"){
            // GEOCLASS_NATION 16、GEOCLASS_REGION 14、GEOCLASS_ALL 0；回呼傳回 FALSE 即停止。EnumSystemGeoNames 略過沒有代碼的位置。
            const bool names=name=="EnumSystemGeoNames";const auto geo_class=static_cast<std::uint32_t>(call(0));const auto procedure=call(names?1:2);
            if(geo_class!=16 && geo_class!=14 && geo_class!=0)co_return fail(1004);
            if(!procedure || (!names && static_cast<std::uint32_t>(call(1))!=geo_not_available && static_cast<std::uint32_t>(call(1))!=0))co_return fail(87);
            for(const auto& entry:geo::entries){
                if((geo_class==16 && geo::region(entry)) || (geo_class==14 && !geo::region(entry)) || (names && entry.code=="XX"))continue;
                const auto keep=names?co_await service_callback::call_value(procedure,{guest(widen(entry.code)),call(2)}):co_await service_callback::call_value(procedure,{entry.id});
                if(!static_cast<std::uint32_t>(keep))break;
            }
            co_return finish(1);
        }
    }catch(const MemoryFault&){co_return fail(998);}
    catch(const std::bad_alloc&){co_return fail(8);}
    co_return fail(87);
    });
}
}
