#include "x64arm/windows_services.hpp"
#include "x64arm/private_key_store.hpp"
#include "legacy_crypto_catalog.hpp"
#include <openssl/evp.h>
namespace x64arm {
PrivateKeyStoreContext WindowsServices::private_key_context(std::string user_sid){initialize_tokens();return {filesystem_,user_sid.empty()?token_user_sid_string(effective_token_id()):std::move(user_sid)};}

namespace {
using Bytes=std::vector<std::uint8_t>;
using Keys=std::map<std::uint32_t,LegacyCryptKey>;
void word(Bytes& out,std::uint32_t value){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
Bytes encode(const Keys& keys){Bytes out{'M','W','K','S'};word(out,1);word(out,static_cast<std::uint32_t>(keys.size()));for(const auto& [spec,key]:keys){word(out,spec);word(out,key.algorithm);word(out,key.bits);word(out,key.exportable?1:0);word(out,key.permissions);word(out,static_cast<std::uint32_t>(key.der.size()));out.insert(out.end(),key.der.begin(),key.der.end());}return out;}
std::uint32_t decode(const Bytes& bytes,Keys& keys){
    if(bytes.size()<12 || !std::equal(bytes.begin(),bytes.begin()+4,"MWKS"))return 0x80090005;std::size_t at=4;
    const auto get=[&](){if(at>bytes.size() || bytes.size()-at<4)throw std::out_of_range("keyset");std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t(bytes[at++])<<(8*i);return value;};
    try {if(get()!=1)return 0x80090005;const auto count=get();if(count>2)return 0x80090005;
        for(unsigned i=0;i<count;++i){const auto spec=get();LegacyCryptKey key;key.algorithm=get();key.bits=get();const auto flags=get();key.exportable=flags!=0;key.permissions=get();const auto length=get();
            if((spec!=1 && spec!=2) || keys.contains(spec) || key.algorithm!=(spec==1?0xa400U:0x2400U) || flags>1 || key.permissions&~0x1fU || length>16384 || length>bytes.size()-at)return 0x80090005;
            key.der.assign(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+length));at+=length;const auto* cursor=key.der.data();
            std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> native(d2i_AutoPrivateKey(nullptr,&cursor,static_cast<long>(length)),EVP_PKEY_free);
            if(!native || cursor!=key.der.data()+key.der.size() || EVP_PKEY_get_base_id(native.get())!=EVP_PKEY_RSA || static_cast<unsigned>(EVP_PKEY_get_bits(native.get()))!=key.bits)return 0x80090005;
            key.private_key=true;keys.emplace(spec,std::move(key));}
        return at==bytes.size()?0U:0x80090005U;
    }catch(const std::out_of_range&){return 0x80090005;}
}
}
std::uint64_t WindowsServices::acquire_crypto_provider(std::string container,std::string name,std::uint32_t type,std::uint32_t flags){
    using namespace legacy_crypto;const auto fail=[&](std::uint32_t error){last_error_=error;return UINT64_C(0);};
    const auto catalog=std::find_if(std::begin(providers),std::end(providers),[&](const auto& p){return p.type==type;});if(catalog==std::end(providers))return fail(0x80090014);
    if(!name.empty() && name!=catalog->name)return fail(0x80090013);if(flags&~0xf0000078U || ((flags&24U)==24U))return fail(0x80090009);
    const auto verify=flags&0xf0000000U;if(verify && verify!=0xf0000000U)return fail(0x80090009);
    if(verify && (!container.empty() || (flags&24U)))return fail(0x80090016);
    if(container.size()>4096 || container.find('\0')!=container.npos)return fail(87);
    if(flags&16U){const auto status=private_key_store_delete(private_key_context(),type,container,(flags&32U)!=0);if(status)return fail(status);return UINT64_MAX;}
    if(runtime_.crypt_providers.size()>=1024)return fail(8);Keys keys;
    if(!verify){Bytes saved;auto status=flags&8U?private_key_store_write(private_key_context(),type,container,(flags&32U)!=0,encode({}),true):private_key_store_read(private_key_context(),type,container,(flags&32U)!=0,saved);
        if(status)return fail(status);if(!(flags&8U) && (status=decode(saved,keys)))return fail(status);}
    LegacyCryptProvider provider;provider.type=type;provider.name=catalog->name;provider.flags=flags;provider.container=container;provider.user_scope=private_key_context().user_sid;provider.client_window=runtime_.crypto_client_window;const auto handle=runtime_.next_crypto_handle++;
    try {runtime_.crypt_providers.emplace(handle,std::move(provider));for(auto& [spec,key]:keys){key.provider=handle;runtime_.legacy_crypto_user_keys.emplace(std::pair{handle,spec},std::move(key));}}
    catch(...){release_crypto_provider(handle);if(flags&8U)(void)private_key_store_delete(private_key_context(),type,container,(flags&32U)!=0);throw;}return handle;
}
bool WindowsServices::release_crypto_provider(std::uint64_t handle){
    const auto provider=runtime_.crypt_providers.find(handle);if(provider==runtime_.crypt_providers.end())return false;if(--provider->second.references)return true;
    runtime_.crypt_providers.erase(provider);std::erase_if(runtime_.legacy_crypto_hashes,[&](const auto& item){return item.second.provider==handle;});
    std::erase_if(runtime_.legacy_crypto_keys,[&](const auto& item){return item.second.provider==handle;});std::erase_if(runtime_.legacy_crypto_user_keys,[&](const auto& item){return item.first.first==handle;});return true;
}
bool WindowsServices::retain_crypto_provider(std::uint64_t handle){const auto found=runtime_.crypt_providers.find(handle);if(found==runtime_.crypt_providers.end()){last_error_=6;return false;}if(found->second.references==UINT32_MAX){last_error_=8;return false;}++found->second.references;return true;}
bool WindowsServices::store_crypto_user_key(std::uint64_t handle,std::uint32_t spec,const LegacyCryptKey& key){
    const auto& provider=runtime_.crypt_providers.at(handle);
    if(!(provider.flags&0xf0000000U)){const auto status=private_key_store_modify(private_key_context(provider.user_scope),provider.type,provider.container,(provider.flags&32U)!=0,[&](Bytes& saved){
        Keys keys;if(const auto error=decode(saved,keys))return error;keys.insert_or_assign(spec,key);auto updated=encode(keys);OPENSSL_cleanse(saved.data(),saved.size());saved=std::move(updated);return 0U;
    });if(status){last_error_=status;return false;}}
    runtime_.legacy_crypto_user_keys.insert_or_assign({handle,spec},key);return true;
}
}
