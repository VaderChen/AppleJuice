#include "x64arm/windows_services.hpp"
#include "icu_enumeration_guest.hpp"
#include "icu_guest_abi.hpp"
#include "icu_locale_guest.hpp"
#include "icu_set_state.hpp"
#include "service_callback_task.hpp"
#include <unicode/locid.h>
#include <unicode/uenum.h>
#include <unicode/uloc.h>
#include <unicode/ulocdata.h>

namespace x64arm {
namespace {
using namespace icu_abi;
using OwnedLocaleData=std::unique_ptr<ULocaleData,decltype(&ulocdata_close)>;
using OwnedEnumeration=std::unique_ptr<UEnumeration,decltype(&uenum_close)>;

class AnsiArgument {
public:
    AnsiArgument(const GuestMemory& memory,std::uint64_t address){
        if(address)value_=read_ansi(memory,address,INT32_MAX);
    }
    const char* get(const char* fallback=nullptr)const{return value_?value_->c_str():fallback;}
private:
    std::optional<std::string> value_;
};

template<class Function>
struct LocaleFunction {
    std::string_view name;
    Function apply;
};

constexpr LocaleFunction<decltype(&uloc_getName)> names[]{
#define LOCALE_NAME(name) {#name,name},
    LOCALE_NAME(uloc_addLikelySubtags)
    LOCALE_NAME(uloc_canonicalize)
    LOCALE_NAME(uloc_getBaseName)
    LOCALE_NAME(uloc_getCountry)
    LOCALE_NAME(uloc_getLanguage)
    LOCALE_NAME(uloc_getName)
    LOCALE_NAME(uloc_getParent)
    LOCALE_NAME(uloc_getScript)
    LOCALE_NAME(uloc_getVariant)
    LOCALE_NAME(uloc_minimizeSubtags)
#undef LOCALE_NAME
};

constexpr LocaleFunction<decltype(&uloc_getDisplayName)> display_names[]{
#define LOCALE_DISPLAY(name) {#name,name},
    LOCALE_DISPLAY(uloc_getDisplayCountry)
    LOCALE_DISPLAY(uloc_getDisplayKeyword)
    LOCALE_DISPLAY(uloc_getDisplayLanguage)
    LOCALE_DISPLAY(uloc_getDisplayName)
    LOCALE_DISPLAY(uloc_getDisplayScript)
    LOCALE_DISPLAY(uloc_getDisplayVariant)
#undef LOCALE_DISPLAY
};

}

struct IcuLocaleState {
    std::optional<std::string> default_locale;
    std::map<std::string,std::uint64_t,std::less<>> default_addresses;
    std::map<std::uint64_t,OwnedLocaleData> data;
};

std::span<const WindowsServiceDescriptor> icu_locale_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_LOCALE_API(name,count,legacy) {"icu.dll",#name,count,0,0,0,true},
#define ICU_LOCALE_RESIDENT ICU_LOCALE_API
#include "icu_locale_services.inc"
#undef ICU_LOCALE_RESIDENT
#undef ICU_LOCALE_API
    };
    return entries;
}

