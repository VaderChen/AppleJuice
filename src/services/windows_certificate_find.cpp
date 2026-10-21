#include "x64arm/windows_services.hpp"
#include "certificate_keys.hpp"
#include "certificate_store.hpp"
#include <openssl/x509v3.h>
namespace x64arm {
using namespace certificate_detail;
std::span<const WindowsServiceDescriptor> certificate_find_service_descriptors(){static constexpr WindowsServiceDescriptor table[]{{"crypt32.dll","CertFindChainInStore",6}};return table;}
bool WindowsServices::free_certificate_chain(GuestState& state,GuestMemory& memory,std::uint64_t pointer){
    if(!pointer)return true;auto& certs=runtime_.certificates;const auto found=certs.chains.find(pointer);if(found==certs.chains.end())return false;if(--found->second.references)return true;
    auto contexts=std::move(found->second.contexts);certs.chains.erase(found);for(const auto context:contexts)(void)free_certificate(state,memory,context);(void)free_heap(memory,pointer,state);return true;
}
void WindowsServices::discard_certificate_find(GuestState& state,GuestMemory& memory,std::uint64_t handle){auto& finds=runtime_.certificates.finds;const auto found=finds.find(handle);if(found==finds.end())return;const auto context=found->second.pending_context;finds.erase(found);if(context)(void)free_certificate(state,memory,context);}
std::optional<std::uint32_t> WindowsServices::invoke_certificate_find(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);auto& certs=runtime_.certificates;std::uint64_t previous=0,operation=0;
    try {
        bool valid=true;if(arg(5)){const auto chain=certs.chains.find(arg(5));if(chain==certs.chains.end())valid=false;else {valid=chain->second.find_store==arg(0);previous=chain->second.find_identity;(void)free_certificate_chain(state,memory,arg(5));}}
        const auto store=certs.stores.find(arg(0));if(!valid || store==certs.stores.end() || !store->second.references || !arg(4))throw CertificateError{87};
        if(!previous)refresh_store(certs,arg(0),true,filesystem_);
        if((arg(1)&65535U)!=1 || arg(3)!=1 || (arg(2)&~UINT64_C(0xc00f)))throw CertificateError{87};const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);const auto pointer=arg(4);const auto size=memory.load(GuestAddress{pointer},32);
        if(size<(x86_?32U:56U) || size>4096)throw CertificateError{87};memory.check(GuestAddress{pointer},static_cast<std::size_t>(size),Access::Read);
        CertificateFind find;find.store=arg(0);find.identity=previous;find.flags=static_cast<std::uint32_t>(arg(2));const auto usage=memory.load(GuestAddress{pointer+unit},bits);if(usage)find.usage=read_ansi(memory,usage,1024);
        find.key_spec=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+2*unit},32));find.acquire_flags=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+2*unit+4},32));if(find.key_spec!=0 && find.key_spec!=1 && find.key_spec!=2 && find.key_spec!=UINT32_MAX)throw CertificateError{87};
        const auto count=memory.load(GuestAddress{pointer+2*unit+8},32),issuers=memory.load(GuestAddress{pointer+(x86_?20:32)},bits);if(count>64 || (count && !issuers))throw CertificateError{87};std::size_t total=0;
        for(std::uint64_t i=0;i<count;++i){const auto p=issuers+i*2*unit;const auto length=memory.load(GuestAddress{p},32),data=memory.load(GuestAddress{p+unit},bits);if(!length || length>output_limit-total || !data)throw CertificateError{87};total+=length;std::vector<std::uint8_t> der(static_cast<std::size_t>(length));memory.read(GuestAddress{data},der);const auto* at=der.data();std::unique_ptr<X509_NAME,decltype(&X509_NAME_free)> name(d2i_X509_NAME(nullptr,&at,static_cast<long>(length)),X509_NAME_free);if(!name || at!=der.data()+der.size())throw CertificateError{0x8009310b};find.issuers.push_back(std::move(der));}
        find.callback=memory.load(GuestAddress{pointer+(x86_?24:40)},bits);find.argument=memory.load(GuestAddress{pointer+(x86_?28:48)},bits);if(find.callback && !handles(GuestAddress{find.callback}))memory.check(GuestAddress{find.callback},1,Access::Execute);
        if(size>=(x86_?40U:72U)){find.chain_index=memory.load(GuestAddress{pointer+(x86_?32:56)},bits);find.element_index=memory.load(GuestAddress{pointer+(x86_?36:64)},bits);if(find.chain_index)memory.check(GuestAddress{find.chain_index},4,Access::Write);if(find.element_index)memory.check(GuestAddress{find.element_index},4,Access::Write);}
        if(certs.finds.size()>=1024)throw CertificateError{8};operation=runtime_.allocate_wait_handle();certs.finds.emplace(operation,std::move(find));return resume_certificate_find(state,memory,operation,1);
    }catch(const CertificateError& error){last_error_=error.code;}catch(const MemoryFault&){last_error_=87;}catch(const std::bad_alloc&){last_error_=8;}
    if(operation)discard_certificate_find(state,memory,operation);arg.return_to(state,0);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::resume_certificate_find(GuestState& state,GuestMemory& memory,std::uint64_t operation,std::uint64_t accepted){
    auto& certs=runtime_.certificates;const auto finish=[&](std::uint64_t value){discard_certificate_find(state,memory,operation);complete_call(state,memory,value);return std::optional<std::uint32_t>{};};
    std::uint64_t chain=0;
    try {
        const auto found=certs.finds.find(operation);if(found==certs.finds.end())throw CertificateError{87};auto& find=found->second;
        for(;;){
            if(find.pending_context){const auto context=find.pending_context;
                if(static_cast<std::uint32_t>(accepted) && certs.contexts.contains(context)){
                    const std::vector<std::string> usages=find.usage.empty()?std::vector<std::string>{}:std::vector<std::string>{find.usage};
                    chain=build_certificate_chain(state,memory,(find.flags&8U)?1:0,context,runtime_.system_filetime(),find.store,usages,false,(find.flags&4U)?4U:0U,(find.flags&0x8000U)!=0);
                    const auto& contexts=certs.chains.at(chain).contexts;bool issuer_match=find.issuers.empty();std::uint32_t element=0;
                    for(std::size_t i=0;!issuer_match && i<contexts.size();++i){const auto& der=certs.contexts.at(contexts[i]).der;const auto* at=der.data();std::unique_ptr<X509,decltype(&X509_free)> cert(d2i_X509(nullptr,&at,static_cast<long>(der.size())),X509_free);if(!cert)throw CertificateError{0x8009310b};const auto name=encoded(X509_get_issuer_name(cert.get()),i2d_X509_NAME);
                        if(std::find(find.issuers.begin(),find.issuers.end(),name)!=find.issuers.end()){issuer_match=true;element=static_cast<std::uint32_t>(i+1);}}
                    bool key_match=true;if(issuer_match && (find.flags&1U)){std::uint64_t provider=0;std::uint32_t spec=0;bool caller_free=false;try {acquire_certificate_key(context,find.acquire_flags|4U,provider,spec,caller_free);key_match=!find.key_spec || find.key_spec==spec;if(caller_free)(void)release_crypto_provider(provider);}catch(const CertificateError&){key_match=false;}}
                    if(issuer_match && key_match){auto& result=certs.chains.at(chain);result.find_store=find.store;result.find_identity=find.identity;if(!find.issuers.empty()){if(find.chain_index)memory.store(GuestAddress{find.chain_index},32,0);if(find.element_index)memory.store(GuestAddress{find.element_index},32,element);}return finish(chain);}
                    (void)free_certificate_chain(state,memory,chain);chain=0;
                }
                (void)free_certificate(state,memory,context);find.pending_context=0;
            }
            const auto store=certs.stores.find(find.store);if(store==certs.stores.end() || !store->second.references)throw CertificateError{87};bool after=!find.identity;
            bool candidate=false;for(const auto& item:store_entries(certs,find.store)){if(!after){if(item.identity==find.identity){after=true;continue;}if(!store->second.collection && item.identity>find.identity)after=true;else continue;}find.identity=item.identity;const auto& props=certs.properties.at(item.properties);if(!store->second.enum_archived && props.contains(19))continue;
                if(!(find.flags&0x4000U)){std::uint32_t spec=0;if(const auto info=props.find(2);info!=props.end())spec=key_info(info->second).spec;if(const auto cached=props.find(5);cached!=props.end()){const auto [handle,value,owned]=key_context(cached->second);(void)owned;if(handle && runtime_.crypt_providers.contains(handle))spec=value;}if(!spec || (find.key_spec && spec!=find.key_spec))continue;}
                if(!find.usage.empty()){const auto* at=item.der->data();std::unique_ptr<X509,decltype(&X509_free)> cert(d2i_X509(nullptr,&at,static_cast<long>(item.der->size())),X509_free);if(!cert)throw CertificateError{0x8009310b};int critical=-1;std::unique_ptr<EXTENDED_KEY_USAGE,decltype(&EXTENDED_KEY_USAGE_free)> eku(static_cast<EXTENDED_KEY_USAGE*>(X509_get_ext_d2i(cert.get(),NID_ext_key_usage,&critical,nullptr)),EXTENDED_KEY_USAGE_free);if(!eku && critical!=-1)throw CertificateError{0x8009310b};bool matches=!eku;for(int i=0;eku && i<sk_ASN1_OBJECT_num(eku.get());++i){const auto name=oid_name(sk_ASN1_OBJECT_value(eku.get(),i));if(name==find.usage || name=="2.5.29.37.0")matches=true;}if(!matches)continue;}
                find.pending_context=create_certificate(memory,*item.der,find.store,item.identity,item.store,item.source_identity);candidate=true;break;}
            if(!candidate)throw CertificateError{0x80092004};accepted=1;
            if(find.callback){try {start_callbacks(state,memory,{{find.callback,{find.pending_context,find.argument}}},Continuation::CertificateFind,operation);}catch(...){discard_certificate_find(state,memory,operation);throw;}return std::nullopt;}
        }
    }catch(const CertificateError& error){last_error_=error.code;}catch(const MemoryFault&){last_error_=87;}catch(const std::bad_alloc&){last_error_=8;}
    if(chain)(void)free_certificate_chain(state,memory,chain);return finish(0);
}
}
