#include "x64arm/windows_services.hpp"
#include "legacy_crypto_catalog.hpp"
#include "crypto_digest.hpp"
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <algorithm>
#include <memory>

namespace x64arm {
namespace {
using Bytes=std::vector<std::uint8_t>;
using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
using Context=std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)>;
using Big=std::unique_ptr<BIGNUM,decltype(&BN_clear_free)>;
constexpr std::uint32_t bad_hash=0x80090002U,bad_key=0x80090003U,bad_data=0x80090005U,bad_signature=0x80090006U;
constexpr std::uint32_t bad_algorithm=0x80090008U,bad_flags=0x80090009U,bad_type=0x8009000aU,bad_key_state=0x8009000bU,bad_hash_state=0x8009000cU,no_key=0x8009000dU;
using crypto_detail::hash_info;
Bytes hash_value(const LegacyCryptHash& hash){
    if(hash.finished)return hash.value;
    auto result=hash.hash.digest();if(hash.algorithm==0x8008){const auto tail=hash.tail.digest();result.insert(result.end(),tail.begin(),tail.end());}return result;
}
std::uint32_t word(const Bytes& data,std::size_t at){if(at>data.size() || data.size()-at<4)return 0;std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t{data[at+i]}<<(8*i);return value;}
Bytes word(std::uint32_t value){return {static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value>>8),static_cast<std::uint8_t>(value>>16),static_cast<std::uint8_t>(value>>24)};}
Key decode_key(const LegacyCryptKey& key){
    const auto* data=key.der.data();return Key(key.private_key?d2i_AutoPrivateKey(nullptr,&data,static_cast<long>(key.der.size())):d2i_PUBKEY(nullptr,&data,static_cast<long>(key.der.size())),EVP_PKEY_free);
}
std::optional<LegacyCryptKey> import_rsa(const Bytes& bytes){
    if(bytes.size()<20 || (bytes[0]!=6 && bytes[0]!=7) || bytes[1]!=2 || bytes[2] || bytes[3])return {};
    const bool priv=bytes[0]==7;const auto alg=word(bytes,4),bits=word(bytes,12),exponent=word(bytes,16);
    if((alg!=0x2400 && alg!=0xa400) || bits<512 || bits>8192 || bits%16 || exponent<3 || !(exponent&1) || word(bytes,8)!=(priv?0x32415352U:0x31415352U))return {};
    const auto width=bits/8;if(bytes.size()!=20+(priv?width*9/2:width))return {};
    std::unique_ptr<OSSL_PARAM_BLD,decltype(&OSSL_PARAM_BLD_free)> builder(OSSL_PARAM_BLD_new(),OSSL_PARAM_BLD_free);
    if(!builder)return {};
    std::vector<Big> values;std::size_t at=20;
    const auto add=[&](const char* name,unsigned size){
        values.emplace_back(BN_lebin2bn(bytes.data()+at,static_cast<int>(size),nullptr),BN_clear_free);at+=size;
        return values.back() && !BN_is_zero(values.back().get()) && OSSL_PARAM_BLD_push_BN(builder.get(),name,values.back().get());
    };
    if(!add(OSSL_PKEY_PARAM_RSA_N,width) || BN_num_bits(values.front().get())!=static_cast<int>(bits) || !BN_is_odd(values.front().get()) || !OSSL_PARAM_BLD_push_uint(builder.get(),OSSL_PKEY_PARAM_RSA_E,exponent))return {};
    if(priv && (!add(OSSL_PKEY_PARAM_RSA_FACTOR1,width/2) || !add(OSSL_PKEY_PARAM_RSA_FACTOR2,width/2) || !add(OSSL_PKEY_PARAM_RSA_EXPONENT1,width/2) || !add(OSSL_PKEY_PARAM_RSA_EXPONENT2,width/2) || !add(OSSL_PKEY_PARAM_RSA_COEFFICIENT1,width/2) || !add(OSSL_PKEY_PARAM_RSA_D,width)))return {};
    std::unique_ptr<OSSL_PARAM,decltype(&OSSL_PARAM_free)> params(OSSL_PARAM_BLD_to_param(builder.get()),OSSL_PARAM_free);
    Context context(EVP_PKEY_CTX_new_from_name(nullptr,"RSA",nullptr),EVP_PKEY_CTX_free);EVP_PKEY* raw=nullptr;
    if(!params || !context || EVP_PKEY_fromdata_init(context.get())<=0 || EVP_PKEY_fromdata(context.get(),&raw,priv?EVP_PKEY_KEYPAIR:EVP_PKEY_PUBLIC_KEY,params.get())<=0){EVP_PKEY_free(raw);return {};}
    Key key(raw,EVP_PKEY_free);Context check(EVP_PKEY_CTX_new(key.get(),nullptr),EVP_PKEY_CTX_free);
    if(!check || (priv?EVP_PKEY_pairwise_check(check.get()):EVP_PKEY_public_check(check.get()))<=0)return {};
    const auto size=priv?i2d_PrivateKey(key.get(),nullptr):i2d_PUBKEY(key.get(),nullptr);if(size<=0)return {};
    LegacyCryptKey result;result.algorithm=alg;result.bits=bits;result.private_key=priv;result.der.resize(static_cast<std::size_t>(size));
    auto* output=result.der.data();if((priv?i2d_PrivateKey(key.get(),&output):i2d_PUBKEY(key.get(),&output))!=size)return {};return result;
}
Bytes export_rsa(const LegacyCryptKey& stored,bool priv){
    auto key=decode_key(stored);if(!key)return {};
    Bytes output{static_cast<std::uint8_t>(priv?7:6),2,0,0};
    const auto put=[&](std::uint32_t value){const auto bytes=word(value);output.insert(output.end(),bytes.begin(),bytes.end());};
    put(stored.algorithm);put(priv?0x32415352U:0x31415352U);put(stored.bits);
    BIGNUM* raw=nullptr;if(!EVP_PKEY_get_bn_param(key.get(),OSSL_PKEY_PARAM_RSA_E,&raw))return {};Big e(raw,BN_clear_free);
    if(BN_num_bits(e.get())>32)return {};put(static_cast<std::uint32_t>(BN_get_word(e.get())));
    const auto append=[&](const char* name,unsigned size){
        BIGNUM* value=nullptr;if(!EVP_PKEY_get_bn_param(key.get(),name,&value))return false;Big number(value,BN_clear_free);
        const auto at=output.size();output.resize(at+size);return BN_bn2lebinpad(number.get(),output.data()+at,static_cast<int>(size))==static_cast<int>(size);
    };
    const auto width=stored.bits/8;
    if(!append(OSSL_PKEY_PARAM_RSA_N,width))return {};
    if(priv && (!append(OSSL_PKEY_PARAM_RSA_FACTOR1,width/2) || !append(OSSL_PKEY_PARAM_RSA_FACTOR2,width/2) || !append(OSSL_PKEY_PARAM_RSA_EXPONENT1,width/2) || !append(OSSL_PKEY_PARAM_RSA_EXPONENT2,width/2) || !append(OSSL_PKEY_PARAM_RSA_COEFFICIENT1,width/2) || !append(OSSL_PKEY_PARAM_RSA_D,width)))return {};
    return output;
}
}
std::uint32_t WindowsServices::legacy_crypto_call(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    ERR_clear_error();
    const auto fail=[&](std::uint32_t error){last_error_=error;return 0U;};
    const auto u32=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const auto read=[&](std::uint64_t pointer,std::size_t size){Bytes bytes(size);memory.read(GuestAddress{pointer},bytes);return bytes;};
    const auto write=[&](const Bytes& data,std::uint64_t output,std::uint64_t length){
        if(!length)return fail(87);
        const auto capacity=memory.load(GuestAddress{length},32);memory.check(GuestAddress{length},4,Access::Write);
        if(output && capacity>=data.size())memory.check(GuestAddress{output},data.size(),Access::Write);
        memory.store(GuestAddress{length},32,data.size());if(!output)return 1U;if(capacity<data.size())return fail(234);
        memory.write(GuestAddress{output},data);return 1U;
    };
    const auto new_key=[&](LegacyCryptKey key,std::uint64_t output){
        if(!output)return fail(87);if(runtime_.legacy_crypto_keys.size()>=4096)return fail(8);
        memory.check(GuestAddress{output},x86_?4:8,Access::Write);const auto handle=runtime_.next_crypto_handle++;
        runtime_.legacy_crypto_keys.emplace(handle,std::move(key));memory.store(GuestAddress{output},x86_?32:64,handle);return 1U;
    };
    if(symbol=="CryptImportKey"){
        const auto provider=runtime_.crypt_providers.find(arg(0));if(provider==runtime_.crypt_providers.end())return fail(6);
        if(arg(3))return fail(bad_key);if(u32(4)&~0x11U)return fail(bad_flags);
        if(!arg(1) || !arg(5))return fail(87);if(u32(2)>16384)return fail(bad_data);
        memory.check(GuestAddress{arg(5)},x86_?4:8,Access::Write);
        const auto blob=read(arg(1),u32(2));auto key=import_rsa(blob);
        if(!key && blob.size()>=12 && blob[0]==8 && blob[1]==2 && !blob[2] && !blob[3]){
            const auto* algorithm=legacy_crypto::find(word(blob,4));const auto size=word(blob,8);
            if(!algorithm || !algorithm->cipher || !legacy_crypto::supported(*algorithm,provider->second.type))return fail(bad_algorithm);
            const auto bits=legacy_crypto::key_bits(*algorithm,size);
            if(size!=blob.size()-12 || bits<algorithm->min_bits || bits>algorithm->max_bits)return fail(bad_data);
            key.emplace();key->algorithm=algorithm->id;key->bits=bits;key->secret.assign(blob.begin()+12,blob.end());key->iv.resize(algorithm->block>1?algorithm->block:0);
            if((key->algorithm==0x6602 || key->algorithm==0x6801) && bits==40 && !(u32(4)&0x10))key->salt.resize(11);
        }
        if(!key)return fail(bad_data);
        if(u32(4)&0x10 && (key->bits!=40 || (key->algorithm!=0x6602 && key->algorithm!=0x6801)))return fail(bad_flags);
        key->provider=arg(0);key->exportable=(u32(4)&1)!=0;
        if(key->exportable)key->permissions|=4;
        if(runtime_.legacy_crypto_keys.size()>=4096)return fail(8);
        if(key->private_key && !store_crypto_user_key(arg(0),key->algorithm==0x2400?2U:1U,*key))return 0;
        if(!new_key(*key,arg(5)))return 0;
        if(!key->secret.empty())provider->second.session_bits=key->bits;
        return 1;
    }
    if(symbol=="CryptGetUserKey"){
        if(!runtime_.crypt_providers.contains(arg(0)))return fail(6);if(u32(1)!=1 && u32(1)!=2)return fail(bad_algorithm);
        const auto key=runtime_.legacy_crypto_user_keys.find({arg(0),u32(1)});if(key==runtime_.legacy_crypto_user_keys.end())return fail(no_key);
        return new_key(key->second,arg(2));
    }
    if(symbol=="CryptDestroyKey" || symbol=="CryptDuplicateKey" || symbol=="CryptExportKey" || symbol=="CryptGetKeyParam"){
        const auto found=runtime_.legacy_crypto_keys.find(arg(0));if(found==runtime_.legacy_crypto_keys.end())return fail(bad_key);const auto& key=found->second;
        if(symbol=="CryptDestroyKey"){runtime_.legacy_crypto_keys.erase(found);return 1;}
        if(symbol=="CryptDuplicateKey"){if(arg(1))return fail(87);if(u32(2))return fail(bad_flags);return new_key(key,arg(3));}
        if(symbol=="CryptGetKeyParam"){
            if(u32(4))return fail(bad_flags);if(!(key.permissions&8))return fail(bad_key_state);
            const auto* algorithm=legacy_crypto::find(key.algorithm);if(!algorithm)return fail(bad_algorithm);
            switch(u32(1)){
            case 1:if(key.secret.empty() || algorithm->block==1)return fail(bad_type);return write(key.iv,arg(2),arg(3));
            case 2:if(key.algorithm!=0x6602 && key.algorithm!=0x6801)return fail(bad_type);return write(key.salt,arg(2),arg(3));
            case 3:case 4:case 5:if(key.secret.empty() || algorithm->block==1)return fail(bad_type);return write(word(u32(1)==3?key.padding:u32(1)==4?key.mode:key.mode_bits),arg(2),arg(3));
            case 6:return write(word(key.permissions),arg(2),arg(3));
            case 7:return write(word(key.algorithm),arg(2),arg(3));
            case 8:return write(word(legacy_crypto::rsa(key.algorithm)?key.bits:algorithm->block==1?0:algorithm->block*8),arg(2),arg(3));
            case 9:return write(word(key.bits),arg(2),arg(3));
            case 19:if(key.algorithm!=0x6602)return fail(bad_type);return write(word(key.effective_bits),arg(2),arg(3));
            default:return fail(bad_type);
            }
        }
        if(arg(1))return fail(bad_key);if(u32(3))return fail(bad_flags);
        if(u32(2)==8){
            if(key.secret.empty())return fail(bad_type);if(!key.exportable || !(key.permissions&4))return fail(bad_key_state);
            Bytes blob{8,2,0,0};const auto algorithm=word(key.algorithm),size=word(static_cast<std::uint32_t>(key.secret.size()));blob.insert(blob.end(),algorithm.begin(),algorithm.end());blob.insert(blob.end(),size.begin(),size.end());blob.insert(blob.end(),key.secret.begin(),key.secret.end());return write(blob,arg(4),arg(5));
        }
        if((u32(2)!=6 && u32(2)!=7) || !key.secret.empty())return fail(bad_type);
        if(u32(2)==7 && (!key.private_key || !key.exportable || !(key.permissions&4)))return fail(bad_key_state);
        const auto bytes=export_rsa(key,u32(2)==7);if(bytes.empty())return fail(bad_key);return write(bytes,arg(4),arg(5));
    }
    if(symbol=="CryptCreateHash"){
        const auto provider=runtime_.crypt_providers.find(arg(0));if(provider==runtime_.crypt_providers.end())return fail(6);
        const auto* info=hash_info(u32(1));if(!info || (provider->second.type==1 && u32(1)>0x8008))return fail(bad_algorithm);
        if(arg(2))return fail(bad_key);if(u32(3))return fail(bad_flags);if(!arg(4))return fail(87);
        if(runtime_.legacy_crypto_hashes.size()>=4096)return fail(8);memory.check(GuestAddress{arg(4)},x86_?4:8,Access::Write);
        LegacyCryptHash hash;hash.provider=arg(0);hash.algorithm=u32(1);hash.hash=CryptoHash(hash.algorithm==0x8008?"MD5":info->name,false,{});
        if(hash.algorithm==0x8008)hash.tail=CryptoHash("SHA1",false,{});
        const auto handle=runtime_.next_crypto_handle++;runtime_.legacy_crypto_hashes.emplace(handle,std::move(hash));memory.store(GuestAddress{arg(4)},x86_?32:64,handle);return 1;
    }
    const auto found=runtime_.legacy_crypto_hashes.find(arg(0));if(found==runtime_.legacy_crypto_hashes.end())return fail(bad_hash);
    auto& hash=found->second;const auto* info=hash_info(hash.algorithm);
    if(symbol=="CryptDestroyHash"){runtime_.legacy_crypto_hashes.erase(found);return 1;}
    if(symbol=="CryptDuplicateHash"){
        if(arg(1) || !arg(3))return fail(87);if(u32(2))return fail(bad_flags);
        if(runtime_.legacy_crypto_hashes.size()>=4096)return fail(8);memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);
        const auto handle=runtime_.next_crypto_handle++;runtime_.legacy_crypto_hashes.emplace(handle,hash);memory.store(GuestAddress{arg(3)},x86_?32:64,handle);return 1;
    }
    if(symbol=="CryptHashData"){
        if(u32(3))return fail(bad_flags);if(hash.finished)return fail(bad_hash_state);
        if(u32(2)>output_limit)return fail(8);if(u32(2) && !arg(1))return fail(87);
        const auto bytes=read(arg(1),u32(2));hash.hash.update(bytes);if(hash.algorithm==0x8008)hash.tail.update(bytes);return 1;
    }
    if(symbol=="CryptSetHashParam"){
        if(u32(3))return fail(bad_flags);if(u32(1)!=2)return fail(bad_type);if(!arg(2))return fail(87);
        if(hash.finished)return fail(bad_hash_state);hash.value=read(arg(2),info->size);hash.finished=true;return 1;
    }
    if(symbol=="CryptGetHashParam"){
        if(u32(4))return fail(bad_flags);
        if(u32(1)==1 || u32(1)==4)return write(word(u32(1)==1?hash.algorithm:info->size),arg(2),arg(3));
        if(u32(1)!=2)return fail(bad_type);const auto bytes=hash_value(hash);const auto result=write(bytes,arg(2),arg(3));
        if(result && arg(2)){hash.value=bytes;hash.finished=true;}return result;
    }
    const bool signing=symbol=="CryptSignHashA" || symbol=="CryptSignHashW";
    if(!signing && symbol!="CryptVerifySignatureA" && symbol!="CryptVerifySignatureW")throw HostCallFault("未知的 CryptoAPI 雜湊／金鑰服務");
    if(arg(signing?2:4))return fail(87);const auto flags=u32(signing?3:5);if(flags&~1U)return fail(bad_flags);
    const LegacyCryptKey* key=nullptr;
    if(signing){
        if(u32(1)!=1 && u32(1)!=2)return fail(bad_algorithm);
        const auto stored=runtime_.legacy_crypto_user_keys.find({hash.provider,u32(1)});if(stored==runtime_.legacy_crypto_user_keys.end())return fail(no_key);key=&stored->second;
    }else {
        const auto stored=runtime_.legacy_crypto_keys.find(arg(3));if(stored==runtime_.legacy_crypto_keys.end())return fail(bad_key);key=&stored->second;
    }
    if(signing){
        if(!arg(5))return fail(87);memory.check(GuestAddress{arg(5)},4,Access::Write);
        const auto capacity=memory.load(GuestAddress{arg(5)},32);memory.store(GuestAddress{arg(5)},32,key->bits/8);
        if(!arg(4))return 1;if(capacity<key->bits/8)return fail(234);memory.check(GuestAddress{arg(4)},key->bits/8,Access::Write);
    }else if(u32(2)!=key->bits/8 || !arg(1))return fail(bad_signature);
    auto native=decode_key(*key);Context context(native?EVP_PKEY_CTX_new(native.get(),nullptr):nullptr,EVP_PKEY_CTX_free);
    if(!context || (signing?EVP_PKEY_sign_init(context.get()):EVP_PKEY_verify_init(context.get()))<=0 || EVP_PKEY_CTX_set_rsa_padding(context.get(),RSA_PKCS1_PADDING)<=0)return fail(bad_key);
    if(!(flags&1) && EVP_PKEY_CTX_set_signature_md(context.get(),EVP_get_digestbyname(info->name))<=0)return fail(bad_algorithm);
    const auto value=hash_value(hash);Bytes signature(key->bits/8);
    hash.value=value;hash.finished=true;
    if(signing){
        std::size_t size=signature.size();if(EVP_PKEY_sign(context.get(),signature.data(),&size,value.data(),value.size())<=0 || size!=signature.size())return fail(bad_data);
        std::reverse(signature.begin(),signature.end());memory.write(GuestAddress{arg(4)},signature);return 1;
    }
    memory.read(GuestAddress{arg(1)},signature);std::reverse(signature.begin(),signature.end());
    return EVP_PKEY_verify(context.get(),signature.data(),signature.size(),value.data(),value.size())==1?1:fail(bad_signature);
}
}
