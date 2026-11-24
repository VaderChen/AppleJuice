#include "ansi_codepage.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "legacy_crypto_catalog.hpp"
#include "x64arm/private_key_store.hpp"
#include "x64arm/file_security.hpp"
#include "x64arm/registry_store.hpp"
#include <cstdio>
#include <cstring>
namespace x64arm {
namespace {
// 預設提供者機碼的邏輯路徑（未套用 WOW64 檢視）與各層的顯示名稱。
std::vector<std::pair<std::u16string,std::u16string>> default_provider_components(std::uint32_t type,bool user){
    char digits[8];std::snprintf(digits,sizeof digits,"%03u",type%1000U);const auto number=utf8_to_utf16(digits);
    if(user)return {{u"software",u"Software"},{u"microsoft",u"Microsoft"},{u"cryptography",u"Cryptography"},{u"provider type "+number,u"Provider Type "+number}};
    return {{u"software",u"SOFTWARE"},{u"microsoft",u"Microsoft"},{u"cryptography",u"Cryptography"},{u"defaults",u"Defaults"},{u"provider types",u"Provider Types"},{u"type "+number,u"Type "+number}};
}
}
std::optional<std::string> WindowsServices::crypto_default_provider(std::uint32_t type,bool user){
    if(!registry_file_.empty() && !runtime_.network.external_io)return std::nullopt;
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);if(transaction.error)return std::nullopt;
    initialize_registry_views(runtime_.registry);
    auto path=user?registry_current_user_path():std::u16string(1,2);for(const auto& [folded,display]:default_provider_components(type,user)){(void)display;path+=u"\\";path+=folded;}
    const auto key=runtime_.registry.keys.find(registry_view_path(path,registry_view(0,x86_)));if(key==runtime_.registry.keys.end())return std::nullopt;
    const auto value=key->second.find(u"name");if(value==key->second.end() || value->second.type!=1 || value->second.bytes.size()%2)return std::nullopt;
    std::u16string text;for(std::size_t i=0;i+1<value->second.bytes.size();i+=2){const auto c=static_cast<char16_t>(value->second.bytes[i]|(value->second.bytes[i+1]<<8));if(!c)break;text+=c;}
    if(text.empty())return std::nullopt;return utf16_to_utf8(text);
}
// 寫入或刪除（name 為空）預設提供者：沿用登錄的安全性檢查（建立子機碼需父機碼的 KEY_CREATE_SUB_KEY，寫值需 KEY_SET_VALUE，刪除需 DELETE），
// 新機碼繼承父機碼的安全性描述元；在同一個登錄交易內提交，其他程序立即看到。
std::uint32_t WindowsServices::set_crypto_default_provider(std::uint32_t type,bool user,const std::optional<std::string>& name){
    if(!registry_file_.empty() && !runtime_.network.external_io)return 50;
    RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,false);if(transaction.error)return transaction.error;
    initialize_registry_views(runtime_.registry);
    auto& registry=runtime_.registry;const auto before=registry;const auto view=registry_view(0,x86_);
    try{
        auto logical=user?registry_current_user_path():std::u16string(1,2);std::vector<std::pair<std::u16string,std::u16string>> levels;
        for(const auto& [folded,display]:default_provider_components(type,user)){logical+=u"\\";logical+=folded;levels.emplace_back(registry_view_path(logical,view),display);}
        const auto& key=levels.back().first;const auto now=runtime_.system_filetime();
        if(!name){
            const auto found=registry.keys.find(key);if(found==registry.keys.end())return 0x80090017U;
            (void)registry_security_access(key,0x10000U);
            for(const auto& [entry,value]:found->second){(void)entry;registry.bytes-=value.bytes.size();--registry.values;}
            registry.keys.erase(found);registry.display_names.erase(key);registry.modified_times.erase(key);registry.security_descriptors.erase(key);registry.classes.erase(key);
            registry.modified_times[key.substr(0,key.find_last_of(u'\\'))]=now;
        }else{
            for(const auto& [path,display]:levels){
                if(registry.keys.contains(path))continue;
                const auto parent=path.substr(0,path.find_last_of(u'\\'));registry.keys.try_emplace(parent);(void)registry_security_access(parent,4U);
                auto descriptor=registry_creation_descriptor(registry_security_descriptor(parent),{});
                registry.keys.try_emplace(path);registry.display_names[path]=display;registry.modified_times[path]=now;registry.security_descriptors[path]=std::move(descriptor);
            }
            (void)registry_security_access(key,2U);
            auto& values=registry.keys[key];auto bytes=utf16_bytes(utf8_to_utf16(*name));
            if(const auto old=values.find(u"name");old!=values.end()){registry.bytes-=old->second.bytes.size();--registry.values;}
            registry.bytes+=bytes.size();++registry.values;values[u"name"]=RegistryValue{1,std::move(bytes),u"Name"};registry.modified_times[key]=now;
        }
        if(const auto error=commit_registry_transaction(transaction)){registry=before;return error;}
        return 0;
    }catch(const SecurityDescriptorError& error){registry=before;return error.code;}
    catch(...){registry=before;throw;}
}
std::uint32_t WindowsServices::legacy_crypto_provider(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    using Bytes=std::vector<std::uint8_t>;using namespace legacy_crypto;
    const auto fail=[&](std::uint32_t error){last_error_=error;return 0U;};
    const auto u32=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const auto text=[](std::string_view value){Bytes bytes(value.begin(),value.end());bytes.push_back(0);return bytes;};
    const auto put=[](Bytes& bytes,std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));};
    const auto word=[&](std::uint32_t value){Bytes bytes(4);put(bytes,0,value);return bytes;};
    const auto write=[&](const Bytes& bytes,std::uint64_t out,std::uint64_t length){
        if(!length)return fail(87);const auto capacity=memory.load(GuestAddress{length},32);memory.check(GuestAddress{length},4,Access::Write);
        if(out && capacity>=bytes.size())memory.check(GuestAddress{out},bytes.size(),Access::Write);
        memory.store(GuestAddress{length},32,bytes.size());if(!out)return 1U;if(capacity<bytes.size())return fail(234);memory.write(GuestAddress{out},bytes);return 1U;
    };
    try {
    if(symbol=="CryptAcquireContextA" || symbol=="CryptAcquireContextW"){
        if(!arg(0))return fail(87);
        const auto string=[&](unsigned n){return symbol.back()=='W'?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(n)},1024)):ansi_to_utf8(read_ansi(memory,arg(n),4096));};
        memory.check(GuestAddress{arg(0)},x86_?4:8,Access::Write);
        const auto handle=acquire_crypto_provider(arg(1)?string(1):std::string{},arg(2)?string(2):std::string{},u32(3),u32(4));
        if(!handle)return 0;memory.store(GuestAddress{arg(0)},x86_?32:64,handle==UINT64_MAX?0:handle);return 1;
    }
    if(symbol.starts_with("CryptGetDefaultProvider")){
        // (dwProvType, pdwReserved, dwFlags, pszProvName, pcbProvName)：CRYPT_MACHINE_DEFAULT 取電腦預設；CRYPT_USER_DEFAULT（或 0）先取使用者預設，
        // 沒有設定時與 CryptAcquireContext 相同地退回電腦預設，再退回內建目錄的第一個提供者。
        if(arg(1))return fail(87);const auto flags=u32(2);if((flags&~3U) || flags==3U)return fail(0x80090009);
        const auto type=u32(0);if(!legacy_crypto::provider(type))return fail(0x80090017);
        std::optional<std::string> name;if(flags!=1U)name=crypto_default_provider(type,true);if(!name)name=crypto_default_provider(type,false);
        if(!name || !legacy_crypto::provider(type,*name))name=legacy_crypto::provider(type)->name;
        return write(symbol.back()=='W'?utf16_bytes(utf8_to_utf16(*name)):text(*name),arg(3),arg(4));
    }
    if(symbol.starts_with("CryptSetProvider")){
        // CryptSetProvider(pszProvName, dwProvType) 即 CryptSetProviderEx(..., NULL, CRYPT_USER_DEFAULT)。
        // CryptSetProviderEx(pszProvName, dwProvType, pdwReserved, dwFlags)：CRYPT_MACHINE_DEFAULT／CRYPT_USER_DEFAULT 擇一，CRYPT_DELETE_DEFAULT 刪除設定。
        const bool extended=symbol.starts_with("CryptSetProviderEx");const auto type=u32(1);const auto flags=extended?u32(3):2U;
        if(extended && arg(2))return fail(87);if((flags&~7U) || !(flags&3U) || (flags&3U)==3U)return fail(0x80090009);
        if(!legacy_crypto::provider(type))return fail(0x80090014);
        std::optional<std::string> name;
        if(!(flags&4U)){
            if(!arg(0))return fail(87);name=symbol.back()=='W'?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)},1024)):ansi_to_utf8(read_ansi(memory,arg(0),4096));
            if(!legacy_crypto::provider(type,*name)){const bool known=std::any_of(std::begin(providers),std::end(providers),[&](const auto& p){return *name==p.name;});return fail(known?0x8009001bU:0x80090013U);}
        }
        const auto status=set_crypto_default_provider(type,(flags&2U)!=0,name);return status?fail(status):1U;
    }
    if(symbol.starts_with("CryptEnumProvider")){
        if(arg(1) || !arg(3) || !arg(5))return fail(87);if(u32(2))return fail(0x80090009);
        const bool types=symbol.starts_with("CryptEnumProviderTypes");std::vector<const Provider*> catalog;
        for(const auto& item:providers)if(!types || std::none_of(catalog.begin(),catalog.end(),[&](const auto* p){return p->type==item.type;}))catalog.push_back(&item);
        if(u32(0)>=catalog.size())return fail(259);
        const auto& provider=*catalog[u32(0)];const auto* name=types?provider.description:provider.name;
        const auto bytes=symbol.back()=='W'?utf16_bytes(utf8_to_utf16(name)):text(name);
        memory.check(GuestAddress{arg(3)},4,Access::Write);const auto result=write(bytes,arg(4),arg(5));memory.store(GuestAddress{arg(3)},32,provider.type);return result;
    }
    if(symbol=="CryptSetProvParam" && u32(1)==1){
        if(u32(3))return fail(0x80090009);if(!arg(2))return fail(87);
        const auto window=memory.load(GuestAddress{arg(2)},x86_?32:64);if(window && !gui_.windows.contains(window))return fail(1400);
        if(!arg(0)){runtime_.crypto_client_window=window;return 1;}
        const auto found=runtime_.crypt_providers.find(arg(0));if(found==runtime_.crypt_providers.end())return fail(6);found->second.client_window=window;return 1;
    }
    const auto found=runtime_.crypt_providers.find(arg(0));if(found==runtime_.crypt_providers.end())return fail(6);auto& provider=found->second;
    if((symbol=="CryptSetProvParam" || symbol=="CryptGetProvParam") && u32(1)==8){
        const bool setting=symbol=="CryptSetProvParam";const auto flags=u32(setting?3:4);
        if(!flags || (flags&~0xa000001fU) || ((flags&0xa0000000U)==0xa0000000U))return fail(87);
        if(flags&8U)return fail(1314);if(provider.flags&0xf0000000U)return fail(0x8009000a);
        if(setting && !arg(2))return fail(87);
        refresh_file_security_context();const auto& primary=runtime_.token_objects.at(1);
        const auto access=setting?((flags&0x13U)?0x80000U:0U)|((flags&0xa0000004U)?0x40000U:0U):0x20000U;
        const auto descriptor=setting?std::optional{security::read_descriptor(memory,arg(2),x86_,true)}:std::nullopt;
        if(descriptor && (flags&4U) && !(descriptor->control&4U))return fail(1338);
        const auto status=private_key_store_security(private_key_context(provider.user_scope),provider.type,provider.container,(provider.flags&32U)!=0,access,[&](const HostFile& file){
            if(setting){security::write_native_file_security(file,*descriptor,flags,primary.user.sid,primary.primary_group);return 0U;}
            const auto bytes=security::relative_descriptor(security::file_descriptor(security::read_native_file_security(file,true),flags));
            return write(bytes,arg(2),arg(3))?0U:last_error_;
        });return status?fail(status):1;
    }
    if(symbol=="CryptSetProvParam"){
        if(u32(3))return fail(0x80090009);
        // 本 CSP 為軟體提供者，硬體 RNG／智慧卡 PIN 等參數沒有對應裝置。
        if(u32(1)==38 && arg(2))return fail(87);
        return fail(0x8009000a);
    }
    const auto parameter=u32(1),flags=u32(4);
    if(parameter==1 || parameter==22){
        if(flags!=1 && flags!=2 && flags!=0)return fail(0x80090009);
        std::vector<Algorithm> catalog;for(const auto& a:algorithms)if(const auto entry=algorithm_for(a.id,provider))catalog.push_back(*entry);
        auto& cursor=parameter==1?provider.algorithm_cursor:provider.extended_cursor;const auto index=flags==1?0:cursor;
        if(index>=catalog.size())return fail(259);const auto& a=catalog[index];Bytes bytes(parameter==1?32:88);
        put(bytes,0,a.id);put(bytes,4,a.default_bits);
        if(parameter==1){put(bytes,8,static_cast<std::uint32_t>(std::strlen(a.name)+1));std::copy_n(a.name,std::strlen(a.name),bytes.begin()+12);}
        else {put(bytes,8,a.min_bits);put(bytes,12,a.max_bits);put(bytes,20,static_cast<std::uint32_t>(std::strlen(a.name)+1));std::copy_n(a.name,std::strlen(a.name),bytes.begin()+24);
            put(bytes,44,static_cast<std::uint32_t>(std::strlen(a.description)+1));std::copy_n(a.description,std::strlen(a.description),bytes.begin()+48);}
        const auto result=write(bytes,arg(2),arg(3));if(result && arg(2))cursor=index+1;else if(flags==1)cursor=0;return result;
    }
    if(parameter==2){
        if(flags!=1 && flags!=2 && flags!=0)return fail(0x80090009);
        if(flags==1){const auto status=private_key_store_list(private_key_context(provider.user_scope),provider.type,(provider.flags&32U)!=0,provider.containers);if(status)return fail(status);provider.container_cursor=0;}
        if(provider.containers.empty() && !arg(2) && flags==1)return write(Bytes(1),0,arg(3));
        if(provider.container_cursor>=provider.containers.size())return fail(259);const auto result=write(text(provider.containers[provider.container_cursor]),arg(2),arg(3));if(result && arg(2))++provider.container_cursor;return result;
    }
    if(flags)return fail(0x80090009);
    switch(parameter){
    case 3:return write(word(2),arg(2),arg(3)); // 實際的軟體後端。
    case 4:return write(text(provider.name),arg(2),arg(3));
    case 5:return write(word(0x200),arg(2),arg(3));
    case 6:case 36:return write(text(provider.container),arg(2),arg(3));
    case 16:return write(word(provider.type),arg(2),arg(3));
    case 19:case 20:return write(word(provider.session_bits),arg(2),arg(3));
    case 27:return write(word(provider.flags&32U),arg(2),arg(3));
    case 34:case 35:return write(word(16),arg(2),arg(3));
    case 39:return write(word(3),arg(2),arg(3));
    default:return fail(0x8009000a);
    }
    }catch(const SecurityDescriptorError& error){return fail(error.code);}
    catch(const std::bad_alloc&){return fail(8);}
}
}
