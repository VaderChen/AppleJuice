#include "x64arm/windows_services.hpp"
#include "windows_crt_character.hpp"
#include "windows_locale_data.hpp"
#include "crt_c_locale.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <unicode/uchar.h>

namespace x64arm {
struct CrtCharacterOperation {
    enum class Kind { Predicate, Mask, Lower, Upper, Property } kind;
    unsigned mask{};
    bool wide{},explicit_locale{};
};
namespace {
using Kind=CrtCharacterOperation::Kind;
struct CharacterService {WindowsServiceDescriptor descriptor;CrtCharacterOperation operation;};
// 同一份契約產生公開 API 與計算操作；不依呼叫端、模組位址或測項選路。
#define CHARACTER_FORM(symbol,kind,mask,wide,argc) \
    CharacterService{{"crt",symbol,argc},{Kind::kind,mask,wide,false}}, \
    CharacterService{{"crt","_" symbol "_l",argc+1},{Kind::kind,mask,wide,true}}
#define CHARACTER_PREDICATE(name,mask) \
    CHARACTER_FORM("is" name,Predicate,mask,false,1), \
    CHARACTER_FORM("isw" name,Predicate,mask,true,1)
constexpr std::array character_services{
    CHARACTER_PREDICATE("alnum",0x107),CHARACTER_PREDICATE("alpha",0x103),
    CHARACTER_PREDICATE("blank",0x40),CHARACTER_PREDICATE("cntrl",0x20),
    CHARACTER_PREDICATE("digit",4),CHARACTER_PREDICATE("graph",0x117),
    CHARACTER_PREDICATE("lower",2),CHARACTER_PREDICATE("print",0x157),
    CHARACTER_PREDICATE("punct",0x10),CHARACTER_PREDICATE("space",8),
    CHARACTER_PREDICATE("upper",1),CHARACTER_PREDICATE("xdigit",0x80),
    CHARACTER_FORM("tolower",Lower,0,false,1),CHARACTER_FORM("toupper",Upper,0,false,1),
    CHARACTER_FORM("towlower",Lower,0,true,1),CHARACTER_FORM("towupper",Upper,0,true,1),
    CharacterService{{"crt","_isctype",2},{Kind::Mask,0,false,false}},
    CharacterService{{"crt","_isctype_l",3},{Kind::Mask,0,false,true}},
    CharacterService{{"crt","iswctype",2},{Kind::Mask,0,true,false}},
    CharacterService{{"crt","_iswctype_l",3},{Kind::Mask,0,true,true}},
    CharacterService{{"crt","wctype",1},{Kind::Property,0,true,false}}
};
#undef CHARACTER_PREDICATE
#undef CHARACTER_FORM
const CrtCharacterOperation* find_operation(const WindowsServiceDescriptor& descriptor){
    if(descriptor.family!="crt")return nullptr;
    const auto found=std::find_if(character_services.begin(),character_services.end(),[&](const auto& item){
        return item.descriptor.symbol==descriptor.symbol;
    });
    return found==character_services.end()?nullptr:&found->operation;
}
unsigned unicode_mask(char16_t value){
    const auto c=static_cast<UChar32>(value);unsigned mask=0;
    if(u_isupper(c))mask|=1;if(u_islower(c))mask|=2;if(u_isdigit(c))mask|=4;
    if(u_isUWhiteSpace(c))mask|=8;if(u_isgraph(c) && !u_isalnum(c))mask|=16;
    if(u_iscntrl(c))mask|=32;if(u_isblank(c) && !u_iscntrl(c))mask|=64;
    if(u_isxdigit(c))mask|=128;if(u_isalpha(c))mask|=0x100;
    return mask;
}
}
std::span<const WindowsServiceDescriptor> crt_character_service_descriptors(){
    static constexpr auto descriptors=[] {
        std::array<WindowsServiceDescriptor,character_services.size()> result{};
        for(std::size_t n=0;n<result.size();++n)result[n]=character_services[n].descriptor;
        return result;
    }();
    return descriptors;
}
const CrtCharacterOperation* crt_character_operation(const WindowsServiceDescriptor& service){
    if(service.family!="crt")return nullptr;
    const auto table=runtime_service_descriptors();
    // 只快取不可變 descriptor 的操作身分；locale 與客體資料每次由當前執行緒取用。
    static const auto operations=[table] {
        std::vector<const CrtCharacterOperation*> result;result.reserve(table.size());
        for(const auto& descriptor:table)result.push_back(find_operation(descriptor));
        return result;
    }();
    const auto address=reinterpret_cast<std::uintptr_t>(&service),begin=reinterpret_cast<std::uintptr_t>(table.data());
    if(address>=begin && address-begin<table.size_bytes() && (address-begin)%sizeof(service)==0)
        return operations[(address-begin)/sizeof(service)];
    return find_operation(service);
}
bool crt_character_requested() noexcept{
    static const bool enabled=[] {const auto* value=std::getenv("X64ARM_CRT_CHARACTER");return !value || std::string_view(value)!="0";}();
    return enabled;
}
std::optional<std::uint32_t> WindowsServices::invoke_crt_character(GuestState& state,GuestMemory& memory,
                                                                const WindowsServiceDescriptor& service,
                                                                const ResidentHostEnvironment* environment){
    const auto* operation=crt_character_operation(service);
    if(!operation)throw HostCallFault("CRT 字元操作沒有對應契約");
    const WindowsCall arg(state,memory,service);
    bool retained=false;
    const auto returned=[&](std::uint32_t value){
        arg.return_to(state,value);
#ifdef X64ARM_HOST_SERVICES_RETAINED_FP
        if(retained && environment)environment->retain();
#endif
        return std::optional<std::uint32_t>{};
    };
    if(operation->kind==Kind::Property){
        const auto name=read_ansi(memory,arg(0),255);
        for(const auto& item:character_services)if(item.operation.kind==Kind::Predicate && item.operation.wide &&
                !item.operation.explicit_locale && item.descriptor.symbol.substr(3)==name)return returned(item.operation.mask);
        return returned(0);
    }
    std::uint64_t locale=operation->explicit_locale?arg(operation->kind==Kind::Mask?2U:1U):0;
    if(!locale){
        const auto thread=runtime_.crt_thread_locales.find(threads_.current);
        locale=thread==runtime_.crt_thread_locales.end()?runtime_.crt_current_locale:thread->second;
    }
    // 尚未建立 locale 物件時就是 CRT 初始 C locale；查詢不為單一字元配置客體堆積。
    static const CrtLocaleCategory initial_locale{};const auto* category=&initial_locale;
    if(locale){
        const auto found=runtime_.crt_locales.find(locale);
        if(found==runtime_.crt_locales.end()){
            if(environment)environment->restore();
            memory.store(GuestAddress{runtime_.errno_cell},32,22);
            if(current_invalid_parameter_handler()){
                start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,0);
                return std::nullopt;
            }
            return 0xc0000417U;
        }
        category=&found->second.categories[2];
    }
    // C locale 的整個家族只用整數；其餘 locale 可能進入 ICU，先建立原宿主環境。
    retained=!category->codepage;
    if(!retained && environment)environment->restore();
    const auto c=operation->wide?static_cast<std::uint32_t>(static_cast<std::uint16_t>(arg(0))):static_cast<std::uint32_t>(arg(0));
    if(operation->kind==Kind::Lower || operation->kind==Kind::Upper){
        const bool upper=operation->kind==Kind::Upper;
        if(!category->codepage)return returned(upper && c>='a' && c<='z'?c-32:!upper && c>='A' && c<='Z'?c+32:c);
        if(operation->wide)return returned(windows_crt_map_character(static_cast<char16_t>(c),*category,upper));
        if(c>255)return returned(c); // EOF 及既有超範圍輸入保持值。
        const auto& table=windows_crt_character_table(*category);
        return returned((upper?table.upper:table.lower)[c]);
    }
    unsigned mask=0;
    if(!category->codepage)mask=crt_c_locale::character_mask(c);
    else if(operation->wide){if(c!=0xffff)mask=unicode_mask(static_cast<char16_t>(c));}
    else if(c<=255)mask=windows_crt_character_table(*category).masks[c+1];
    const auto requested=operation->kind==Kind::Mask?static_cast<std::uint16_t>(arg(1)):operation->mask;
    // isblank 包含水平 Tab；print 的 _BLANK 組合不能因此將控制字元當成可列印字元。
    if(operation->kind==Kind::Predicate && requested==0x40 && c=='\t')mask|=0x40;
    if(operation->kind==Kind::Mask)return returned(mask&requested);
    // 窄 isdigit／isxdigit 的契約不隨 locale 增加非 ASCII 數字。
    if(!operation->wide && (requested==4 || requested==0x80))mask=crt_c_locale::character_mask(c);
    return returned((mask&requested)!=0);
}
}
