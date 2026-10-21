#include "x64arm/windows_services.hpp"
#include "x64arm/private_key_store.hpp"
#include "certificate_keys.hpp"
#include "legacy_rsa_generation.hpp"
#include "ncrypt_key_store.hpp"
#include <openssl/evp.h>
#include <openssl/err.h>
#include <set>
#include <cstdio>
namespace x64arm {
using namespace certificate_detail;
std::span<const WindowsServiceDescriptor> certificate_key_service_descriptors(){static constexpr WindowsServiceDescriptor table[]{{"advapi32.dll","CryptContextAddRef",3},{"crypt32.dll","CryptAcquireCertificatePrivateKey",6},{"crypt32.dll","CertCreateSelfSignCertificate",8}};return table;}
void WindowsServices::prune_certificate_properties(){
    auto& certs=runtime_.certificates;std::set<std::uint64_t> used;for(const auto& [pointer,context]:certs.contexts){(void)pointer;used.insert(context.property_identity);}
    for(const auto& [pointer,context]:certs.crl_contexts){(void)pointer;used.insert(context.property_identity);}
    for(const auto& [handle,store]:certs.stores){(void)handle;for(const auto& [identity,properties]:store.property_identities){(void)identity;used.insert(properties);}}
    for(const auto& [handle,store]:certs.stores){(void)handle;for(const auto& [identity,properties]:store.crl_property_identities){(void)identity;used.insert(properties);}}
    for(auto it=certs.properties.begin();it!=certs.properties.end();)if(!used.contains(it->first)){if(const auto key=it->second.find(5);key!=it->second.end()){const auto [provider,spec,owned]=key_context(key->second);if(owned && provider){if(spec==UINT32_MAX)(void)release_storage_crypto_key(runtime_,provider);else (void)release_crypto_provider(provider);}}it=certs.properties.erase(it);}else ++it;
}
std::uint32_t WindowsServices::certificate_key_property(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);auto& certs=runtime_.certificates;const auto ctx=certs.contexts.find(arg(0));if(ctx==certs.contexts.end())throw CertificateError{87};
    auto& props=certs.properties.at(ctx->second.property_identity);const auto id=static_cast<std::uint32_t>(arg(1));
    const auto set_context=[&](std::uint64_t handle,std::uint32_t spec,bool owned){
        if(spec!=0 && spec!=1 && spec!=2 && spec!=UINT32_MAX)throw CertificateError{87};if(handle && (spec==UINT32_MAX?!runtime_.storage_crypto_keys.contains(handle):!runtime_.crypt_providers.contains(handle)))throw CertificateError{6};
        auto value=key_context(handle,spec,owned);if(const auto old=props.find(5);old!=props.end()){const auto [prior,prior_spec,prior_owned]=key_context(old->second);if(prior!=handle && prior_owned && prior){if(prior_spec==UINT32_MAX)(void)release_storage_crypto_key(runtime_,prior);else (void)release_crypto_provider(prior);}}props.insert_or_assign(5,std::move(value));
    };
    if(symbol=="CertSetCertificateContextProperty"){
        const auto flags=static_cast<std::uint32_t>(arg(2));if(flags&~0xc0000001U)throw CertificateError{50};
        if(!arg(3)){if(id==2)props.erase(2);else {if(const auto old=props.find(5);old!=props.end()){const auto [provider,spec,owned]=key_context(old->second);if(owned && provider){if(spec==UINT32_MAX)(void)release_storage_crypto_key(runtime_,provider);else (void)release_crypto_provider(provider);}props.erase(old);}}return 1;}
        if(id==2){const auto info=read_key_info(memory,arg(3),x86_);
            props.insert_or_assign(2,key_info(info));return 1;
        }
        auto handle=UINT64_C(0);auto spec=0U;if(const auto old=props.find(5);old!=props.end())std::tie(handle,spec,std::ignore)=key_context(old->second);
        if(id==1)handle=arg(3);else if(id==6)spec=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));
        else {if(memory.load(GuestAddress{arg(3)},32)!=(x86_?12U:24U))throw CertificateError{87};handle=memory.load(GuestAddress{arg(3)+unit},bits);spec=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)+2*unit},32));}
        bool owned=(flags&1U)==0;if(id==6)if(const auto old=props.find(5);old!=props.end())owned=std::get<2>(key_context(old->second));set_context(handle,spec,owned);return 1;
    }
    if(!arg(3))throw CertificateError{87};const auto capacity=memory.load(GuestAddress{arg(3)},32);memory.check(GuestAddress{arg(3)},4,Access::Write);CertificatePacket packet(x86_);
    if(id==2){const auto found=props.find(2);if(found==props.end())throw CertificateError{0x80092004};const auto info=key_info(found->second);packet.reserve(x86_?28:48,unit);
        if(!info.container.empty())packet.point(0,packet.data(utf16_bytes(utf8_to_utf16(info.container))));if(!info.name.empty())packet.point(unit,packet.data(utf16_bytes(utf8_to_utf16(info.name))));
        packet.put(2*unit,info.type);packet.put(2*unit+4,info.flags);packet.put(2*unit+8,info.parameters.size());packet.put(x86_?24:40,info.spec);
        if(!info.parameters.empty()){const auto params=packet.reserve(info.parameters.size()*(x86_?16:24),unit);packet.point(x86_?20:32,params);for(std::size_t i=0;i<info.parameters.size();++i){const auto at=params+i*(x86_?16:24);const auto& param=info.parameters[i];packet.put(at,param.id);packet.put(at+2*unit,param.bytes.size());packet.put(at+2*unit+4,param.flags);if(!param.bytes.empty())packet.point(at+unit,packet.data(param.bytes));}}
    }else {const auto found=props.find(5);std::uint64_t handle=0;std::uint32_t spec=0;
        if(found!=props.end())std::tie(handle,spec,std::ignore)=key_context(found->second);
        else if(id==6 && props.contains(2))spec=key_info(props.at(2)).spec;
        else throw CertificateError{0x80092004};
        if(id==1){packet.reserve(unit,unit);packet.put(0,handle,unit);}else if(id==6){packet.reserve(4,4);packet.put(0,spec);}else {packet.reserve(x86_?12:24,unit);packet.put(0,x86_?12:24);packet.put(unit,handle,unit);packet.put(2*unit,spec);}}
    memory.store(GuestAddress{arg(3)},32,packet.bytes.size());if(arg(2)){if(capacity<packet.bytes.size())throw CertificateError{234};packet.relocate(arg(2));memory.write(GuestAddress{arg(2)},packet.bytes);}return 1;
}
bool WindowsServices::acquire_certificate_key(std::uint64_t context,std::uint32_t flags,std::uint64_t& provider,std::uint32_t& spec,bool& caller_free){
    auto& certs=runtime_.certificates;const auto found=certs.contexts.find(context);if(found==certs.contexts.end())throw CertificateError{87};provider=0;spec=0;caller_free=false;
    if(flags&~0x700cfU || ((flags&0x70000U) && ((flags&0x70000U)&((flags&0x70000U)-1))))throw CertificateError{87};
    auto& props=certs.properties.at(found->second.property_identity);
    if(const auto cached=props.find(5);cached!=props.end()){const auto [handle,key_spec,owned]=key_context(cached->second);(void)owned;if(handle && ((key_spec==UINT32_MAX && (flags&0x70000U) && runtime_.storage_crypto_keys.contains(handle)) || (key_spec!=UINT32_MAX && !(flags&0x40000U) && runtime_.crypt_providers.contains(handle) && runtime_.legacy_crypto_user_keys.contains({handle,key_spec})))){provider=handle;spec=key_spec;return true;}}
    const auto matches_der=[&](const std::vector<std::uint8_t>& der){const auto* p=der.data();std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key(d2i_AutoPrivateKey(nullptr,&p,static_cast<long>(der.size())),EVP_PKEY_free);const auto* c=found->second.der.data();std::unique_ptr<X509,decltype(&X509_free)> cert(d2i_X509(nullptr,&c,static_cast<long>(found->second.der.size())),X509_free);return key && cert && EVP_PKEY_eq(key.get(),X509_get0_pubkey(cert.get()))==1;};
    const auto matches=[&](std::uint64_t handle,std::uint32_t key_spec){const auto user=runtime_.legacy_crypto_user_keys.find({handle,key_spec});return user!=runtime_.legacy_crypto_user_keys.end() && matches_der(user->second.der);};
    KeyProviderInfo info;auto metadata=props.find(2);
    if(metadata==props.end()){
        if(flags&8U)throw CertificateError{0x8009000d};bool healed=false;
        for(const auto type:{1U,24U}){for(const bool machine:{false,true}){std::vector<std::string> names;const auto status=private_key_store_list(private_key_context(),type,machine,names);if(status)continue;
            for(const auto& name:names){const auto handle=acquire_crypto_provider(name,{},type,(machine?32U:0U)|64U);if(!handle)continue;
                for(const auto candidate:{1U,2U})if(matches(handle,candidate)){info.container=name;info.type=type;info.flags=machine?32U:0U;info.spec=candidate;healed=true;break;}(void)release_crypto_provider(handle);if(healed)break;}
            if(healed)break;}if(healed)break;}
        if(!healed)throw CertificateError{0x8009000d};metadata=props.emplace(2,key_info(info)).first;
    }else info=key_info(metadata->second);
    if(info.flags&~0x61U)throw CertificateError{0x80090009};
    if(info.spec==UINT32_MAX || !info.type){
        if(!(flags&0x70000U))throw CertificateError{50};
        if(!info.parameters.empty())throw CertificateError{50};
        if(!info.name.empty() && info.name!="Microsoft Software Key Storage Provider")throw CertificateError{0x80090013};
        if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生金鑰儲存與金鑰產生");
        StorageCryptoKey value;const auto status=load_storage_crypto_key(private_key_context(),info.container,(info.flags&32U)!=0,0,value);if(status)throw CertificateError{status};
        if(!value.private_key)throw CertificateError{0x8009000d};if((flags&4U) && !matches_der(value.der))throw CertificateError{0x80090015};
        if(runtime_.storage_crypto_keys.size()>=4096)throw CertificateError{8};const auto handle=runtime_.next_crypto_handle++;
        runtime_.storage_crypto_keys.emplace(handle,std::move(value));
        try {
            const bool caching=(flags&1U) || ((flags&2U) && (info.flags&1U));
            if(caching){auto data=key_context(handle,UINT32_MAX,true);if(const auto old=props.find(5);old!=props.end()){const auto [prior,prior_spec,owned]=key_context(old->second);if(owned && prior){if(prior_spec==UINT32_MAX)(void)release_storage_crypto_key(runtime_,prior);else (void)release_crypto_provider(prior);}}props.insert_or_assign(5,std::move(data));}
            provider=handle;spec=UINT32_MAX;caller_free=!caching;return true;
        }catch(...){(void)release_storage_crypto_key(runtime_,handle);throw;}
    }
    if(flags&0x40000U)throw CertificateError{50};if(info.spec!=1 && info.spec!=2)throw CertificateError{87};if(!info.parameters.empty())throw CertificateError{50};
    const auto handle=acquire_crypto_provider(info.container,info.name,info.type,(info.flags&32U)|((flags&64U)?64U:0U));if(!handle)throw CertificateError{last_error_};
    try {if(!runtime_.legacy_crypto_user_keys.contains({handle,info.spec}))throw CertificateError{0x8009000d};if((flags&4U) && !matches(handle,info.spec))throw CertificateError{0x80090015};
        const bool caching=(flags&1U) || ((flags&2U) && (info.flags&1U));
        if(caching){auto value=key_context(handle,info.spec,true);if(const auto old=props.find(5);old!=props.end()){const auto [prior,old_spec,owned]=key_context(old->second);if(owned && prior){if(old_spec==UINT32_MAX)(void)release_storage_crypto_key(runtime_,prior);else (void)release_crypto_provider(prior);}}props.insert_or_assign(5,std::move(value));}
        provider=handle;spec=info.spec;caller_free=!caching;return true;
    }catch(...){(void)release_crypto_provider(handle);throw;}
}
std::optional<std::uint32_t> WindowsServices::invoke_certificate_key(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    #include "certificate_selfsign.inc"
    if(service.symbol=="CryptContextAddRef")return invoke_crypto(state,memory,service);const WindowsCall arg(state,memory,service);std::uint32_t result=0;
    try {if(!arg(3) || !arg(4) || !arg(5))throw CertificateError{87};memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);memory.check(GuestAddress{arg(4)},4,Access::Write);memory.check(GuestAddress{arg(5)},4,Access::Write);memory.store(GuestAddress{arg(5)},32,0);
        const auto flags=static_cast<std::uint32_t>(arg(1));if(flags&128U){if((flags&64U) || !arg(2))throw CertificateError{87};const auto hwnd=memory.load(GuestAddress{arg(2)},x86_?32:64);if(hwnd && !gui_.windows.contains(hwnd))throw CertificateError{1400};}else if(arg(2))throw CertificateError{87};
        std::uint64_t provider=0;std::uint32_t spec=0;bool caller_free=false;acquire_certificate_key(arg(0),flags,provider,spec,caller_free);memory.store(GuestAddress{arg(3)},x86_?32:64,provider);memory.store(GuestAddress{arg(4)},32,spec);memory.store(GuestAddress{arg(5)},32,caller_free?1:0);result=1;
    }catch(const CertificateError& error){last_error_=error.code;}catch(const MemoryFault&){last_error_=87;}catch(const std::bad_alloc&){last_error_=8;}arg.return_to(state,result);return std::nullopt;
}
}
