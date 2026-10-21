#include "x64arm/windows_services.hpp"
#include "legacy_crypto_catalog.hpp"
#include "legacy_rsa_generation.hpp"
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/core_names.h>
#include <algorithm>
#include <bit>
#include <memory>
namespace x64arm {
std::uint32_t WindowsServices::legacy_crypto_cipher(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    using Bytes=std::vector<std::uint8_t>;using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;using Context=std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)>;
    using namespace legacy_crypto;ERR_clear_error();
    constexpr std::uint32_t bad_key=0x80090003U,bad_data=0x80090005U,bad_algorithm=0x80090008U,bad_flags=0x80090009U,bad_type=0x8009000aU,bad_state=0x8009000bU;
    const auto fail=[&](std::uint32_t error){last_error_=error;return 0U;};
    const auto u32=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const auto read=[&](std::uint64_t address,std::size_t size){Bytes bytes(size);memory.read(GuestAddress{address},bytes);return bytes;};
    const auto blob=[&](std::uint64_t address,std::size_t limit)->std::optional<Bytes>{
        const auto count=memory.load(GuestAddress{address},32),pointer=memory.load(GuestAddress{address+(x86_?4:8)},x86_?32:64);
        if(count>limit || (count && !pointer))return {};return read(pointer,static_cast<std::size_t>(count));
    };
    if(symbol=="CryptGenKey"){
        const auto provider=runtime_.crypt_providers.find(arg(0));if(provider==runtime_.crypt_providers.end())return fail(6);
        const auto flags=u32(2)&0xffffU;const auto id=u32(1)==1?0xa400U:u32(1)==2?0x2400U:u32(1);
        const auto* a=find(id);if(!a || (!a->cipher && !rsa(id)) || !supported(*a,provider->second.type))return fail(bad_algorithm);
        if(flags&~0x15U)return fail(bad_flags);if(!arg(3))return fail(87);if(runtime_.legacy_crypto_keys.size()>=4096)return fail(8);
        const auto bits=(u32(2)>>16)?u32(2)>>16:a->default_bits;
        if(bits<a->min_bits || bits>a->max_bits || bits%(rsa(id)?16U:(id==0x6601 || id==0x6603 || id==0x6609)?1U:8U))return fail(bad_data);
        if((flags&0x14) && (id!=0x6602 && id!=0x6801))return fail(bad_flags);
        memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);LegacyCryptKey key;key.provider=arg(0);key.algorithm=id;key.bits=bits;key.exportable=bool(flags&1);if(key.exportable)key.permissions|=4;
        if(rsa(id)){
            std::uint32_t generation_error=0;const auto der=generate_rsa(runtime_,bits,[&](std::span<std::uint8_t> bytes){return random_bytes(bytes);},&generation_error);if(!der)return fail(generation_error);key.der=*der;
            key.private_key=true;
            if(!store_crypto_user_key(arg(0),id==0x2400?2U:1U,key))return 0;
        }else {
            key.secret.resize((id==0x6601 || id==0x6603 || id==0x6609)?bits/7:bits/8);if(!random_bytes(key.secret))return fail(8);
            if(id==0x6601 || id==0x6603 || id==0x6609)for(auto& value:key.secret){value=static_cast<std::uint8_t>(value&0xfe);value=static_cast<std::uint8_t>(value | ((std::popcount(value)&1)?0:1));}
            key.iv.resize(a->block>1?a->block:0);
            if((id==0x6602 || id==0x6801) && bits==40 && !(flags&0x10)){key.salt.resize(11);if(flags&4 && !random_bytes(key.salt))return fail(8);}
            else if(flags&4)return fail(bad_flags);provider->second.session_bits=bits;
        }
        const auto handle=runtime_.next_crypto_handle++;runtime_.legacy_crypto_keys.emplace(handle,std::move(key));memory.store(GuestAddress{arg(3)},x86_?32:64,handle);return 1;
    }
    const auto stored=runtime_.legacy_crypto_keys.find(arg(0));if(stored==runtime_.legacy_crypto_keys.end())return fail(bad_key);auto& key=stored->second;const auto* a=find(key.algorithm);if(!a)return fail(bad_algorithm);
    if(symbol=="CryptSetKeyParam"){
        if(u32(3))return fail(bad_flags);if(!arg(2))return fail(87);if(!(key.permissions&16) || key.cipher)return fail(bad_state);
        if(u32(1)==6){const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));if(value&~0x1fU || ((value&4) && !key.exportable))return fail(bad_data);key.permissions=value;return 1;}
        if(u32(1)==36){if(!rsa(key.algorithm))return fail(bad_type);const auto value=blob(arg(2),output_limit);if(!value)return fail(bad_data);key.oaep_label=*value;return 1;}
        if(key.secret.empty())return fail(bad_type);
        switch(u32(1)){
        case 1:if(a->block==1)return fail(bad_type);key.iv=read(arg(2),a->block);return 1;
        case 2:if(key.algorithm!=0x6602 && key.algorithm!=0x6801)return fail(bad_type);key.salt=read(arg(2),key.salt.size());return 1;
        case 10:{if(key.algorithm!=0x6602 && key.algorithm!=0x6801)return fail(bad_type);const auto value=blob(arg(2),16-key.secret.size());if(!value)return fail(bad_data);key.salt=*value;return 1;}
        case 3:case 4:case 5:case 19:{
            const auto value=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));
            if(u32(1)==19){if(key.algorithm!=0x6602)return fail(bad_type);if(!value || value>1024)return fail(bad_data);key.effective_bits=value;return 1;}
            if(a->block==1)return fail(bad_type);
            if(u32(1)==3){if(value!=1)return fail(bad_data);key.padding=value;}
            else if(u32(1)==4){if(value!=1 && value!=2)return fail(bad_data);key.mode=value;}
            else {if(value!=8)return fail(bad_data);key.mode_bits=value;}return 1;
        }
        default:return fail(bad_type);
        }
    }
    const bool encrypt=symbol=="CryptEncrypt",final=u32(2)!=0;const auto flags=u32(3);
    if(!(key.permissions&(encrypt?1U:2U)))return fail(bad_state);
    if(!arg(5) || (!encrypt && !arg(4)))return fail(87);const auto count=memory.load(GuestAddress{arg(5)},32);if(count>output_limit)return fail(8);
    memory.check(GuestAddress{arg(5)},4,Access::Write);
    auto hash=runtime_.legacy_crypto_hashes.end();if(arg(1)){hash=runtime_.legacy_crypto_hashes.find(arg(1));if(hash==runtime_.legacy_crypto_hashes.end())return fail(0x80090002);if(hash->second.finished)return fail(0x8009000c);}
    std::size_t required=static_cast<std::size_t>(count);
    if(rsa(key.algorithm)){
        if(key.algorithm!=0xa400)return fail(bad_algorithm);if(!final)return fail(bad_data);
        if(flags&~(encrypt?0x40U:0x60U) || (flags&0x60U)==0x60U)return fail(bad_flags);
        const auto width=key.bits/8;if(encrypt){if(count>width-((flags&0x40)?42U:11U))return fail(bad_data);required=width;}
        else {if(!key.private_key)return fail(bad_key);if(count!=width)return fail(bad_data);}
    }else {
        if(flags)return fail(bad_flags);if(!a->cipher || key.secret.empty())return fail(bad_algorithm);
        if(a->block>1){if((!encrypt || !final) && count%a->block)return fail(bad_data);if(!encrypt && final && !count)return fail(bad_data);if(encrypt && final)required+=a->block-static_cast<std::size_t>(count)%a->block;}
    }
    if(encrypt && !arg(4)){memory.store(GuestAddress{arg(5)},32,required);return 1;}
    if(encrypt && u32(6)<required){memory.store(GuestAddress{arg(5)},32,required);return fail(234);}
    if(required>output_limit)return fail(8);memory.check(GuestAddress{arg(4)},encrypt?required:static_cast<std::size_t>(count),Access::Write);
    const auto input=read(arg(4),static_cast<std::size_t>(count));Bytes output;auto next=key;
    if(rsa(key.algorithm)){
        const auto* at=key.der.data();Key native(key.private_key?d2i_AutoPrivateKey(nullptr,&at,static_cast<long>(key.der.size())):d2i_PUBKEY(nullptr,&at,static_cast<long>(key.der.size())),EVP_PKEY_free);
        Context context(native?EVP_PKEY_CTX_new(native.get(),nullptr):nullptr,EVP_PKEY_CTX_free);
        if(!context || (encrypt?EVP_PKEY_encrypt_init(context.get()):EVP_PKEY_decrypt_init(context.get()))<=0 || EVP_PKEY_CTX_set_rsa_padding(context.get(),(flags&0x40)?RSA_PKCS1_OAEP_PADDING:(flags&0x20)?RSA_NO_PADDING:RSA_PKCS1_PADDING)<=0)return fail(bad_key);
        if(!encrypt && !flags){unsigned rejection=0;OSSL_PARAM params[]{OSSL_PARAM_construct_uint(OSSL_ASYM_CIPHER_PARAM_IMPLICIT_REJECTION,&rejection),OSSL_PARAM_construct_end()};if(EVP_PKEY_CTX_set_params(context.get(),params)<=0)return fail(bad_key);}
        if(flags&0x40){
            if(EVP_PKEY_CTX_set_rsa_oaep_md(context.get(),EVP_sha1())<=0 || EVP_PKEY_CTX_set_rsa_mgf1_md(context.get(),EVP_sha1())<=0)return fail(bad_algorithm);
            if(!key.oaep_label.empty()){auto* label=static_cast<unsigned char*>(OPENSSL_memdup(key.oaep_label.data(),key.oaep_label.size()));if(!label)return fail(8);if(EVP_PKEY_CTX_set0_rsa_oaep_label(context.get(),label,static_cast<int>(key.oaep_label.size()))<=0){OPENSSL_free(label);return fail(bad_data);}}
        }
        auto bytes=input;if(!encrypt)std::reverse(bytes.begin(),bytes.end());output.resize(key.bits/8);auto size=output.size();
        if(encrypt){
            CryptoHash request("SHA256",false,{});request.update(key.der);
            const std::array<std::uint8_t,8> parameters{static_cast<std::uint8_t>(flags),0,0,0,static_cast<std::uint8_t>(key.oaep_label.size()),static_cast<std::uint8_t>(key.oaep_label.size()>>8),static_cast<std::uint8_t>(key.oaep_label.size()>>16),static_cast<std::uint8_t>(key.oaep_label.size()>>24)};
            request.update(parameters);request.update(key.oaep_label);request.update(bytes);const auto digest=request.digest();
            auto& pool=*runtime_.native_crypto_entropy;const auto entropy_key=std::pair{runtime_.entropy_index,std::string(digest.begin(),digest.end())};const bool comparing=runtime_.entropy.use_count()>1;const auto cached=comparing?pool.find(entropy_key):pool.end();
            if(comparing && cached==pool.end() && pool.size()>=4096)return fail(8);std::array<std::uint8_t,4> reservation{};if(!random_bytes(reservation))return fail(8);
            if(cached!=pool.end())output=cached->second;
            else {if(EVP_PKEY_encrypt(context.get(),output.data(),&size,bytes.data(),bytes.size())<=0)return fail(bad_data);output.resize(size);std::reverse(output.begin(),output.end());if(comparing)pool.emplace(entropy_key,output);}
        }else {if(EVP_PKEY_decrypt(context.get(),output.data(),&size,bytes.data(),bytes.size())<=0)return fail(bad_data);output.resize(size);}
    }else {
        if(next.cipher && next.encrypting!=encrypt)return fail(bad_state);
        try {
            if(!next.cipher){auto secret=key.secret;secret.insert(secret.end(),key.salt.begin(),key.salt.end());next.cipher=CryptoCipher(cipher_name(*a,key.mode),secret,key.mode==2?Bytes{}:key.iv,key.effective_bits,encrypt);next.encrypting=encrypt;}
            auto bytes=input;if(encrypt && final && a->block>1)bytes.resize(required,static_cast<std::uint8_t>(required-input.size()));output=next.cipher.update(bytes);
        }catch(const std::bad_alloc&){return fail(8);}catch(const std::exception&){return fail(bad_data);}
        if(!encrypt && final && a->block>1){const auto padding=output.back();if(!padding || padding>a->block || padding>output.size() || !std::all_of(output.end()-padding,output.end(),[&](auto value){return value==padding;}))return fail(bad_data);output.resize(output.size()-padding);}
        if(final)next.cipher={};
    }
    auto next_hash=hash==runtime_.legacy_crypto_hashes.end()?LegacyCryptHash{}:hash->second;
    if(hash!=runtime_.legacy_crypto_hashes.end()){const auto& plaintext=encrypt?input:output;next_hash.hash.update(plaintext);if(next_hash.algorithm==0x8008)next_hash.tail.update(plaintext);}
    memory.write(GuestAddress{arg(4)},output);memory.store(GuestAddress{arg(5)},32,output.size());key=std::move(next);if(hash!=runtime_.legacy_crypto_hashes.end())hash->second=std::move(next_hash);return 1;
}
}
