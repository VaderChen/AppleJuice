#include "x64arm/windows_services.hpp"
#include "x64arm/windows_property_keys.hpp"
#include "x64arm/windows_guid.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <new>

namespace x64arm {
namespace {
constexpr WindowsPropertyKeyDefinition catalog[]{
#include "windows_property_keys.inc"
};
constexpr WindowsPropertyKeyAlias aliases[]{
#include "windows_property_key_aliases.inc"
};
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,
    not_found=0x8002802bU,out_of_memory=0x8007000eU,buffer_small=0x8007007aU;
std::optional<WindowsPropertyKey> parse_property_key(std::u16string_view text){
    if(text.size()<40 || text[38]!=u' ')return {};
    const auto guid=parse_windows_guid(text.substr(0,38),true);if(!guid)return {};
    std::uint32_t pid=0;
    for(const auto c:text.substr(39)){
        if(c<u'0' || c>u'9')return {};
        const auto digit=static_cast<std::uint32_t>(c-u'0');
        if(pid>(UINT32_MAX-digit)/10U)return {};
        pid=pid*10U+digit;
    }
    WindowsPropertyKey key{};std::copy(guid->begin(),guid->end(),key.begin());
    for(unsigned i=0;i<4;++i)key[16+i]=static_cast<std::uint8_t>(pid>>(i*8));
    return key;
}
std::u16string property_key_text(const WindowsPropertyKey& key){
    WindowsGuid guid{};std::copy_n(key.begin(),16,guid.begin());
    std::uint32_t pid=0;for(unsigned i=0;i<4;++i)pid|=static_cast<std::uint32_t>(key[16+i])<<(i*8);
    auto text=windows_guid_text(guid);text+=u' ';
    for(const auto c:std::to_string(pid))text+=static_cast<char16_t>(c);
    return text;
}
}
std::span<const WindowsPropertyKeyDefinition> windows_property_key_catalog(){return catalog;}
std::span<const WindowsPropertyKeyAlias> windows_property_key_aliases(){return aliases;}
std::optional<WindowsPropertyKey> windows_property_key_from_name(std::u16string_view name){
    const auto legacy=std::lower_bound(std::begin(aliases),std::end(aliases),name,
        [](const auto& item,auto value){return item.name<value;});
    if(legacy!=std::end(aliases) && legacy->name==name)name=legacy->canonical_name;
    const auto entry=std::lower_bound(std::begin(catalog),std::end(catalog),name,
        [](const auto& item,auto value){return item.name<value;});
    if(entry!=std::end(catalog) && entry->name==name)return entry->key;
    return {};
}
std::u16string_view windows_property_key_name(const WindowsPropertyKey& key){
    const auto entry=std::find_if(std::begin(catalog),std::end(catalog),[&](const auto& item){return item.key==key;});
    return entry==std::end(catalog)?std::u16string_view{}:entry->name;
}
std::span<const WindowsServiceDescriptor> property_key_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"propsys.dll","PSGetPropertyKeyFromName",2},{"propsys.dll","PSGetNameFromPropertyKey",2},
        {"propsys.dll","PSPropertyKeyFromString",2},{"propsys.dll","PSStringFromPropertyKey",3}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_property_keys(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto done=[&](std::uint32_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    if(!arg(1))return done(pointer_error);
    const std::uint8_t bits=x86_?32:64;
    try {
        if(name=="PSGetPropertyKeyFromName" || name=="PSPropertyKeyFromString"){
            if(!arg(0))return done(invalid);
            const auto text=read_utf16(memory,GuestAddress{arg(0)});
            const auto key=name=="PSGetPropertyKeyFromName"?windows_property_key_from_name(text):parse_property_key(text);
            if(!key)return done(name=="PSGetPropertyKeyFromName"?not_found:invalid);
            memory.write(GuestAddress{arg(1)},*key);return done(0);
        }
        if(name=="PSGetNameFromPropertyKey"){
            memory.check(GuestAddress{arg(1)},bits/8U,Access::Write);
            WindowsPropertyKey key{};if(arg(0))memory.read(GuestAddress{arg(0)},key);
            memory.store(GuestAddress{arg(1)},bits,0);
            if(!arg(0))return done(invalid);
            const auto canonical=windows_property_key_name(key);if(canonical.empty())return done(not_found);
            const auto bytes=utf16_bytes(canonical);const auto allocation=allocate_heap(memory,bytes.size());
            if(!allocation)return done(out_of_memory);
            try {
                gui_.task_allocations.insert(allocation);
                memory.write(GuestAddress{allocation},bytes);memory.store(GuestAddress{arg(1)},bits,allocation);
            }catch(...){gui_.task_allocations.erase(allocation);free_heap(memory,allocation,state);throw;}
            return done(0);
        }
        if(!arg(0))return done(invalid);
        WindowsPropertyKey key{};memory.read(GuestAddress{arg(0)},key);
        const auto text=property_key_text(key);const auto count=static_cast<std::uint32_t>(arg(2));
        if(count<=text.size())return done(buffer_small);
        memory.write(GuestAddress{arg(1)},utf16_bytes(text));return done(0);
    }catch(const std::bad_alloc&){return done(out_of_memory);}
    catch(const std::invalid_argument&){return done(invalid);}
}
}
