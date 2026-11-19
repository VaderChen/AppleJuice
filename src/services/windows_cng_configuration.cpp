// bcrypt（ncrypt 的同名匯出轉送到這裡）的 CNG 設定：設定內容（context）中各介面的函式清單、函式的提供者順序，以及提供者註冊。
// - 存在沙箱登錄 HKLM\SYSTEM\CurrentControlSet\Control\Cryptography\Configuration\Local\<內容>\<介面八位十六進位>：Functions（REG_MULTI_SZ），
//   <函式> 子機碼的 Providers（REG_MULTI_SZ）。提供者在 ...\Cryptography\Providers\<名稱>：Aliases（REG_MULTI_SZ），UM／KM 子機碼的 Image（REG_SZ）
//   與 <介面> 子機碼的 Functions、Flags（REG_DWORD）。
// - Default 內容在登錄沒有該介面時採用內建演算法清單（與 BCryptEnumAlgorithms 共用），各函式的提供者為 Microsoft Primitive Provider；
//   第一次修改某介面時先把預設值寫入登錄再套用。其他內容須已存在於登錄。
// - 只支援 CRYPT_LOCAL；CRYPT_DOMAIN 回 STATUS_NOT_SUPPORTED。寫入前依登錄 ACL 檢查 KEY_SET_VALUE|KEY_CREATE_SUB_KEY（不足為 STATUS_ACCESS_DENIED）。
// - 加入已存在的函式或提供者時改到指定位置；CRYPT_PRIORITY_TOP（0）／CRYPT_PRIORITY_BOTTOM（0xffffffff），其他值為索引（超出時放最後）。
// - BCryptOpenAlgorithmProvider 依同一份 Default 設定：登錄中已設定的介面若不含該演算法，回 STATUS_NOT_FOUND。
#include "x64arm/windows_services.hpp"
#include "x64arm/registry_store.hpp"
#include "x64arm/unicode.hpp"
#include "registry_direct_write.hpp"
#include "cng_algorithms.hpp"
#include <algorithm>
#include <functional>
#include <optional>

