#include "x64arm/windows_services.hpp"
#include "x64arm/private_key_store.hpp"
#include "ncrypt_key_store.hpp"
#include "cng_asymmetric.hpp"
#include "x64arm/unicode.hpp"
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pkcs12.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <algorithm>
#include <memory>
#include <cstdio>
#include <cstdlib>

namespace x64arm {
namespace {
using Bytes=std::vector<std::uint8_t>;
using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
using Context=std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)>;
using Big=std::unique_ptr<BIGNUM,decltype(&BN_clear_free)>;
constexpr std::uint32_t bad_handle=0x80090026;
constexpr std::uint32_t bad_state=0x8009000b;
constexpr std::uint32_t small=0x80090028,key_permission=0x80090010;
using namespace cng;
bool storage_supported(std::string_view name){return name=="RSA" || curve(name);}
Key decode(const StorageCryptoKey& value){return cng::decode(value.der,value.private_key);}
std::uint32_t storage_error(std::uint32_t code){return code==8?no_memory:code==87?invalid:code==5?key_permission:code;}
Bytes saved(const StorageCryptoKey& value){
    Bytes out{'M','W','N','K'};word(out,1);word(out,value.export_policy);word(out,value.usage);word(out,value.legacy_spec);word(out,value.private_key?1:0);
    const auto append=[&](std::span<const std::uint8_t> data){word(out,static_cast<std::uint32_t>(data.size()));out.insert(out.end(),data.begin(),data.end());};
    append({reinterpret_cast<const std::uint8_t*>(value.algorithm.data()),value.algorithm.size()});append(value.der);word(out,static_cast<std::uint32_t>(value.properties.size()));for(const auto& [name,data]:value.properties){append({reinterpret_cast<const std::uint8_t*>(name.data()),name.size()});append(data);}return out;
}
StorageCryptoKey restored(const Bytes& bytes){
    require(bytes.size()>=24 && std::equal(bytes.begin(),bytes.begin()+4,"MWNK") && word(bytes,4)==1,bad_data);StorageCryptoKey value;value.export_policy=word(bytes,8);value.usage=word(bytes,12);value.legacy_spec=word(bytes,16);const auto priv=word(bytes,20);require(priv<=1 && !(value.export_policy&~15U) && !(value.usage&~0xffffffU) && value.legacy_spec<=2,bad_data);value.private_key=priv!=0;std::size_t at=24;
    const auto take=[&](){const auto n=word(bytes,at);at+=4;require(at<=bytes.size() && n<=bytes.size()-at,bad_data);Bytes out(bytes.begin()+static_cast<std::ptrdiff_t>(at),bytes.begin()+static_cast<std::ptrdiff_t>(at+n));at+=n;return out;};
    const auto name=take();value.algorithm.assign(name.begin(),name.end());value.der=take();const auto count=word(bytes,at);at+=4;require(count<=64,bad_data);for(unsigned i=0;i<count;++i){const auto p=take();std::string property(p.begin(),p.end());auto data=take();require(property.size()<=1024 && data.size()<=16384 && value.properties.emplace(property,std::move(data)).second,bad_data);}require(at==bytes.size() && storage_supported(value.algorithm),bad_data);auto key=decode(value);require(algorithm(key.get())==value.algorithm || (curve(value.algorithm) && EVP_PKEY_is_a(key.get(),"EC")),bad_data);validate(key.get(),value.private_key);value.bits=static_cast<std::uint32_t>(EVP_PKEY_get_bits(key.get()));value.finalized=true;return value;
}
}
bool release_storage_crypto_key(RuntimeState& runtime,std::uint64_t handle){
    const auto key=runtime.storage_crypto_keys.find(handle);if(key==runtime.storage_crypto_keys.end())return false;
    OPENSSL_cleanse(key->second.der.data(),key->second.der.size());runtime.storage_crypto_keys.erase(key);return true;
}
std::uint32_t save_storage_crypto_key(const PrivateKeyStoreContext& store,const StorageCryptoKey& value,bool create){
    try{require(!value.name.empty());struct Serialized {Bytes bytes;~Serialized(){OPENSSL_cleanse(bytes.data(),bytes.size());}} data{saved(value)};
        auto code=private_key_store_write({store.files,value.user_scope.empty()?store.user_sid:value.user_scope},storage_crypto_type,value.name,value.machine,data.bytes,create);if(!create && code==0x80090016)code=private_key_store_write({store.files,value.user_scope.empty()?store.user_sid:value.user_scope},storage_crypto_type,value.name,value.machine,data.bytes,true);return storage_error(code);}
    catch(const Error& error){return error.code;}catch(const std::bad_alloc&){return no_memory;}
}
std::uint32_t load_storage_crypto_key(const PrivateKeyStoreContext& store,std::string_view name,bool machine,std::uint32_t legacy_spec,StorageCryptoKey& result){
    try {
        require(!name.empty() && legacy_spec<=2);
        Bytes bytes;const auto status=private_key_store_read(store,storage_crypto_type,std::string(name),machine,bytes);
        require(!status,storage_error(status));auto value=restored(bytes);value.user_scope=store.user_sid;
        value.name=name;value.machine=machine;value.export_policy&=~12U;
        if(legacy_spec)require(value.legacy_spec==legacy_spec,bad_data);
        result=std::move(value);return 0;
    }catch(const Error& error){return error.code;}catch(const std::bad_alloc&){return no_memory;}
}
std::uint32_t WindowsServices::storage_crypto_call(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    ERR_clear_error();
    try {
        const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto unit=x86_?4U:8U;
        const auto wide=[&](std::uint64_t p){require(p!=0);return utf16_to_utf8(read_utf16(memory,GuestAddress{p},1024));};
        const auto read=[&](std::uint64_t p,std::size_t n){require(n<=output_limit);require(p || !n);Bytes bytes(n);memory.read(GuestAddress{p},bytes);return bytes;};
        const auto output=[&](const Bytes& bytes,unsigned pointer,unsigned length,unsigned result){require(arg(result)!=0);memory.check(GuestAddress{arg(result)},4,Access::Write);if(arg(pointer) && u32(length)>=bytes.size())memory.check(GuestAddress{arg(pointer)},bytes.size(),Access::Write);memory.store(GuestAddress{arg(result)},32,bytes.size());if(!arg(pointer))return 0U;if(u32(length)<bytes.size())return small;memory.write(GuestAddress{arg(pointer)},bytes);return 0U;};
        const auto external=[&](){if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生金鑰儲存與金鑰產生");};
        const auto persist=[&](const StorageCryptoKey& value,bool create){if(value.name.empty())return;external();const auto status=save_storage_crypto_key(private_key_context(),value,create);require(!status,status);};
        const auto create=[&](StorageCryptoKey value,unsigned output_arg){require(arg(output_arg)!=0);memory.check(GuestAddress{arg(output_arg)},unit,Access::Write);require(runtime_.storage_crypto_keys.size()<4096,no_memory);const auto handle=runtime_.next_crypto_handle++;runtime_.storage_crypto_keys.emplace(handle,std::move(value));memory.store(GuestAddress{arg(output_arg)},bits,handle);return 0U;};
        auto key=runtime_.storage_crypto_keys.find(arg(0));const bool provider=runtime_.storage_crypto_providers.contains(arg(0));
        if(symbol=="NCryptIsKeyHandle")return key!=runtime_.storage_crypto_keys.end();
        if(symbol=="NCryptFreeObject"){if(key!=runtime_.storage_crypto_keys.end()){(void)release_storage_crypto_key(runtime_,arg(0));return 0;}require(runtime_.storage_crypto_providers.erase(arg(0))!=0,bad_handle);return 0;}
        if(symbol=="NCryptOpenStorageProvider"){
            require(!u32(2),bad_flags);require(arg(0)!=0);memory.check(GuestAddress{arg(0)},unit,Access::Write);if(arg(1))require(wide(arg(1))=="Microsoft Software Key Storage Provider",0x80090013);require(runtime_.storage_crypto_providers.size()<1024,no_memory);const auto handle=runtime_.next_crypto_handle++;runtime_.storage_crypto_providers.insert(handle);memory.store(GuestAddress{arg(0)},bits,handle);return 0;
        }
        if(symbol=="NCryptIsAlgSupported"){require(provider,bad_handle);require(!(u32(2)&~0x40U),bad_flags);require(storage_supported(wide(arg(1))),unsupported);return 0;}
        if(symbol=="NCryptCreatePersistedKey" || symbol=="NCryptOpenKey"){
            require(provider,bad_handle);require(arg(1)!=0);memory.check(GuestAddress{arg(1)},unit,Access::Write);const bool open=symbol=="NCryptOpenKey";const auto flags=u32(open?4:5);require(!(flags&~(open?0x60U:0xe0U)),bad_flags);require(u32(open?3:4)<=2);StorageCryptoKey value;value.user_scope=private_key_context().user_sid;value.machine=(flags&32)!=0;value.overwrite=(flags&128)!=0;value.legacy_spec=u32(open?3:4);
            if(open){value.name=wide(arg(2));require(!value.name.empty());external();StorageCryptoKey restored_key;const auto status=load_storage_crypto_key(private_key_context(),value.name,value.machine,value.legacy_spec,restored_key);require(!status,status);return create(std::move(restored_key),1);}
            value.algorithm=wide(arg(2));require(storage_supported(value.algorithm),unsupported);value.bits=value.algorithm=="RSA"?2048:curve(value.algorithm)->bits;if(arg(3)){value.name=wide(arg(3));require(!value.name.empty());external();Bytes bytes;const auto status=private_key_store_read(private_key_context(),storage_crypto_type,value.name,value.machine,bytes);if(!status)require(value.overwrite,0x8009000f);else require(status==0x80090016,storage_error(status));}return create(std::move(value),1);
        }
        if(symbol=="NCryptImportKey"){
            require(provider,bad_handle);require(!arg(1),unsupported);require(arg(4)!=0);memory.check(GuestAddress{arg(4)},unit,Access::Write);const auto flags=u32(7);require(!(flags&~0x4e0U),bad_flags);StorageCryptoKey value;value.user_scope=private_key_context().user_sid;value.machine=(flags&32)!=0;value.overwrite=(flags&128)!=0;std::string password;bool has_password=false;
            if(arg(3)){const auto version=memory.load(GuestAddress{arg(3)},32),count=memory.load(GuestAddress{arg(3)+4},32),buffers=memory.load(GuestAddress{arg(3)+8},bits);require(!version && count<=16 && (buffers || !count));std::set<std::uint32_t> seen;for(unsigned i=0;i<count;++i){const auto base=buffers+i*(x86_?12:16),size=memory.load(GuestAddress{base},32),type=memory.load(GuestAddress{base+4},32),p=memory.load(GuestAddress{base+8},bits);require(seen.insert(static_cast<std::uint32_t>(type)).second && size>=2 && size<=2048 && !(size&1));const auto bytes=read(p,size);require(bytes[size-1]==0 && bytes[size-2]==0);const auto text=utf16_to_utf8(read_utf16(memory,GuestAddress{p},size/2));if(type==45)value.name=text;else if(type==46){password=text;has_password=true;}else throw Error{unsupported};}}
            const auto type=wide(arg(2));const auto bytes=read(arg(5),u32(6));Key native(nullptr,EVP_PKEY_free);
            if(type=="PKCS8_PRIVATEKEY"){
                const auto* p=bytes.data();std::unique_ptr<PKCS8_PRIV_KEY_INFO,decltype(&PKCS8_PRIV_KEY_INFO_free)> info(nullptr,PKCS8_PRIV_KEY_INFO_free);
                if(has_password){std::unique_ptr<X509_SIG,decltype(&X509_SIG_free)> encrypted(d2i_X509_SIG(nullptr,&p,static_cast<long>(bytes.size())),X509_SIG_free);require(encrypted && p==bytes.data()+bytes.size(),bad_data);info.reset(PKCS8_decrypt(encrypted.get(),password.data(),static_cast<int>(password.size())));}
                else info.reset(d2i_PKCS8_PRIV_KEY_INFO(nullptr,&p,static_cast<long>(bytes.size())));
                require(info && p==bytes.data()+bytes.size(),bad_data);native.reset(EVP_PKCS82PKEY(info.get()));require(bool(native),bad_data);validate(native.get(),true);value.algorithm=algorithm(native.get());
            }else {require(!has_password,unsupported);native=from_blob(bytes,type,value.algorithm,value.private_key);}
            value.bits=static_cast<std::uint32_t>(EVP_PKEY_get_bits(native.get()));value.der=der(native.get(),value.private_key);value.finalized=!(flags&0x400);if(value.finalized && !value.name.empty())persist(value,!value.overwrite);return create(std::move(value),4);
        }
        require(key!=runtime_.storage_crypto_keys.end() || provider,bad_handle);
        if(symbol=="NCryptGetProperty" || symbol=="NCryptSetProperty"){
            const bool set=symbol=="NCryptSetProperty";const auto property=wide(arg(1));const auto flags=u32(set?4:5);require(!(flags&~0xc0000040U),bad_flags);Bytes bytes;
            if(set){require(key!=runtime_.storage_crypto_keys.end(),unsupported);auto value=key->second;const auto input=read(arg(2),u32(3));const bool persist_only=(flags&0x40000000)!=0;if(flags&0xc0000000U)require(!value.name.empty(),bad_state);
                if(!persist_only && (property=="Length" || property=="Export Policy" || property=="Key Usage")){require(input.size()==4);const auto n=word(input,0);if(property=="Length"){require(!value.finalized,bad_state);require(value.algorithm=="RSA" && n>=512 && n<=8192 && n%64==0);value.bits=n;}else if(property=="Export Policy"){require(!(n&~15U));require(!value.finalized || !(n&~value.export_policy),bad_state);value.export_policy=n;}else{require(!(n&~0xffffffU));require(!value.finalized || !(n&~value.usage),bad_state);value.usage=n;}}
                else {require(flags&0xc0000000U,unsupported);require(input.size()<=16384 && value.properties.size()<64);value.properties.insert_or_assign(property,input);}
                if(value.finalized && !value.name.empty() && (flags&0xc0000000U))persist(value,false);key->second=std::move(value);return 0;
            }
            if(flags&0x40000000){require(key!=runtime_.storage_crypto_keys.end(),unsupported);const auto p=key->second.properties.find(property);require(p!=key->second.properties.end(),0x80090011);bytes=p->second;}
            else if(provider){if(property=="Impl Type")word(bytes,2);else if(property=="Name")bytes=utf16_bytes(u"Microsoft Software Key Storage Provider");else if(property=="Max Name Length")word(bytes,1024);else throw Error{unsupported};}
            else {const auto& value=key->second;if(property=="Algorithm Name")bytes=utf16_bytes(utf8_to_utf16(value.algorithm));else if(property=="Algorithm Group")bytes=utf16_bytes(utf8_to_utf16(value.algorithm=="RSA"?"RSA":value.algorithm.starts_with("ECDH")?"ECDH":"ECDSA"));else if(property=="Name" || property=="Unique Name")bytes=utf16_bytes(utf8_to_utf16(value.name));else if(property=="Length")word(bytes,value.bits);else if(property=="Export Policy")word(bytes,value.export_policy);else if(property=="Key Usage")word(bytes,value.usage);else if(property=="Key Type")word(bytes,value.machine?32:0);else if(property=="Block Length" && value.algorithm=="RSA")word(bytes,(value.bits+7)/8);else if(property=="Lengths"){word(bytes,value.algorithm=="RSA"?512:value.bits);word(bytes,value.algorithm=="RSA"?8192:value.bits);word(bytes,value.algorithm=="RSA"?64:0);word(bytes,value.algorithm=="RSA"?2048:value.bits);}else{const auto p=value.properties.find(property);require(p!=value.properties.end(),unsupported);bytes=p->second;}}
            return output(bytes,2,3,4);
        }
        require(key!=runtime_.storage_crypto_keys.end(),bad_handle);auto& value=key->second;
        if(symbol=="NCryptDeleteKey"){require(!(u32(1)&~0x40U),bad_flags);if(!value.name.empty() && value.finalized){external();const auto status=private_key_store_delete(private_key_context(value.user_scope),storage_crypto_type,value.name,value.machine);require(!status,storage_error(status));}(void)release_storage_crypto_key(runtime_,arg(0));return 0;}
        if(symbol=="NCryptFinalizeKey"){
            require(!(u32(1)&~0x40U),bad_flags);require(!value.finalized,bad_state);auto next=value;
            if(next.der.empty()){external();auto generated=cng::generate(next.algorithm,next.bits);next.der=der(generated.get(),true);}
            next.finalized=true;persist(next,!next.overwrite);value=std::move(next);return 0;
        }
        require(value.finalized,bad_state);auto native=decode(value);
        if(symbol=="NCryptExportKey"){
            require(!(u32(7)&~0x40U),bad_flags);require(!arg(1) && !arg(3),unsupported);const auto type=wide(arg(2));const bool priv=type=="PKCS8_PRIVATEKEY" || type=="PRIVATEBLOB" || type=="ECCPRIVATEBLOB" || type=="RSAPRIVATEBLOB" || type=="RSAFULLPRIVATEBLOB";bool archive=false;if(priv){require(value.private_key,bad_state);require((value.export_policy&3)==3 || (!value.archived && (value.export_policy&12)==12),key_permission);archive=(value.export_policy&3)!=3;}
            Bytes bytes;if(type=="PKCS8_PRIVATEKEY"){std::unique_ptr<PKCS8_PRIV_KEY_INFO,decltype(&PKCS8_PRIV_KEY_INFO_free)> info(EVP_PKEY2PKCS8(native.get()),PKCS8_PRIV_KEY_INFO_free);require(bool(info),bad_data);const auto length=i2d_PKCS8_PRIV_KEY_INFO(info.get(),nullptr);require(length>0,bad_data);bytes.resize(static_cast<std::size_t>(length));auto* p=bytes.data();require(i2d_PKCS8_PRIV_KEY_INFO(info.get(),&p)==length,bad_data);}else bytes=blob(native.get(),value.algorithm,value.bits,type,priv);const auto status=output(bytes,4,5,6);if(!status && arg(4) && archive)value.archived=true;return status;
        }
        if(symbol=="NCryptSignHash" || symbol=="NCryptVerifySignature"){
            const bool sign=symbol=="NCryptSignHash";const auto flags=u32(sign?7:6);
            require(!(flags&~0x4aU),bad_flags);
            if(sign)require(value.private_key && (value.usage&2),key_permission);
            SignatureOptions options;options.padding=flags&10U;
            if(cng::rsa(value.algorithm)){
                require(arg(1)!=0);const auto pointer=memory.load(GuestAddress{arg(1)},bits);
                const auto name=pointer?wide(pointer):std::string{};
                options.digest=name.empty()?nullptr:EVP_get_digestbyname(name.c_str());require(name.empty() || options.digest);
                if(options.padding==8)options.salt=static_cast<unsigned>(memory.load(GuestAddress{arg(1)+unit},32));
            }else require(!arg(1) && value.algorithm.starts_with("ECDSA"),unsupported);
            const auto hash=read(arg(2),u32(3));
            if(sign){const bool size_only=!arg(4) || u32(5)<signature_size(native.get());return output(cng::sign(native.get(),hash,options,size_only),4,5,6);}
            const auto signature=read(arg(4),u32(5));cng::verify(native.get(),hash,signature,options);return 0;
        }
        if(symbol=="NCryptEncrypt" || symbol=="NCryptDecrypt"){
            const bool encrypt=symbol=="NCryptEncrypt";const auto flags=u32(7);require(!(flags&~0x47U),bad_flags);require(value.algorithm=="RSA",unsupported);if(!encrypt)require(value.private_key && (value.usage&1),key_permission);CipherOptions options;options.padding=flags&7;
            if(options.padding==4){require(arg(3)!=0);const auto name=wide(memory.load(GuestAddress{arg(3)},bits));options.digest=EVP_get_digestbyname(name.c_str());require(options.digest!=nullptr);const auto pointer=memory.load(GuestAddress{arg(3)+unit},bits),length=memory.load(GuestAddress{arg(3)+unit*2},32);options.label=read(pointer,length);}else require(!arg(3));
            const auto input=read(arg(1),u32(2));return output(cng::crypt(native.get(),input,options,encrypt,!arg(4)),4,5,6);
        }
        throw HostCallFault("未實作此金鑰儲存 API");
    }catch(const Error& error){if(const char* trace=std::getenv("X64ARM_TRACE_CRYPTO_ERRORS");trace && *trace && *trace!='0'){const auto native=ERR_peek_last_error();std::fprintf(stderr,"[crypto] %.*s status=0x%x native=%s\n",static_cast<int>(symbol.size()),symbol.data(),error.code,native?ERR_reason_error_string(native):"none");}return error.code;}catch(const MemoryFault&){return invalid;}catch(const std::invalid_argument&){return invalid;}catch(const std::bad_alloc&){return no_memory;}
}
}
