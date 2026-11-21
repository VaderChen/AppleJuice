#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "builtin_module_data.hpp"
#include "icu_guest_abi.hpp"
#include "service_callback_task.hpp"
#include "windows_math_environment.hpp"
#include <unicode/icudataver.h>
#include <unicode/uchar.h>
#include <unicode/uscript.h>
#include <unicode/ustring.h>
#include <algorithm>
#include <bit>

namespace x64arm {
namespace {
using namespace icu_abi;

template<auto Function> struct ScalarIcuCall;
template<class Result,class... Args,Result(*Function)(Args...)>
struct ScalarIcuCall<Function> {
    static constexpr auto arguments=sizeof...(Args);
    template<std::size_t... I>
    static std::uint32_t invoke(const WindowsCall& call,std::index_sequence<I...>){
        return static_cast<std::uint32_t>(Function(static_cast<Args>(signed_argument(call(I)))...));
    }
    static std::uint32_t invoke(const WindowsCall& call){return invoke(call,std::index_sequence_for<Args...>{});}
};

struct IcuScalar {
    WindowsServiceDescriptor descriptor;
    std::uint32_t (*invoke)(const WindowsCall&);
};

#define ICU_SCALAR(symbol) \
    {{"icu.dll",#symbol,ScalarIcuCall<&symbol>::arguments,0,0,0,true},ScalarIcuCall<&symbol>::invoke},
#define ICU_PREDICATE(symbol) {{"icu.dll",#symbol,1,0,0,1,true},ScalarIcuCall<&symbol>::invoke},
#define ICU_BYTE_PROPERTY(symbol, byte, type) {{"icu.dll",#symbol,1,0,0,1,true},ScalarIcuCall<&symbol>::invoke},
constexpr IcuScalar scalar_calls[]{
#include "icu_character_scalars.inc"
};
#undef ICU_SCALAR
#undef ICU_PREDICATE
#undef ICU_BYTE_PROPERTY


struct CategoryRange { UChar32 start,limit; UCharCategory category; };
UBool U_CALLCONV capture_category(const void* context,UChar32 start,UChar32 limit,UCharCategory category){
    auto& ranges=*static_cast<std::vector<CategoryRange>*>(const_cast<void*>(context));
    ranges.push_back({start,limit,category});return true;
}

struct CodepointName {UChar32 code;std::string name;};
UBool U_CALLCONV capture_name(void* context,UChar32 code,UCharNameChoice,const char* name,std::int32_t length){
    auto& names=*static_cast<std::vector<CodepointName>*>(context);
    names.push_back({code,std::string(name,static_cast<std::size_t>(length))});return true;
}
}

std::span<const WindowsServiceDescriptor> icu_character_service_descriptors(){
    static const auto table=[]{
        std::vector<WindowsServiceDescriptor> descriptors;
        descriptors.reserve(std::size(scalar_calls)+24);
        for(const auto& scalar:scalar_calls)descriptors.push_back(scalar.descriptor);
        constexpr std::pair<std::string_view,unsigned> services[]{
            {"u_charAge",2},{"u_charFromName",3},{"u_charName",5},{"u_charsToUChars",3},
            {"u_UCharsToChars",3},{"u_enumCharNames",6},{"u_enumCharTypes",2},{"u_errorName",1},
            {"u_getDataVersion",2},{"u_getFC_NFKC_Closure",4},{"u_getNumericValue",1},
            {"u_getPropertyEnum",1},{"u_getPropertyName",2},{"u_getPropertyValueEnum",2},
            {"u_getPropertyValueName",3},{"u_getUnicodeVersion",1},{"u_getVersion",1},
            {"u_versionFromString",2},{"u_versionFromUString",2},{"u_versionToString",2},
            {"uscript_getCode",4},{"uscript_getName",1},{"uscript_getSampleString",4},{"uscript_getScript",2},
            {"uscript_getScriptExtensions",4},{"uscript_getShortName",1},
        };
        for(const auto& [name,count]:services)descriptors.push_back({"icu.dll",name,count,0,0,0,true});
        return descriptors;
    }();
    return table;
}

std::uint64_t WindowsServices::publish_icu_literal(GuestMemory& memory,const char* text){
    if(!text)return 0;
    if(const auto found=icu_character_literals_.find(text);found!=icu_character_literals_.end())return found->second;
    std::vector<std::uint64_t> created;
    const auto module=load_module(memory,u"C:\\Windows\\System32\\icu.dll",0,created);
    const auto where=detail::initialize_builtin_module_data(memory,runtime_.builtin_images.at(module),
        icu_bytes(text,std::char_traits<char>::length(text)+1),1);
    icu_character_literals_.emplace(text,where);
    return where;
}

std::optional<std::uint32_t> WindowsServices::invoke_icu_character(GuestState& state,GuestMemory& memory,
        const WindowsServiceDescriptor& service){
    if(service.symbol=="u_enumCharTypes" || service.symbol=="u_enumCharNames"){
        return nested_continuation(state,memory,"ICU 字元列舉",[this,service](GuestState& s,GuestMemory& m)
            ->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall arg(s,m,service);
            const auto done=[&]{arg.return_to(s,0);return std::optional<std::uint32_t>{};};
            if(service.symbol=="u_enumCharTypes"){
                const auto callback=arg(0),context=arg(1);
                if(!callback)co_return done();
                std::vector<CategoryRange> ranges;
                u_enumCharTypes(capture_category,&ranges);
                for(const auto& range:ranges){
                    const auto value=co_await service_callback::call_value(callback,{context,
                        static_cast<std::uint32_t>(range.start),static_cast<std::uint32_t>(range.limit),
                        static_cast<std::uint32_t>(range.category)});
                    if(!static_cast<std::uint8_t>(value))break;
                }
                co_return done();
            }
            IcuStatus status(m,arg(5));
            if(U_FAILURE(status.value))co_return done();
            const auto interpretation=static_cast<UCharNameChoice>(signed_argument(arg(4)));
            const auto callback=arg(2),context=arg(3);
            if(!callback || static_cast<unsigned>(interpretation)>=U_CHAR_NAME_CHOICE_COUNT){
                status.value=U_ILLEGAL_ARGUMENT_ERROR;status.commit();co_return done();
            }
            std::uint64_t text_at=0;
            struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::uint64_t& address;
                ~Release(){if(address)(void)self.free_heap(memory,address,state);}
            } release{*this,m,s,text_at};
            std::uint64_t capacity=0;
            const auto limit=std::min<std::uint32_t>(static_cast<std::uint32_t>(arg(1)),0x110000);
            bool running=true;
            for(auto next=static_cast<std::uint32_t>(arg(0));running && next<limit;){
                const auto end=std::min(next+256,limit);
                std::vector<CodepointName> names;
                u_enumCharNames(static_cast<UChar32>(next),static_cast<UChar32>(end),capture_name,&names,interpretation,&status.value);
                if(U_FAILURE(status.value)){status.commit();break;}
                for(const auto& item:names){
                    const auto size=item.name.size()+1;
                    if(size>capacity){
                        const auto block=text_at?reallocate_heap(m,text_at,size,s):allocate_heap(m,size);
                        if(!block){status.value=U_MEMORY_ALLOCATION_ERROR;status.commit();running=false;break;}
                        text_at=block;capacity=size;
                    }
                    m.write(GuestAddress{text_at},icu_bytes(item.name.c_str(),size));
                    const auto more=co_await service_callback::call_value(callback,{context,static_cast<std::uint32_t>(item.code),
                        static_cast<std::uint32_t>(interpretation),text_at,item.name.size()});
                    if(!static_cast<std::uint8_t>(more)){running=false;break;}
                }
                next=end;
            }
            co_return done();
        });
    }
    const WindowsCall arg(state,memory,service);
    const auto name=service.symbol;
    const auto i=[&](unsigned n){return signed_argument(arg(n));};
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    for(const auto& entry:scalar_calls)if(entry.descriptor.symbol==name)return finish(entry.invoke(arg));
    const auto literal=[&](const char* text){return publish_icu_literal(memory,text);};
    if(name=="u_getNumericValue"){
        windows_floating_return(state,x86_,u_getNumericValue(i(0)),false);
        return finish(0);
    }
    if(name=="u_errorName")return finish(literal(u_errorName(static_cast<UErrorCode>(i(0)))));
    if(name=="u_getPropertyName")
        return finish(literal(u_getPropertyName(static_cast<UProperty>(i(0)),static_cast<UPropertyNameChoice>(i(1)))));
    if(name=="u_getPropertyValueName")
        return finish(literal(u_getPropertyValueName(static_cast<UProperty>(i(0)),i(1),static_cast<UPropertyNameChoice>(i(2)))));
    if(name=="uscript_getName" || name=="uscript_getShortName"){
        const auto code=static_cast<UScriptCode>(i(0));
        return finish(literal(name=="uscript_getName"?uscript_getName(code):uscript_getShortName(code)));
    }
    if(name=="u_getPropertyEnum" || name=="u_getPropertyValueEnum"){
        const bool property=name=="u_getPropertyEnum";
        const auto input=arg(property?0U:1U);
        const auto text=input?read_ansi(memory,input):std::string{};
        const auto* pointer=input?text.c_str():nullptr;
        return finish(static_cast<std::uint32_t>(property?u_getPropertyEnum(pointer):u_getPropertyValueEnum(static_cast<UProperty>(i(0)),pointer)));
    }
    if(name=="u_getVersion" || name=="u_getUnicodeVersion" || name=="u_getDataVersion" || name=="u_charAge" ||
       name=="u_versionFromString" || name=="u_versionFromUString"){
        UVersionInfo version{};
        const auto output=arg(name=="u_charAge"?1U:0U);
        if(name=="u_getDataVersion"){
            IcuStatus status(memory,arg(1));
            if(U_FAILURE(status.value))return finish(0);
            u_getDataVersion(version,&status.value);status.commit();
            if(U_FAILURE(status.value))return finish(0);
        }else if(name=="u_getVersion")u_getVersion(version);
        else if(name=="u_getUnicodeVersion")u_getUnicodeVersion(version);
        else if(name=="u_charAge")u_charAge(i(0),version);
        else if(name=="u_versionFromString"){
            const auto input=arg(1)?read_ansi(memory,arg(1)):std::string{};
            u_versionFromString(version,arg(1)?input.c_str():nullptr);
        }else{
            if(!arg(1))return finish(0);
            const auto input=read_utf16(memory,GuestAddress{arg(1)});
            u_versionFromUString(version,input.c_str());
        }
        if(output)memory.write(GuestAddress{output},icu_bytes(version,std::size(version)));
        return finish(0);
    }
    if(name=="u_versionToString"){
        if(!arg(1))return finish(0);
        UVersionInfo version{};
        if(arg(0))memory.read(GuestAddress{arg(0)},version);
        std::array<char,U_MAX_VERSION_STRING_LENGTH> text{};
        u_versionToString(arg(0)?version:nullptr,text.data());
        memory.write(GuestAddress{arg(1)},icu_bytes(text.data(),std::char_traits<char>::length(text.data())+1));return finish(0);
    }
    if(name=="u_charsToUChars" || name=="u_UCharsToChars"){
        const auto length=i(2);
        if(length<=0)return finish(0);
        const auto count=static_cast<std::size_t>(length);
        const bool widen=name=="u_charsToUChars";
        memory.check(GuestAddress{arg(0)},count*(widen?1U:sizeof(UChar)),Access::Read);
        memory.check(GuestAddress{arg(1)},count*(widen?sizeof(UChar):1U),Access::Write);
        std::vector<char> bytes(static_cast<std::size_t>(length));
        std::vector<UChar> words(static_cast<std::size_t>(length));
        if(widen){
            memory.read(GuestAddress{arg(0)},{reinterpret_cast<std::uint8_t*>(bytes.data()),bytes.size()});
            u_charsToUChars(bytes.data(),words.data(),length);
            memory.write(GuestAddress{arg(1)},icu_bytes(words.data(),words.size()));
        }else{
            memory.read(GuestAddress{arg(0)},{reinterpret_cast<std::uint8_t*>(words.data()),words.size()*sizeof(UChar)});
            u_UCharsToChars(words.data(),bytes.data(),length);
            memory.write(GuestAddress{arg(1)},icu_bytes(bytes.data(),bytes.size()));
        }
        return finish(0);
    }
    if(name=="u_charName"){
        IcuStatus status(memory,arg(4));
        return finish(icu_output<char>(memory,arg(2),i(3),status,IcuOutput::TruncatedString,[&](auto* out,auto size,auto* error){
            return u_charName(i(0),static_cast<UCharNameChoice>(i(1)),out,size,error);
        }));
    }
    if(name=="u_charFromName"){
        IcuStatus status(memory,arg(2));
        if(U_FAILURE(status.value))return finish(static_cast<std::uint32_t>(u_charFromName(U_UNICODE_CHAR_NAME,nullptr,&status.value)));
        const auto input=arg(1)?read_ansi(memory,arg(1)):std::string{};
        const auto code=u_charFromName(static_cast<UCharNameChoice>(i(0)),arg(1)?input.c_str():nullptr,&status.value);
        status.commit();return finish(static_cast<std::uint32_t>(code));
    }
    if(name=="uscript_getScript"){
        IcuStatus status(memory,arg(1));
        const auto script=uscript_getScript(i(0),&status.value);status.commit();
        return finish(static_cast<std::uint32_t>(script));
    }
    if(name=="uscript_getCode" || name=="uscript_getScriptExtensions"){
        IcuStatus status(memory,arg(3));
        if(U_FAILURE(status.value))return finish(0);
        const bool by_name=name=="uscript_getCode";
        const auto input=by_name && arg(0)?read_ansi(memory,arg(0)):std::string{};
        const auto kind=by_name?IcuOutput::Array:IcuOutput::TruncatedArray;
        return finish(icu_output<UScriptCode>(memory,arg(1),i(2),status,kind,[&](auto* out,auto size,auto* error){
            return by_name?uscript_getCode(arg(0)?input.c_str():nullptr,out,size,error):uscript_getScriptExtensions(i(0),out,size,error);
        }));
    }
    if(name=="uscript_getSampleString" || name=="u_getFC_NFKC_Closure"){
        IcuStatus status(memory,arg(3));
        const bool sample=name=="uscript_getSampleString";
        return finish(icu_output<UChar>(memory,arg(1),i(2),status,IcuOutput::String,[&](auto* out,auto size,auto* error){
            return sample?uscript_getSampleString(static_cast<UScriptCode>(i(0)),out,size,error):u_getFC_NFKC_Closure(i(0),out,size,error);
        }));
    }
    throw HostCallFault("未知 ICU 字元服務入口");
}
}