namespace x64arm {
namespace {
using namespace registry_direct;
constexpr std::u16string_view config_display=u"\x02\\SYSTEM\\CurrentControlSet\\Control\\Cryptography\\Configuration\\Local";
constexpr std::u16string_view providers_display=u"\x02\\SYSTEM\\CurrentControlSet\\Control\\Cryptography\\Providers";
constexpr std::uint32_t success=0,invalid_parameter=0xc000000dU,not_supported=0xc00000bbU,not_found=0xc0000225U,buffer_too_small=0xc0000023U,
    no_memory=0xc0000017U,access_denied=0xc0000022U,name_collision=0xc0000035U;
constexpr std::uint32_t crypt_local=1,crypt_domain=2,priority_bottom=0xffffffffU,crypt_overwrite=1;
bool same(std::u16string_view a,std::u16string_view b){return folded(a)==folded(b);}
bool valid_interface(std::uint32_t id){return (id>=1 && id<=7) || (id>=0x10001 && id<=0x10004);}
std::u16string interface_name(std::uint32_t id){
    constexpr char16_t digits[]=u"0123456789ABCDEF";std::u16string out(8,u'0');for(unsigned i=0;i<8;++i)out[7-i]=digits[(id>>(i*4))&15U];return out;
}
std::vector<std::u16string> default_functions(std::u16string_view context,std::uint32_t id){
    std::vector<std::u16string> out;if(!same(context,u"Default"))return out;
    for(const auto& algorithm:cng_catalog::algorithms)if(algorithm.kind==id)out.emplace_back(algorithm.name);return out;
}
std::u16string context_display(std::u16string_view context){return std::u16string(config_display)+u"\\"+std::u16string(context);}
std::u16string interface_display(std::u16string_view context,std::uint32_t id){return context_display(context)+u"\\"+interface_name(id);}
bool context_exists(const RegistryState& registry,std::u16string_view context){return same(context,u"Default") || registry.keys.contains(folded(context_display(context)));}
// 介面目前的函式清單：登錄有該介面時以登錄為準，否則 Default 內容採用內建清單；兩者皆無時沒有此介面。
std::optional<std::vector<std::u16string>> read_functions(const RegistryState& registry,std::u16string_view context,std::uint32_t id){
    const auto key=folded(interface_display(context,id));
    if(registry.keys.contains(key))return multi_strings(find_value(registry,key,u"Functions"));
    auto defaults=default_functions(context,id);if(defaults.empty())return std::nullopt;return defaults;
}
std::vector<std::u16string> read_providers(const RegistryState& registry,std::u16string_view context,std::uint32_t id,std::u16string_view function){
    const auto key=folded(interface_display(context,id));
    if(registry.keys.contains(key))return multi_strings(find_value(registry,key+u"\\"+folded(function),u"Providers"));
    return {std::u16string(cng_catalog::primitive_provider)};
}
// 修改前把預設清單寫入登錄（只在該介面尚未存在於登錄時）。
void materialize(RegistryState& registry,std::u16string_view context,std::uint32_t id,std::uint64_t now){
    const auto display=interface_display(context,id);if(registry.keys.contains(folded(display)))return;
    const auto functions=default_functions(context,id);put_value(registry,display,u"Functions",7,multi_string_bytes(functions),now);
    for(const auto& function:functions)put_value(registry,display+u"\\"+function,u"Providers",7,multi_string_bytes({std::u16string(cng_catalog::primitive_provider)}),now);
}
void place(std::vector<std::u16string>& items,std::u16string_view name,std::uint32_t position){
    std::erase_if(items,[&](const auto& item){return same(item,name);});
    const auto at=position==priority_bottom?items.size():std::min<std::size_t>(position,items.size());
    items.insert(items.begin()+static_cast<std::ptrdiff_t>(at),std::u16string(name));
}
}
bool WindowsServices::cng_function_enabled(std::u16string_view algorithm){
    // BCryptOpenAlgorithmProvider：Default 內容已在登錄設定該演算法所屬介面時，必須列在 Functions 中。
    const auto* entry=std::find_if(std::begin(cng_catalog::algorithms),std::end(cng_catalog::algorithms),[&](const auto& a){return same(a.name,algorithm);});
    if(entry==std::end(cng_catalog::algorithms) || (!registry_file_.empty() && !runtime_.network.external_io))return true;
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return true;
    const auto key=folded(interface_display(u"Default",entry->kind));if(!runtime_.registry.keys.contains(key))return true;
    const auto functions=multi_strings(find_value(runtime_.registry,key,u"Functions"));
    return std::any_of(functions.begin(),functions.end(),[&](const auto& f){return same(f,algorithm);});
}
std::uint32_t WindowsServices::cng_configuration_call(GuestState& state,GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto text=[&](std::uint64_t pointer)->std::optional<std::u16string>{if(!pointer)return std::nullopt;auto value=read_utf16(memory,GuestAddress{pointer},256);if(value.empty() || value.size()>=256)return std::nullopt;return value;};
    const auto pointer_at=[&](std::uint64_t at){return memory.load(GuestAddress{at},bits);};
    const auto writable=[&](std::u16string probe){
        probe=folded(probe);
        while(!runtime_.registry.keys.contains(probe)){const auto slash=probe.find_last_of(u'\\');if(slash==probe.npos)return true;probe.resize(slash);}
        return (registry_security_access(probe,6)&6U)==6U;
    };
    // 可寫交易內套用變更；ACL 不足、變更失敗或超出登錄上限時還原。
    const auto mutate=[&](const std::u16string& root,const std::function<std::uint32_t(RegistryState&,std::uint64_t)>& change)->std::uint32_t{
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_);if(transaction.error)return access_denied;
        if(!writable(root)){transaction.unchanged();return access_denied;}
        auto before=runtime_.registry;auto status=change(runtime_.registry,runtime_.system_filetime());
        if(!status && (runtime_.registry.keys.size()>65536 || runtime_.registry.bytes>64U*1024U*1024U))status=no_memory;
        if(status){runtime_.registry=std::move(before);transaction.unchanged();return status;}
        if(transaction.commit()){runtime_.registry=std::move(before);return access_denied;}
        poll_registry_notifications(true,folded(root),5);return success;
    };
    const auto table_status=[&](std::uint32_t table)->std::uint32_t{return table==crypt_local?success:table==crypt_domain?not_supported:invalid_parameter;};
    if(symbol=="BCryptEnumContextFunctions"){
        // (dwTable, pszContext, dwInterface, pcbBuffer, ppBuffer)：CRYPT_CONTEXT_FUNCTIONS{cFunctions, rgpszFunctions}，字串在同一塊。
        if(const auto status=table_status(u32(0)))return status;
        const auto context=text(arg(1));if(!context || !valid_interface(u32(2)) || !arg(3) || !arg(4))return invalid_parameter;
        std::optional<std::vector<std::u16string>> functions;
        {RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return not_found;
            if(!context_exists(runtime_.registry,*context))return not_found;functions=read_functions(runtime_.registry,*context,u32(2));}
        if(!functions)return not_found;
        const unsigned header=x86_?8U:16U;std::size_t size=header+functions->size()*unit;for(const auto& f:*functions)size+=(f.size()+1)*2;
        memory.check(GuestAddress{arg(3)},4,Access::Write);memory.check(GuestAddress{arg(4)},unit,Access::Write);
        auto block=pointer_at(arg(4));
        if(block){if(memory.load(GuestAddress{arg(3)},32)<size){memory.store(GuestAddress{arg(3)},32,size);return buffer_too_small;}}
        else {block=allocate_heap(memory,size);if(!block)return no_memory;}
        memory.check(GuestAddress{block},size,Access::Write);
        memory.store(GuestAddress{block},32,functions->size());memory.store(GuestAddress{block+(x86_?4U:8U)},bits,functions->empty()?0:block+header);
        auto at=block+header+functions->size()*unit;
        for(std::size_t i=0;i<functions->size();++i){const auto bytes=string_bytes((*functions)[i]);memory.write(GuestAddress{at},bytes);memory.store(GuestAddress{block+header+i*unit},bits,at);at+=bytes.size();}
        memory.store(GuestAddress{arg(3)},32,size);memory.store(GuestAddress{arg(4)},bits,block);return success;
    }
    if(symbol=="BCryptAddContextFunction" || symbol=="BCryptRemoveContextFunction"){
        // (dwTable, pszContext, dwInterface, pszFunction[, dwPosition])
        if(const auto status=table_status(u32(0)))return status;
        const auto context=text(arg(1)),function=text(arg(3));if(!context || !function || !valid_interface(u32(2)))return invalid_parameter;
        const bool add=symbol=="BCryptAddContextFunction";const auto id=u32(2);const auto position=add?u32(4):0U;
        return mutate(context_display(*context),[&](RegistryState& registry,std::uint64_t now)->std::uint32_t{
            if(!context_exists(registry,*context))return not_found;
            auto functions=read_functions(registry,*context,id).value_or(std::vector<std::u16string>{});
            const bool present=std::any_of(functions.begin(),functions.end(),[&](const auto& f){return same(f,*function);});
            if(!add && !present)return not_found;
            if(add && functions.size()>=1024 && !present)return no_memory;
            materialize(registry,*context,id,now);const auto display=interface_display(*context,id);
            if(add){place(functions,*function,position);ensure_key(registry,display+u"\\"+*function,now);}
            else {std::erase_if(functions,[&](const auto& f){return same(f,*function);});erase_tree(registry,folded(display+u"\\"+*function));}
            put_value(registry,display,u"Functions",7,multi_string_bytes(functions),now);return success;
        });
    }
    if(symbol=="BCryptAddContextFunctionProvider" || symbol=="BCryptRemoveContextFunctionProvider"){
        // (dwTable, pszContext, dwInterface, pszFunction, pszProvider[, dwPosition])：函式必須已在該介面。
        if(const auto status=table_status(u32(0)))return status;
        const auto context=text(arg(1)),function=text(arg(3)),provider=text(arg(4));if(!context || !function || !provider || !valid_interface(u32(2)))return invalid_parameter;
        const bool add=symbol=="BCryptAddContextFunctionProvider";const auto id=u32(2);const auto position=add?u32(5):0U;
        return mutate(context_display(*context),[&](RegistryState& registry,std::uint64_t now)->std::uint32_t{
            if(!context_exists(registry,*context))return not_found;
            const auto functions=read_functions(registry,*context,id);
            if(!functions || std::none_of(functions->begin(),functions->end(),[&](const auto& f){return same(f,*function);}))return not_found;
            auto providers=read_providers(registry,*context,id,*function);
            const bool present=std::any_of(providers.begin(),providers.end(),[&](const auto& p){return same(p,*provider);});
            if(!add && !present)return not_found;
            if(add && providers.size()>=64 && !present)return no_memory;
            materialize(registry,*context,id,now);
            if(add)place(providers,*provider,position);else std::erase_if(providers,[&](const auto& p){return same(p,*provider);});
            put_value(registry,interface_display(*context,id)+u"\\"+*function,u"Providers",7,multi_string_bytes(providers),now);return success;
        });
    }
    if(symbol=="BCryptRegisterProvider"){
        // (pszProvider, dwFlags, pReg)：CRYPT_PROVIDER_REG{cAliases, rgpszAliases, pUM, pKM}；CRYPT_IMAGE_REG{pszImage, cInterfaces, rgpInterfaces}；
        // CRYPT_INTERFACE_REG{dwInterface, dwFlags, cFunctions, rgpszFunctions}。已存在且沒有 CRYPT_OVERWRITE 時 STATUS_OBJECT_NAME_COLLISION。
        const auto name=text(arg(0));const auto flags=u32(1);if(!name || !arg(2) || (flags&~crypt_overwrite))return invalid_parameter;
        struct Image {std::u16string path;std::vector<std::pair<std::uint32_t,std::pair<std::uint32_t,std::vector<std::u16string>>>> interfaces;};
        std::vector<std::u16string> aliases;std::array<std::optional<Image>,2> images;
        const auto reg=arg(2);const auto alias_count=static_cast<std::uint32_t>(memory.load(GuestAddress{reg},32));if(alias_count>64)return invalid_parameter;
        const auto alias_list=pointer_at(reg+unit);if(alias_count && !alias_list)return invalid_parameter;
        for(std::uint32_t i=0;i<alias_count;++i){const auto alias=text(pointer_at(alias_list+i*unit));if(!alias)return invalid_parameter;aliases.push_back(*alias);}
        for(unsigned which=0;which<2;++which){
            const auto image=pointer_at(reg+unit*(2+which));if(!image)continue;
            Image item;const auto path=text(pointer_at(image));if(!path)return invalid_parameter;item.path=*path;
            const auto count=static_cast<std::uint32_t>(memory.load(GuestAddress{image+unit},32));const auto list=pointer_at(image+unit*2);
            if(count>64 || (count && !list))return invalid_parameter;
            for(std::uint32_t i=0;i<count;++i){
                const auto entry=pointer_at(list+i*unit);if(!entry)return invalid_parameter;
                const auto id=static_cast<std::uint32_t>(memory.load(GuestAddress{entry},32)),entry_flags=static_cast<std::uint32_t>(memory.load(GuestAddress{entry+4},32));
                const auto functions=static_cast<std::uint32_t>(memory.load(GuestAddress{entry+8},32));const auto names=pointer_at(entry+(x86_?12U:16U));
                if(!valid_interface(id) || functions>1024 || (functions && !names))return invalid_parameter;
                std::vector<std::u16string> list_names;for(std::uint32_t f=0;f<functions;++f){const auto function=text(pointer_at(names+f*unit));if(!function)return invalid_parameter;list_names.push_back(*function);}
                item.interfaces.push_back({id,{entry_flags,std::move(list_names)}});
            }
            images[which]=std::move(item);
        }
        const auto display=std::u16string(providers_display)+u"\\"+*name;
        return mutate(display,[&](RegistryState& registry,std::uint64_t now)->std::uint32_t{
            const auto canonical=folded(display);const bool exists=registry.keys.contains(canonical) || same(*name,cng_catalog::primitive_provider);
            if(exists && !(flags&crypt_overwrite))return name_collision;
            erase_tree(registry,canonical);ensure_key(registry,display,now);
            if(!aliases.empty())put_value(registry,display,u"Aliases",7,multi_string_bytes(aliases),now);
            for(unsigned which=0;which<2;++which)if(images[which]){
                const auto image_display=display+(which?u"\\KM":u"\\UM");put_value(registry,image_display,u"Image",1,string_bytes(images[which]->path),now);
                for(const auto& [id,entry]:images[which]->interfaces){const auto key=image_display+u"\\"+interface_name(id);
                    put_value(registry,key,u"Functions",7,multi_string_bytes(entry.second),now);put_value(registry,key,u"Flags",4,{static_cast<std::uint8_t>(entry.first),static_cast<std::uint8_t>(entry.first>>8),static_cast<std::uint8_t>(entry.first>>16),static_cast<std::uint8_t>(entry.first>>24)},now);}
            }
            return success;
        });
    }
    if(symbol=="BCryptUnregisterProvider"){
        // 內建的 Microsoft Primitive Provider 不能移除（STATUS_ACCESS_DENIED）；只移除登錄中註冊的提供者。
        const auto name=text(arg(0));if(!name)return invalid_parameter;
        const auto display=std::u16string(providers_display)+u"\\"+*name;
        return mutate(display,[&](RegistryState& registry,std::uint64_t)->std::uint32_t{
            const auto canonical=folded(display);
            if(!registry.keys.contains(canonical))return same(*name,cng_catalog::primitive_provider)?access_denied:not_found;
            erase_tree(registry,canonical);return success;
        });
    }
    (void)state;throw HostCallFault("未知的 CNG 設定服務");
}
}