const char* WindowsServices::icu_default_locale() const{
    return icu_locale_ && icu_locale_->default_locale?icu_locale_->default_locale->c_str():nullptr;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_locale(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name=="uloc_countAvailable" || name=="uloc_getAvailable" ||
       name=="uloc_getISOCountries" || name=="uloc_getISOLanguages"){
        const auto base=icu_locale::image_address(x86_);
        state.rip=GuestAddress{base+runtime_.builtin_images.at(base).export_names.at(std::string(name))};
        return {};
    }
    if(name=="uloc_acceptLanguage" || name=="uloc_acceptLanguageFromHTTP" ||
       name=="uloc_openKeywords" || name=="uloc_openAvailableByType"){
        return nested_continuation(state,memory,"ICU 地區列舉",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                const auto finish=[&](std::uint64_t result){call.return_to(s,result);return std::optional<std::uint32_t>{};};
                IcuStatus status(m,call(service.arguments-1));
                if(U_FAILURE(status.value))co_return finish(0);
                const auto symbol=service.symbol;
                const auto base=icu_enumeration::image_address(x86_);
                const auto& enumeration=runtime_.builtin_images.at(base).export_names;
                if(symbol.starts_with("uloc_open")){
                    const bool keywords=symbol=="uloc_openKeywords";
                    const auto type=keywords?0:signed_argument(call(0));
                    if(type<ULOC_AVAILABLE_DEFAULT || type>ULOC_AVAILABLE_WITH_LEGACY_ALIASES){
                        status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();co_return finish(0);
                    }
                    const AnsiArgument locale(m,keywords?call(0):0);
                    OwnedEnumeration selected(keywords?
                        uloc_openKeywords(locale.get(icu_default_locale()),&status.value):
                        uloc_openAvailableByType(static_cast<ULocAvailableType>(type),&status.value),uenum_close);
                    if(!selected){status.commit();co_return finish(0);}
                    co_return finish(co_await export_icu_enumeration(s,m,*selected,status));
                }

                const bool http=symbol=="uloc_acceptLanguageFromHTTP";
                const auto capacity=signed_argument(call(1));
                const auto source=call(http?4U:5U);
                const auto desired_count=http?0:signed_argument(call(4));
                if(capacity<0 || (!call(0) && capacity) || !source ||
                   (http?!call(3):desired_count<0 || (desired_count && !call(3)))){
                    status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();co_return finish(0);
                }
                const AnsiArgument header(m,http?call(3):0);
                if(http){
                    UErrorCode validation=U_ZERO_ERROR;
                    OwnedEnumeration empty(uenum_openCharStringsEnumeration(nullptr,0,&validation),uenum_close);
                    if(empty)(void)uloc_acceptLanguageFromHTTP(nullptr,0,nullptr,header.get(),empty.get(),&validation);
                    if(U_FAILURE(validation)){status.value=validation;status.commit();co_return finish(0);}
                }
                std::vector<std::string> desired;
                std::vector<const char*> requested;
                const auto width=x86_?4U:8U;
                for(std::int32_t index=0;index<desired_count;++index){
                    const auto address=m.load(GuestAddress{call(3)+static_cast<std::uint64_t>(index)*width},
                        static_cast<std::uint8_t>(width*8));
                    if(!address){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();co_return finish(0);}
                    desired.push_back(read_ansi(m,address,INT32_MAX));
                }
                for(const auto& value:desired)requested.push_back(value.c_str());

                // 保留客體列舉器的目前位置與回呼 ABI，依 ICU 契約消耗剩餘項目。
                std::vector<std::string> available;
                const auto next=base+enumeration.at("uenum_next");
                status.commit();
                for(;;){
                    const auto value=co_await service_callback::call_value(next,{source,0,status.address.value});
                    status.value=static_cast<UErrorCode>(signed_argument(m.load(status.address,32)));
                    if(U_FAILURE(status.value))co_return finish(0);
                    if(!value)break;
                    available.push_back(read_ansi(m,value,INT32_MAX));
                    if(icu::Locale(available.back().c_str()).isBogus()){
                        status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();co_return finish(0);
                    }
                }
                std::vector<const char*> available_names;
                for(const auto& value:available)available_names.push_back(value.c_str());
                OwnedEnumeration local(uenum_openCharStringsEnumeration(available_names.data(),
                    static_cast<std::int32_t>(available_names.size()),&status.value),uenum_close);
                if(!local){status.commit();co_return finish(0);}
                UAcceptResult match=ULOC_ACCEPT_FAILED;
                const auto result=icu_preserving_output<char>(m,call(0),capacity,status,[&](auto* out,auto size,auto* error){
                    uenum_reset(local.get(),error);
                    return http?uloc_acceptLanguageFromHTTP(out,size,&match,header.get(),local.get(),error):
                        uloc_acceptLanguage(out,size,&match,requested.data(),desired_count,local.get(),error);
                });
                if(call(2) && (U_SUCCESS(status.value) || status.value==U_BUFFER_OVERFLOW_ERROR))
                    m.store(GuestAddress{call(2)},32,static_cast<std::uint32_t>(match));
                co_return finish(result);
            });
    }

    const WindowsCall call(state,memory,service);
    const auto i=[&](unsigned n){return signed_argument(call(n));};
    const auto finish=[&](std::uint64_t result){call.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto literal=[&](const char* value){return finish(publish_icu_literal(memory,value));};
    const auto get_locale=[&](unsigned n){return AnsiArgument(memory,call(n));};
    const auto* default_locale=icu_default_locale();
    if(name=="uloc_getDefault" || name=="uloc_setDefault"){
        if(!icu_locale_)icu_locale_=std::make_shared<IcuLocaleState>();
        if(name=="uloc_getDefault"){
            const auto* value=default_locale?default_locale:uloc_getDefault();
            if(const auto found=icu_locale_->default_addresses.find(value);found!=icu_locale_->default_addresses.end())
                return finish(found->second);
            const auto size=std::char_traits<char>::length(value)+1;
            const auto address=allocate_heap(memory,size);
            if(address){
                memory.write(GuestAddress{address},icu_bytes(value,size));
                icu_locale_->default_addresses.emplace(value,address);
            }
            return finish(address);
        }
        IcuStatus status(memory,call(1));
        if(U_FAILURE(status.value))return finish(0);
        const auto locale=get_locale(0);
        if(!locale.get())icu_locale_->default_locale.reset();
        else{
            UErrorCode error=U_ZERO_ERROR;
            std::vector<char> buffer(ULOC_FULLNAME_CAPACITY);
            auto length=uloc_getName(locale.get(),buffer.data(),static_cast<std::int32_t>(buffer.size()),&error);
            if(error==U_BUFFER_OVERFLOW_ERROR){
                error=U_ZERO_ERROR;
                buffer.resize(static_cast<std::size_t>(length)+1);
                length=uloc_getName(locale.get(),buffer.data(),static_cast<std::int32_t>(buffer.size()),&error);
            }
            if(U_SUCCESS(error))icu_locale_->default_locale.emplace(buffer.data(),static_cast<std::size_t>(length));
        }
        return finish(0);
    }
    if(name=="uloc_getISO3Language" || name=="uloc_getISO3Country"){
        const auto locale=get_locale(0);
        return literal(name=="uloc_getISO3Language"?uloc_getISO3Language(locale.get(default_locale)):
                                                  uloc_getISO3Country(locale.get(default_locale)));
    }
    if(name=="uloc_getLCID" || name=="uloc_isRightToLeft"){
        const auto locale=get_locale(0);
        if(name=="uloc_getLCID")return finish(uloc_getLCID(locale.get()));
        return finish(uloc_isRightToLeft(locale.get(default_locale))!=0);
    }
    if(name=="uloc_toLegacyKey" || name=="uloc_toLegacyType" || name=="uloc_toUnicodeLocaleKey" || name=="uloc_toUnicodeLocaleType"){
        const auto key=get_locale(0);
        const bool type=name.ends_with("Type");
        const auto value=AnsiArgument(memory,type?call(1):0);
        if(!key.get() || (type && !value.get()))return finish(0);
        const char* result=nullptr;
        if(name=="uloc_toLegacyKey")result=uloc_toLegacyKey(key.get());
        else if(name=="uloc_toUnicodeLocaleKey")result=uloc_toUnicodeLocaleKey(key.get());
        else if(name=="uloc_toLegacyType")result=uloc_toLegacyType(key.get(),value.get());
        else result=uloc_toUnicodeLocaleType(key.get(),value.get());
        if(result==key.get())return finish(call(0));
        if(value.get() && result==value.get())return finish(call(1));
        return literal(result);
    }
    if(name=="ulocdata_close" || name=="ulocdata_setNoSubstitute" || name=="ulocdata_getNoSubstitute"){
        if(!icu_locale_)return finish(0);
        const auto found=icu_locale_->data.find(call(0));
        if(found==icu_locale_->data.end())return finish(0);
        if(name=="ulocdata_getNoSubstitute")return finish(ulocdata_getNoSubstitute(found->second.get())!=0);
        if(name=="ulocdata_setNoSubstitute")ulocdata_setNoSubstitute(found->second.get(),static_cast<UBool>(call(1)));
        else{
            icu_locale_->data.erase(found);
            (void)free_heap(memory,call(0),state);
        }
        return finish(0);
    }

    IcuStatus status(memory,call(service.arguments-1));
    if(U_FAILURE(status.value))return finish(0);
    if(name=="ulocdata_open"){
        const auto locale=get_locale(0);
        OwnedLocaleData opened(ulocdata_open(locale.get(default_locale),&status.value),ulocdata_close);
        std::uint64_t handle{};
        if(opened){
            if(!icu_locale_)icu_locale_=std::make_shared<IcuLocaleState>();
            handle=allocate_heap(memory,1);
            if(handle)icu_locale_->data.emplace(handle,std::move(opened));
            else status.value=U_MEMORY_ALLOCATION_ERROR;
        }
        status.commit();return finish(handle);
    }
    if(name=="ulocdata_getCLDRVersion"){
        UVersionInfo version{};
        if(call(0)){
            ulocdata_getCLDRVersion(version,&status.value);
            if(U_SUCCESS(status.value))memory.write(GuestAddress{call(0)},icu_bytes(version,4));
        }else status.value=U_ILLEGAL_ARGUMENT_ERROR;
        status.commit();return finish(0);
    }
    if(name=="ulocdata_getMeasurementSystem" || name=="ulocdata_getPaperSize"){
        const auto locale=get_locale(0);
        if(name=="ulocdata_getMeasurementSystem"){
            const auto result=ulocdata_getMeasurementSystem(locale.get(default_locale),&status.value);
            status.commit();return finish(static_cast<std::uint32_t>(result));
        }
        std::int32_t height{},width{};
        if(call(1) && call(2)){
            ulocdata_getPaperSize(locale.get(default_locale),&height,&width,&status.value);
            if(U_SUCCESS(status.value)){
                memory.store(GuestAddress{call(1)},32,static_cast<std::uint32_t>(height));
                memory.store(GuestAddress{call(2)},32,static_cast<std::uint32_t>(width));
            }
        }else status.value=U_ILLEGAL_ARGUMENT_ERROR;
        status.commit();return finish(0);
    }
    if(name.starts_with("ulocdata_")){
        ULocaleData* object=nullptr;
        if(icu_locale_){
            const auto found=icu_locale_->data.find(call(0));
            if(found!=icu_locale_->data.end())object=found->second.get();
        }
        if(!object){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);}
        if(name=="ulocdata_getExemplarSet"){
            const auto handle=call(1);
            const auto* source=handle && icu_sets_?icu_sets_->get(handle):nullptr;
            OwnedIcuSet scratch(source?uset_clone(source):nullptr);
            const auto type=i(3);
            if(type<0 || type>=ULOCDATA_ES_COUNT || (handle && !source))status.value=U_ILLEGAL_ARGUMENT_ERROR;
            else if(source && !scratch)status.value=U_MEMORY_ALLOCATION_ERROR;
            std::uint64_t result{};
            if(U_SUCCESS(status.value)){
                auto* set=ulocdata_getExemplarSet(object,scratch.get(),static_cast<std::uint32_t>(call(2)),
                    static_cast<ULocaleDataExemplarSetType>(type),&status.value);
                if(set && U_SUCCESS(status.value)){
                    if(set==scratch.get())scratch.release();
                    result=install_icu_set(state,memory,handle,set);
                    if(!result)status.value=U_MEMORY_ALLOCATION_ERROR;
                }
            }
            status.commit();return finish(result);
        }
        if(name=="ulocdata_getDelimiter"){
            if(i(1)<0 || i(1)>=ULOCDATA_DELIMITER_COUNT){
                status.value=U_ILLEGAL_ARGUMENT_ERROR; status.commit();return finish(0);
            }
            return finish(icu_preserving_output<UChar>(memory,call(2),i(3),status,[&](auto* out,auto size,auto* error){
                return ulocdata_getDelimiter(object,static_cast<ULocaleDataDelimiterType>(i(1)),out,size,error);
            }));
        }
        const auto function=name=="ulocdata_getLocaleDisplayPattern"?ulocdata_getLocaleDisplayPattern:ulocdata_getLocaleSeparator;
        return finish(icu_preserving_output<UChar>(memory,call(1),i(2),status,
            [&](auto* out,auto size,auto* error){return function(object,out,size,error);}));
    }
    if(name=="uloc_getCharacterOrientation" || name=="uloc_getLineOrientation"){
        const auto locale=get_locale(0);
        const auto function=name=="uloc_getCharacterOrientation"?uloc_getCharacterOrientation:uloc_getLineOrientation;
        const auto result=function(locale.get(default_locale),&status.value);
        status.commit();return finish(static_cast<std::uint32_t>(result));
    }
    if(name=="uloc_forLanguageTag" || name=="uloc_toLanguageTag"){
        const bool incoming=name=="uloc_forLanguageTag";
        const auto input=get_locale(0);
        if(incoming && !input.get()){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);}
        std::int32_t parsed{};
        const auto result=icu_preserving_output<char>(memory,call(1),i(2),status,[&](auto* out,auto size,auto* error){
            return incoming?uloc_forLanguageTag(input.get(),out,size,&parsed,error):
                uloc_toLanguageTag(input.get(default_locale),out,size,static_cast<UBool>(call(3)),error);
        });
        if(incoming && call(3) && (U_SUCCESS(status.value) || status.value==U_BUFFER_OVERFLOW_ERROR))
            memory.store(GuestAddress{call(3)},32,static_cast<std::uint32_t>(parsed));
        return finish(result);
    }
    if(name=="uloc_getLocaleForLCID"){
        return finish(icu_preserving_output<char>(memory,call(1),i(2),status,[&](auto* out,auto size,auto* error){
            return uloc_getLocaleForLCID(static_cast<std::uint32_t>(call(0)),out,size,error);
        }));
    }
    if(name=="uloc_getKeywordValue"){
        const auto locale=get_locale(0),key=get_locale(1);
        if(!key.get()){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);}
        return finish(icu_preserving_output<char>(memory,call(2),i(3),status,[&](auto* out,auto size,auto* error){
            return uloc_getKeywordValue(locale.get(default_locale),key.get(),out,size,error);
        }));
    }
    if(name=="uloc_setKeywordValue"){
        const auto key=get_locale(0),value=get_locale(1);
        const auto capacity=i(3);
        if(!key.get() || !call(2) || capacity<=0){
            status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);
        }
        std::string text;
        for(std::int32_t n=0;n<capacity;++n){
            const auto unit=static_cast<char>(memory.load(GuestAddress{call(2)+static_cast<std::uint64_t>(n)},8));
            if(!unit)break;
            text.push_back(unit);
        }
        const auto limit=static_cast<std::size_t>(capacity);
        if(text.size()==limit){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);}
        std::vector<char> result(std::min(limit,std::max<std::size_t>(256,text.size()+1)));
        const auto initial=status.value;
        const auto apply=[&]{
            std::copy_n(text.c_str(),text.size()+1,result.data());
            return uloc_setKeywordValue(key.get(),value.get(),result.data(),static_cast<std::int32_t>(result.size()),&status.value);
        };
        auto length=apply();
        if(length>=0 && status.value==U_BUFFER_OVERFLOW_ERROR){
            const auto required=std::min(limit,static_cast<std::size_t>(length)+1);
            if(required>result.size()){
                result.resize(required);
                status.value=initial;
                length=apply();
            }
        }
        if(length>=0 && U_SUCCESS(status.value)){
            const auto copied=std::min(result.size(),static_cast<std::size_t>(length)+1);
            memory.write(GuestAddress{call(2)},icu_bytes(result.data(),copied));
        }
        status.commit();return finish(static_cast<std::uint32_t>(length));
    }
    if(name=="uloc_getDisplayKeywordValue"){
        const auto locale=get_locale(0),key=get_locale(1),display=get_locale(2);
        if(!key.get()){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);}
        return finish(icu_preserving_output<UChar>(memory,call(3),i(4),status,[&](auto* out,auto size,auto* error){
            return uloc_getDisplayKeywordValue(locale.get(default_locale),key.get(),display.get(default_locale),out,size,error);
        }));
    }
    const auto matches=[&](const auto& item){return item.name==name;};
    if(const auto found=std::ranges::find_if(display_names,matches);found!=std::end(display_names)){
        const auto locale=get_locale(0),display=get_locale(1);
        const bool keyword=name=="uloc_getDisplayKeyword";
        if(keyword && !locale.get()){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);}
        return finish(icu_preserving_output<UChar>(memory,call(2),i(3),status,[&](auto* out,auto size,auto* error){
            return found->apply(locale.get(keyword?nullptr:default_locale),display.get(default_locale),out,size,error);
        }));
    }
    const auto found=std::ranges::find_if(names,matches);
    if(found==std::end(names))throw HostCallFault("ICU locale 未登錄的服務");
    const auto locale=get_locale(0);
    return finish(icu_preserving_output<char>(memory,call(1),i(2),status,[&](auto* out,auto size,auto* error){
        return found->apply(locale.get(default_locale),out,size,error);
    }));
}
}
