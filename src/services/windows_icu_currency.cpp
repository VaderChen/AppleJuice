#include "x64arm/windows_services.hpp"
#include "icu_guest_abi.hpp"
#include "service_callback_task.hpp"
#include <unicode/ucurr.h>
#include <unicode/uenum.h>
#include <charstr.h>
#include <uinvchar.h>
#include <ulocimp.h>
#include <ustr_imp.h>
#include <deque>

namespace x64arm {
namespace {
using namespace icu_abi;
struct RegistryEntry {
    std::uint64_t handle{};
    std::string region;
    std::array<UChar,4> currency{};
};

// ICU 的三碼資料查詢讀取管制長度，名稱函式還會使用完整的 NUL 結尾字串。
std::u16string read_currency(const GuestMemory& memory,std::uint64_t address){
    if(!address)return {};
    auto result=read_utf16(memory,GuestAddress{address},INT32_MAX);
    if(result.size()<3)result.resize(3);
    return result;
}

bool valid_usage(std::int32_t usage){
    return usage==UCURR_USAGE_STANDARD || usage==UCURR_USAGE_CASH;
}

}

struct IcuCurrencyState {
    std::deque<RegistryEntry> registrations;
    std::map<std::u16string,std::uint64_t,std::less<>> names;
};

std::span<const WindowsServiceDescriptor> icu_currency_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_CURR_API(name,count,wide) {"icu.dll",#name,count,wide,0,0,true},
#define ICU_CURR_QUERY ICU_CURR_API
#include "icu_currency_services.inc"
#undef ICU_CURR_QUERY
#undef ICU_CURR_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_currency(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name=="ucurr_openISOCurrencies" || name=="ucurr_getKeywordValuesForLocale"){
        return nested_continuation(state,memory,"ICU 貨幣列舉",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);
                const auto finish=[&](std::uint64_t result){call.return_to(s,result);return std::optional<std::uint32_t>{};};
                IcuStatus status(m,call(service.arguments-1));
                if(U_FAILURE(status.value))co_return finish(0);
                std::unique_ptr<UEnumeration,decltype(&uenum_close)> enumeration(nullptr,uenum_close);
                if(service.symbol=="ucurr_openISOCurrencies")
                    enumeration.reset(ucurr_openISOCurrencies(static_cast<std::uint32_t>(call(0)),&status.value));
                else if(call(0)){
                    const auto key=read_ansi(m,call(0),INT32_MAX);
                    const auto locale=call(1)?read_ansi(m,call(1),INT32_MAX):std::string{};
                    enumeration.reset(ucurr_getKeywordValuesForLocale(key.c_str(),call(1)?locale.c_str():icu_default_locale(),
                        static_cast<UBool>(call(2)),&status.value));
                }else status.value=U_ILLEGAL_ARGUMENT_ERROR;
                if(!enumeration){status.commit();co_return finish(0);}
                co_return finish(co_await export_icu_enumeration(s,m,*enumeration,status));
            });
    }

    const WindowsCall call(state,memory,service);
    const auto i=[&](unsigned n){return signed_argument(call(n));};
    const auto finish=[&](std::uint64_t result){call.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto date=[&](unsigned n){return icu_double_argument(state,call,x86_,n);};
    if(name=="ucurr_getNumericCode"){
        const auto currency=read_currency(memory,call(0));
        return finish(static_cast<std::uint32_t>(ucurr_getNumericCode(call(0)?currency.c_str():nullptr)));
    }
    const bool rounding=name=="ucurr_getRoundingIncrement" || name=="ucurr_getRoundingIncrementForUsage";
    IcuStatus status(memory,call(service.arguments-1));
    const auto floating=[&](double result){windows_floating_return(state,x86_,result,false);return finish(0);};
    if(U_FAILURE(status.value))return rounding?floating(0.0):finish(0);

    if(name=="ucurr_getDefaultFractionDigits" || name=="ucurr_getDefaultFractionDigitsForUsage" || rounding){
        const auto storage=read_currency(memory,call(0));
        const auto* currency=call(0)?storage.c_str():nullptr;
        const auto usage=name.ends_with("ForUsage")?i(1):UCURR_USAGE_STANDARD;
        if(!valid_usage(usage)){
            status.value=rounding && (!currency || !*currency)?U_ILLEGAL_ARGUMENT_ERROR:U_UNSUPPORTED_ERROR;
            status.commit();return rounding?floating(0.0):finish(0);
        }
        if(rounding){
            const auto value=ucurr_getRoundingIncrementForUsage(currency,static_cast<UCurrencyUsage>(usage),&status.value);
            status.commit();return floating(value);
        }
        const auto value=ucurr_getDefaultFractionDigitsForUsage(currency,static_cast<UCurrencyUsage>(usage),&status.value);
        status.commit();return finish(static_cast<std::uint32_t>(value));
    }
    if(name=="ucurr_isAvailable"){
        const auto currency=read_currency(memory,call(0));
        if(!call(0)){status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);}
        const bool result=ucurr_isAvailable(currency.c_str(),date(1),date(2),&status.value)!=0;
        status.commit();return finish(result);
    }
    if(name=="ucurr_getName" || name=="ucurr_getPluralName"){
        const bool plural=name=="ucurr_getPluralName";
        if(!call(0) || (plural?!call(3):i(2)<UCURR_SYMBOL_NAME || i(2)>UCURR_FORMAL_SYMBOL_NAME)){
            status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();return finish(0);
        }
        const auto currency=read_currency(memory,call(0));
        const auto locale=call(1)?read_ansi(memory,call(1),INT32_MAX):std::string{};
        const auto* locale_id=call(1)?locale.c_str():icu_default_locale();
        const auto count=plural?read_ansi(memory,call(3),INT32_MAX):std::string{};
        UBool choice{};
        std::int32_t length{};
        const auto* result=plural?ucurr_getPluralName(currency.c_str(),locale_id,&choice,count.c_str(),&length,&status.value):
            ucurr_getName(currency.c_str(),locale_id,static_cast<UCurrNameStyle>(i(2)),&choice,&length,&status.value);
        std::uint64_t address{};
        if(result && U_SUCCESS(status.value)){
            if(result==currency.c_str())address=call(0);
            else{
                if(!icu_currency_)icu_currency_=std::make_shared<IcuCurrencyState>();
                const std::u16string_view text(result,static_cast<std::size_t>(length));
                auto& names=icu_currency_->names;
                if(const auto found=names.find(text);found!=names.end())address=found->second;
                else{
                    address=allocate_heap(memory,(text.size()+1)*sizeof(UChar));
                    if(address){
                        memory.write(GuestAddress{address},icu_bytes(result,text.size()+1));
                        names.emplace(text,address);
                    }else status.value=U_MEMORY_ALLOCATION_ERROR;
                }
            }
            if(address){
                if(call(plural?2:3))memory.store(GuestAddress{call(plural?2:3)},8,choice!=0);
                if(call(4))memory.store(GuestAddress{call(4)},32,static_cast<std::uint32_t>(length));
            }
        }
        status.commit();return finish(address);
    }
    if(name=="ucurr_register"){
        const auto locale=call(1)?read_ansi(memory,call(1),INT32_MAX):std::string{};
        const auto region=ulocimp_getRegionForSupplementalData(call(1)?locale.c_str():icu_default_locale(),false,status.value);
        std::uint64_t handle{};
        if(U_SUCCESS(status.value) && call(0)){
            RegistryEntry item;
            item.region=region.data();
            const auto code=read_currency(memory,call(0));
            std::copy_n(code.begin(),3,item.currency.begin());
            if(!icu_currency_)icu_currency_=std::make_shared<IcuCurrencyState>();
            handle=allocate_heap(memory,1);
            if(handle){
                item.handle=handle;
                icu_currency_->registrations.push_front(std::move(item));
            }else status.value=U_MEMORY_ALLOCATION_ERROR;
        }
        status.commit();return finish(handle);
    }
    if(name=="ucurr_unregister"){
        if(!icu_currency_)return finish(0);
        auto& entries=icu_currency_->registrations;
        const auto found=std::ranges::find(entries,call(0),&RegistryEntry::handle);
        if(found==entries.end())return finish(0);
        entries.erase(found);
        (void)free_heap(memory,call(0),state);
        return finish(1);
    }

    const auto locale=call(0)?read_ansi(memory,call(0),INT32_MAX):std::string{};
    const auto* locale_id=call(0)?locale.c_str():icu_default_locale();
    if(name=="ucurr_countCurrencies"){
        const auto result=ucurr_countCurrencies(locale_id,date(1),&status.value);
        status.commit();return finish(static_cast<std::uint32_t>(result));
    }
    if(name=="ucurr_forLocaleAndDate"){
        const auto when=date(1);
        return finish(icu_preserving_output<UChar>(memory,call(3),i(4),status,
            [&](auto* out,auto size,auto* error){return ucurr_forLocaleAndDate(locale_id,when,i(2),out,size,error);}));
    }

    const RegistryEntry* registered=nullptr;
    if(icu_currency_ && !icu_currency_->registrations.empty()){
        UErrorCode keyword_error=U_ZERO_ERROR;
        const auto keyword=ulocimp_getKeywordValue(locale_id,"currency",keyword_error);
        if(U_FAILURE(keyword_error) || keyword.length()!=3 || !uprv_isInvariantString(keyword.data(),3)){
            const auto region=ulocimp_getRegionForSupplementalData(locale_id,false,status.value);
            if(U_FAILURE(status.value)){status.commit();return finish(0);}
            const auto& entries=icu_currency_->registrations;
            const auto found=std::ranges::find(entries,std::string_view(region.data()),&RegistryEntry::region);
            if(found!=entries.end())registered=&*found;
        }
    }
    return finish(icu_preserving_output<UChar>(memory,call(1),i(2),status,[&](auto* out,auto size,auto* error){
        if(!registered)return ucurr_forLocale(locale_id,out,size,error);
        const auto& code=registered->currency;
        const auto length=static_cast<std::int32_t>(std::char_traits<UChar>::length(code.data()));
        if(size>length)std::copy_n(code.data(),static_cast<std::size_t>(length)+1,out);
        return u_terminateUChars(out,size,length,error);
    }));
}
}
