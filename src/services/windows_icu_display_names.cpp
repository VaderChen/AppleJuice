#include "x64arm/windows_services.hpp"
#include "icu_display_names_layout.hpp"
#include "icu_guest_abi.hpp"
#include <unicode/uscript.h>

namespace x64arm {
namespace {
using OwnedNames=std::unique_ptr<ULocaleDisplayNames,decltype(&uldn_close)>;
struct DisplayLookup {
    std::string_view symbol;
    int32_t (U_EXPORT2 *lookup)(const ULocaleDisplayNames*,const char*,UChar*,int32_t,UErrorCode*);
};
constexpr DisplayLookup lookups[]{
    {"uldn_keyDisplayName",uldn_keyDisplayName},
    {"uldn_languageDisplayName",uldn_languageDisplayName},
    {"uldn_localeDisplayName",uldn_localeDisplayName},
    {"uldn_regionDisplayName",uldn_regionDisplayName},
    {"uldn_scriptDisplayName",uldn_scriptDisplayName},
    {"uldn_variantDisplayName",uldn_variantDisplayName},
};
}

struct IcuDisplayNamesState {
    std::map<std::uint64_t,OwnedNames> objects;
};

std::span<const WindowsServiceDescriptor> icu_display_names_service_descriptors(){
    static constexpr WindowsServiceDescriptor entries[]{
#define ICU_LDN_API(name,count) {"icu.dll",#name,count,0,0,0,true},
#define ICU_LDN_QUERY(name,count) {"icu.dll",#name,count,0,0,0,true},
#include "icu_display_names_services.inc"
#undef ICU_LDN_QUERY
#undef ICU_LDN_API
    };
    return entries;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_display_names(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    using namespace icu_abi;
    using namespace icu_display_names;
    const WindowsCall call(state,memory,service);
    const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value=0){
        call.return_to(state,value);
        return std::optional<std::uint32_t>{};
    };

    if(name=="uldn_open" || name=="uldn_openForContext"){
        IcuStatus status(memory,call(service.arguments-1));
        const auto before=status.value;
        const auto done=[&](std::uint64_t handle=0){
            if(status.value!=before)status.commit();
            return finish(handle);
        };
        if(U_FAILURE(before))return finish();
        try{
            OwnedNames native(nullptr,uldn_close);
            const auto input=call(0);
            const auto locale=input?read_ansi(memory,input):std::string{};
            const auto* locale_id=input?locale.c_str():icu_default_locale();
            if(name=="uldn_open"){
                const auto dialect=signed_argument(call(1));
                if(dialect<ULDN_STANDARD_NAMES || dialect>ULDN_DIALECT_NAMES){
                    status.value=U_ILLEGAL_ARGUMENT_ERROR;
                    return done();
                }
                native.reset(uldn_open(locale_id,static_cast<UDialectHandling>(dialect),&status.value));
            }else{
                const auto input_contexts=call(1);
                const auto count=signed_argument(call(2));
                if(count<0 || (count>0 && !input_contexts)){
                    status.value=U_ILLEGAL_ARGUMENT_ERROR;
                    return done();
                }
                Contexts contexts;
                if(count)memory.check(GuestAddress{input_contexts},4*std::size_t(count),Access::Read);
                for(std::int32_t index=0;index<count;++index)
                    contexts.apply(signed_argument(memory.load(GuestAddress{input_contexts+4*std::uint64_t(index)},32)));
                native.reset(uldn_openForContext(locale_id,contexts.items,context_count,&status.value));
            }
            if(!native && U_SUCCESS(status.value))status.value=U_MEMORY_ALLOCATION_ERROR;
            if(!native || U_FAILURE(status.value))return done();

            View view;
            for(std::int32_t type=0;type<context_count;++type)
                view.contexts.items[type]=uldn_getContext(native.get(),static_cast<UDisplayContextType>(type),&status.value);
            if(U_FAILURE(status.value))return done();
            const std::string_view locale_name(uldn_getLocale(native.get()));
            if(!icu_display_names_)icu_display_names_=std::make_shared<IcuDisplayNamesState>();
            if(status.value!=before)memory.check(status.address,4,Access::Write);
            const auto bytes=locale_name.size()+1;
            const auto handle=allocate_heap(memory,sizeof(View)+bytes);
            if(!handle){status.value=U_MEMORY_ALLOCATION_ERROR;return done();}
            try{
                write_icu_record(memory,handle,view);
                memory.write(GuestAddress{handle+sizeof(View)},icu_bytes(locale_name.data(),bytes));
                icu_display_names_->objects.emplace(handle,std::move(native));
            }catch(...){
                (void)free_heap(memory,handle,state);
                throw;
            }
            return done(handle);
        }catch(const std::bad_alloc&){
            status.value=U_MEMORY_ALLOCATION_ERROR;
            return done();
        }
    }

    const auto handle=call(0);
    auto* owners=icu_display_names_.get();
    const auto object=owners?owners->objects.find(handle):decltype(owners->objects.begin()){};
    const auto owned=owners && object!=owners->objects.end();
    if(name=="uldn_close"){
        if(owned && free_heap(memory,handle,state))owners->objects.erase(object);
        return finish();
    }

    if(name=="uldn_getLocale" || name=="uldn_getDialectHandling" || name=="uldn_getContext"){
        View view;
        if(owned)read_icu_record(memory,handle,view);
        const bool valid=owned && view.magic==View::signature;
        if(name=="uldn_getLocale")return finish(valid?handle+sizeof(View):0);
        if(name=="uldn_getDialectHandling")return finish(valid?view.contexts.get(UDISPCTX_TYPE_DIALECT_HANDLING):0U);

        IcuStatus status(memory,call(2));
        if(U_FAILURE(status.value))return finish();
        if(!valid){
            status.value=U_ILLEGAL_ARGUMENT_ERROR;
            status.commit();
            return finish();
        }
        return finish(view.contexts.get(signed_argument(call(1))));
    }

    IcuStatus status(memory,call(service.arguments-1));
    if(U_FAILURE(status.value))return finish();
    const auto invalid=[&](){
        status.value=U_ILLEGAL_ARGUMENT_ERROR;
        status.commit();
        return finish();
    };
    const bool key_value=name=="uldn_keyValueDisplayName";
    const bool script_code=name=="uldn_scriptCodeDisplayName";
    const unsigned output=key_value?3U:2U;
    const auto capacity=signed_argument(call(output+1));
    const auto destination=call(output);
    if(!owned || capacity<0 || (capacity>0 && !destination) ||
       (!script_code && !call(1)) || (key_value && !call(2)))return invalid();

    try{
        const auto first=script_code?std::string{}:read_ansi(memory,call(1));
        const auto second=key_value?read_ansi(memory,call(2)):std::string{};
        const auto script=signed_argument(call(1));
        const auto* native=object->second.get();
        return finish(icu_preserving_output<UChar>(memory,destination,capacity,status,
            [&](UChar* buffer,std::int32_t length,UErrorCode* error){
                if(key_value)return uldn_keyValueDisplayName(native,first.c_str(),second.c_str(),buffer,length,error);
                if(script_code){
                    const auto code=script>=0 && script<USCRIPT_CODE_LIMIT?static_cast<UScriptCode>(script):USCRIPT_INVALID_CODE;
                    return uldn_scriptCodeDisplayName(native,code,buffer,length,error);
                }
                const auto match=std::find_if(std::begin(lookups),std::end(lookups),
                    [&](const auto& lookup){return lookup.symbol==name;});
                if(match==std::end(lookups))throw HostCallFault("ICU 地區顯示名稱服務未登錄："+std::string(name));
                return match->lookup(native,first.c_str(),buffer,length,error);
            }));
    }catch(const std::bad_alloc&){
        status.value=U_MEMORY_ALLOCATION_ERROR;
        status.commit();
        return finish();
    }
}
}
