#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "cng_asymmetric.hpp"
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include <algorithm>
#include <memory>

namespace x64arm {
namespace {
constexpr std::uint32_t invalid=0xc000000dU, bad_handle=0xc0000008U, unsupported=0xc00000bbU;
constexpr std::uint32_t small=0xc0000023U, no_memory=0xc0000017U, bad_signature=0xc000a000U;
using Bytes=std::vector<std::uint8_t>;
using Big=std::unique_ptr<BIGNUM,decltype(&BN_clear_free)>;
using Context=std::unique_ptr<BN_CTX,decltype(&BN_CTX_free)>;
using Pkey=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
Big number(std::span<const std::uint8_t> bytes) {return Big(BN_bin2bn(bytes.data(),static_cast<int>(bytes.size()),nullptr),BN_clear_free);}
Big number() {return Big(BN_new(),BN_clear_free);}
Bytes encoded(const BIGNUM* value,std::size_t size) {
    Bytes result(size);if (BN_bn2binpad(value,result.data(),static_cast<int>(size))<0) return {};return result;
}
void word(Bytes& bytes,std::uint32_t value) {for (unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));}
std::uint32_t word(const Bytes& bytes,std::size_t offset) {
    if (offset>bytes.size() || bytes.size()-offset<4) return 0;
    std::uint32_t result=0;for (unsigned i=0;i<4;++i) result|=std::uint32_t{bytes[offset+i]}<<(8*i);return result;
}
void append(Bytes& bytes,const Bytes& tail) {bytes.insert(bytes.end(),tail.begin(),tail.end());}
Bytes slice(const Bytes& bytes,std::size_t offset,std::size_t count) {return Bytes(bytes.begin()+static_cast<std::ptrdiff_t>(offset),bytes.begin()+static_cast<std::ptrdiff_t>(offset+count));}
bool dh_range(const BIGNUM* value,const BIGNUM* modulus) {
    auto limit=Big(BN_dup(modulus),BN_clear_free);
    return limit && BN_sub_word(limit.get(),1)==1 && BN_cmp(value,BN_value_one())>0 && BN_cmp(value,limit.get())<0;
}

}
std::uint32_t WindowsServices::crypto_key_call(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol) {
    try {
    ERR_clear_error();
    const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto wide=[&](unsigned i){return utf16_to_utf8(read_utf16(memory,GuestAddress{arg(i)},1024));};
    const auto read=[&](unsigned pointer,unsigned length) {
        const auto count=u32(length);if (count>output_limit) throw HostCallFault("密碼服務資料超過 16 MiB 上限");
        Bytes data(count);memory.read(GuestAddress{arg(pointer)},data);return data;
    };
    const auto output=[&](const Bytes& bytes,unsigned pointer,unsigned length,unsigned written)->std::uint32_t {
        if (!arg(written)) return invalid;
        memory.check(GuestAddress{arg(written)},4,Access::Write);
        if (arg(pointer) && u32(length)>=bytes.size()) memory.check(GuestAddress{arg(pointer)},bytes.size(),Access::Write);
        memory.store(GuestAddress{arg(written)},32,bytes.size());
        if (!arg(pointer)) return 0;
        if (u32(length)<bytes.size()) return small;
        memory.write(GuestAddress{arg(pointer)},bytes);return 0;
    };
    auto algorithm=runtime_.crypto_algorithms.find(arg(0));auto key=runtime_.crypto_keys.find(arg(0));
    const auto create=[&](CryptoKey value,unsigned target)->std::uint32_t {
        if (!arg(target)) return invalid;
        memory.check(GuestAddress{arg(target)},x86_?4:8,Access::Write);
        if (runtime_.crypto_keys.size()>=4096) return no_memory;
        const auto handle=runtime_.next_crypto_handle++;runtime_.crypto_keys.emplace(handle,std::move(value));
        memory.store(GuestAddress{arg(target)},x86_?32:64,handle);return 0;
    };
    if (symbol=="BCryptDestroyKey") {
        if (key==runtime_.crypto_keys.end()) return bad_handle;
        OPENSSL_cleanse(key->second.secret.data(),key->second.secret.size());OPENSSL_cleanse(key->second.der.data(),key->second.der.size());runtime_.crypto_keys.erase(key);return 0;
    }
    if (symbol=="BCryptDestroySecret") {
        const auto found=runtime_.crypto_secrets.find(arg(0));if (found==runtime_.crypto_secrets.end()) return bad_handle;
        OPENSSL_cleanse(found->second.data(),found->second.size());runtime_.crypto_secrets.erase(found);return 0;
    }
    if (symbol=="BCryptGetProperty" || symbol=="BCryptSetProperty") {
        if (algorithm==runtime_.crypto_algorithms.end() && key==runtime_.crypto_keys.end()) return bad_handle;
        auto& kind=key!=runtime_.crypto_keys.end()?key->second.algorithm:algorithm->second;
        const auto property=wide(1);
        if (symbol=="BCryptSetProperty") {
            if (u32(4)) return invalid;
            if (property=="ChainingMode" && kind.name=="AES") {
                if (!arg(2) || u32(3)<2 || u32(3)>128 || u32(3)%2) return invalid;
                const auto value=utf16_to_utf8(read_utf16(memory,GuestAddress{arg(2)},u32(3)/2));
                if (value!="ChainingModeCBC" && value!="ChainingModeECB") return unsupported;
                kind.chaining=value;return 0;
            }
            if (property=="DHParameters" && key!=runtime_.crypto_keys.end() && kind.name=="DH" && !key->second.finalized) {
                const auto data=read(2,3);const auto width=word(data,8);
                if (word(data,0)!=data.size() || word(data,4)!=0x4d504844 || width!=key->second.bits/8 || data.size()!=12+2*width) return invalid;
                auto modulus=number(std::span(data).subspan(12,width)),generator=number(std::span(data).subspan(12+width,width));
                Context context(BN_CTX_new(),BN_CTX_free);
                if (!modulus || !generator || !context) return no_memory;
                if (!dh_range(generator.get(),modulus.get()) || !BN_is_odd(modulus.get()) || BN_num_bits(modulus.get())<512 || BN_check_prime(modulus.get(),context.get(),nullptr)!=1) return invalid;
                key->second.modulus=slice(data,12,width);key->second.generator=slice(data,12+width,width);return 0;
            }
            return unsupported;
        }
        if (u32(5)) return invalid;
        Bytes bytes;
        if (property=="AlgorithmName") bytes=utf16_bytes(utf8_to_utf16(kind.name));
        else if (property=="ChainingMode" && kind.name=="AES") bytes=utf16_bytes(utf8_to_utf16(kind.chaining));
        else if (property=="ObjectLength") word(bytes,256);
        else if (property=="BlockLength" && kind.name=="AES") word(bytes,16);
        else if (property=="KeyLength" && key!=runtime_.crypto_keys.end()) word(bytes,key->second.bits);
        else if (property=="KeyLengths") {const auto* c=cng::curve(kind.name);word(bytes,c?c->bits:kind.name=="AES"?128:512);word(bytes,c?c->bits:kind.name=="AES"?256:cng::rsa(kind.name)?16384:8192);word(bytes,c?0:64);}
        else if (property=="SignatureLength" && key!=runtime_.crypto_keys.end() && (cng::rsa(kind.name) || kind.name.starts_with("ECDSA"))) word(bytes,((key->second.bits+7)/8)*(cng::rsa(kind.name)?1:2));
        else return unsupported;
        return output(bytes,2,3,4);
    }
    if (symbol=="BCryptGenerateSymmetricKey" || symbol=="BCryptImportKey" || symbol=="BCryptImportKeyPair" || symbol=="BCryptGenerateKeyPair") {
        if (algorithm==runtime_.crypto_algorithms.end()) return bad_handle;
        CryptoKey value;value.algorithm=algorithm->second;
        const auto& kind=value.algorithm.name;
        if (symbol=="BCryptGenerateKeyPair") {
            if (u32(3)) return invalid;
            const auto* c=cng::curve(kind);
            if (kind!="DH" && !cng::supported(kind)) return unsupported;
            if (c?u32(2)!=c->bits:u32(2)<512 || u32(2)>(cng::rsa(kind)?16384U:8192U) || u32(2)%64) return invalid;
            value.bits=u32(2);return create(std::move(value),1);
        }
        if (symbol=="BCryptImportKeyPair") {
            if (arg(1) || (u32(6)&~8U)) return invalid;
            const auto type=wide(2);const auto data=read(4,5);
            if (cng::supported(kind)) {
                std::string imported;auto native=cng::from_blob(data,type,imported,value.private_key);
                if (imported!=kind && !(cng::rsa(kind) && imported=="RSA")) return invalid;
                value.bits=static_cast<std::uint32_t>(EVP_PKEY_get_bits(native.get()));value.der=cng::der(native.get(),value.private_key);
            } else if (kind=="DH" && (type=="DHPUBLICBLOB" || type=="DHPRIVATEBLOB")) {
                const bool priv=type=="DHPRIVATEBLOB";const auto width=word(data,4);
                if (width<64 || width>1024 || width%8 || word(data,0)!=(priv?0x56504844U:0x42504844U) || data.size()!=8+(priv?4:3)*width) return invalid;
                value.bits=width*8;value.modulus=slice(data,8,width);value.generator=slice(data,8+width,width);value.public_value=slice(data,8+2*width,width);
                if (priv) value.secret=slice(data,8+3*width,width);
                auto n=number(value.modulus),g=number(value.generator),y=number(value.public_value);
                if (!n || !g || !y) return no_memory;
                if (!BN_is_odd(n.get()) || !dh_range(g.get(),n.get()) || !dh_range(y.get(),n.get())) return invalid;
                if(priv) {
                    auto x=number(value.secret),computed=number();Context context(BN_CTX_new(),BN_CTX_free);
                    if(!x || !computed || !context)return no_memory;
                    if(!dh_range(x.get(),n.get()) || BN_check_prime(n.get(),context.get(),nullptr)!=1 || !BN_mod_exp_mont_consttime(computed.get(),g.get(),x.get(),n.get(),context.get(),nullptr) || BN_cmp(computed.get(),y.get()))return invalid;
                }
            } else return unsupported;
            value.finalized=true;return create(std::move(value),3);
        }
        if (kind!="AES") return unsupported;
        const bool imported=symbol=="BCryptImportKey";
        const auto target=imported?3U:1U,object=imported?4U:2U,length=object+1;
        if (u32(imported?8:6) || (arg(object)?u32(length)<256:u32(length)!=0)) return invalid;
        if (arg(object)) memory.check(GuestAddress{arg(object)},256,Access::Write);
        if (imported) {
            if (arg(1) || wide(2)!="KeyDataBlob") return unsupported;
            const auto data=read(6,7);const auto count=word(data,8);
            if (data.size()<12 || word(data,0)!=0x4d42444b || word(data,4)!=1 || data.size()!=12+count) return invalid;
            value.secret=slice(data,12,count);
        } else value.secret=read(4,5);
        if (value.secret.size()!=16 && value.secret.size()!=24 && value.secret.size()!=32) return invalid;
        value.bits=static_cast<std::uint32_t>(value.secret.size()*8);value.finalized=true;return create(std::move(value),target);
    }
    if (symbol=="BCryptDeriveKey") {
        const auto found=runtime_.crypto_secrets.find(arg(0));if (found==runtime_.crypto_secrets.end()) return bad_handle;
        if (arg(2) || u32(6)) return invalid;
        if (wide(1)!="TRUNCATE") return unsupported;
        return output(found->second,3,4,5);
    }
    if (key==runtime_.crypto_keys.end()) return bad_handle;
    auto& value=key->second;const auto& kind=value.algorithm.name;
    if (symbol=="BCryptFinalizeKeyPair") {
        if (u32(1) || value.finalized) return invalid;
        if(cng::supported(kind)){
            if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生金鑰產生");
            auto native=cng::generate(kind,value.bits);auto next=value;next.der=cng::der(native.get(),true);next.private_key=true;next.finalized=true;
            value=std::move(next);return 0;
        }
        if(kind!="DH" || value.modulus.empty()) return invalid;
        auto n=number(value.modulus),g=number(value.generator),x=number(),y=number();Context context(BN_CTX_new(),BN_CTX_free);
        if (!n || !g || !x || !y || !context) return no_memory;
        Bytes random(static_cast<std::size_t>((BN_num_bits(n.get())+7)/8));bool accepted=false;
        for (unsigned retry=0;retry<128 && !accepted;++retry) {
            if (!random_bytes(random)) return no_memory;
            random[0]&=static_cast<std::uint8_t>(0xffU>>(random.size()*8-static_cast<std::size_t>(BN_num_bits(n.get()))));
            if (!BN_bin2bn(random.data(),static_cast<int>(random.size()),x.get())) return no_memory;
            accepted=dh_range(x.get(),n.get());
        }
        OPENSSL_cleanse(random.data(),random.size());if (!accepted) return no_memory;
        if (!BN_mod_exp_mont_consttime(y.get(),g.get(),x.get(),n.get(),context.get(),nullptr)) return invalid;
        value.secret=encoded(x.get(),value.modulus.size());value.public_value=encoded(y.get(),value.modulus.size());value.finalized=true;return 0;
    }
    if (!value.finalized) return invalid;
    if (symbol=="BCryptExportKey") {
        if (arg(1) || u32(6)) return invalid;
        const auto type=wide(2);Bytes bytes;
        if (kind=="DH" && (type=="DHPUBLICBLOB" || type=="DHPRIVATEBLOB")) {
            const bool priv=type=="DHPRIVATEBLOB";if (priv && value.secret.empty()) return invalid;
            word(bytes,priv?0x56504844:0x42504844);word(bytes,value.bits/8);append(bytes,value.modulus);append(bytes,value.generator);append(bytes,value.public_value);if (priv) append(bytes,value.secret);
        } else if (cng::supported(kind)) {
            const bool priv=type=="PRIVATEBLOB" || type=="ECCPRIVATEBLOB" || type=="RSAPRIVATEBLOB" || type=="RSAFULLPRIVATEBLOB";
            if(priv && !value.private_key)return invalid;
            auto native=cng::decode(value.der,value.private_key);bytes=cng::blob(native.get(),kind,value.bits,type,priv);
        } else if (kind=="AES" && type=="KeyDataBlob") {
            word(bytes,0x4d42444b);word(bytes,1);word(bytes,value.bits/8);append(bytes,value.secret);
        } else return unsupported;
        return output(bytes,3,4,5);
    }
    if (symbol=="BCryptSecretAgreement") {
        const auto peer=runtime_.crypto_keys.find(arg(1));if (peer==runtime_.crypto_keys.end()) return bad_handle;
        if (u32(3) || !arg(2)) return invalid;
        if(kind.starts_with("ECDH")){
            if(!value.private_key || !peer->second.finalized || peer->second.algorithm.name!=kind)return invalid;
            memory.check(GuestAddress{arg(2)},x86_?4:8,Access::Write);if(runtime_.crypto_secrets.size()>=4096)return no_memory;
            auto native=cng::decode(value.der,true),other=cng::decode(peer->second.der,peer->second.private_key);
            auto bytes=cng::agreement(native.get(),other.get());const auto handle=runtime_.next_crypto_handle++;
            runtime_.crypto_secrets.emplace(handle,std::move(bytes));memory.store(GuestAddress{arg(2)},x86_?32:64,handle);return 0;
        }
        if (kind!="DH" || value.secret.empty() || !peer->second.finalized || peer->second.algorithm.name!="DH" || peer->second.modulus!=value.modulus || peer->second.generator!=value.generator) return invalid;
        memory.check(GuestAddress{arg(2)},x86_?4:8,Access::Write);if (runtime_.crypto_secrets.size()>=4096) return no_memory;
        auto n=number(value.modulus),y=number(peer->second.public_value),x=number(value.secret),result=number();Context context(BN_CTX_new(),BN_CTX_free);
        if (!n || !x || !y || !result || !context) return no_memory;
        if (!dh_range(y.get(),n.get()) || !BN_mod_exp_mont_consttime(result.get(),y.get(),x.get(),n.get(),context.get(),nullptr) || !dh_range(result.get(),n.get())) return invalid;
        auto bytes=encoded(result.get(),value.modulus.size());std::reverse(bytes.begin(),bytes.end());
        const auto handle=runtime_.next_crypto_handle++;runtime_.crypto_secrets.emplace(handle,std::move(bytes));memory.store(GuestAddress{arg(2)},x86_?32:64,handle);return 0;
    }
    if (symbol=="BCryptSignHash" || symbol=="BCryptVerifySignature") {
        const bool sign=symbol=="BCryptSignHash";
        if(!cng::rsa(kind) && !kind.starts_with("ECDSA"))return unsupported;
        if(sign && !value.private_key)return invalid;
        cng::SignatureOptions options;options.padding=u32(sign?7:6);
        if(cng::rsa(kind)){
            if(!arg(1) || (options.padding!=2 && options.padding!=8))return invalid;
            const auto pointer=memory.load(GuestAddress{arg(1)},x86_?32:64);
            const auto name=pointer?utf16_to_utf8(read_utf16(memory,GuestAddress{pointer},64)):std::string{};
            options.digest=name.empty()?nullptr:EVP_get_digestbyname(name.c_str());if(!name.empty() && !options.digest)return invalid;
            if(options.padding==8)options.salt=static_cast<unsigned>(memory.load(GuestAddress{arg(1)+(x86_?4:8)},32));
        }else if(arg(1) || options.padding)return invalid;
        const auto hash=read(2,3);auto native=cng::decode(value.der,value.private_key);
        if(sign){const bool size_only=!arg(4) || u32(5)<cng::signature_size(native.get());return output(cng::sign(native.get(),hash,options,size_only),4,5,6);}
        const auto signature=read(4,5);cng::verify(native.get(),hash,signature,options);return 0;
    }
    if (symbol=="BCryptEncrypt" || symbol=="BCryptDecrypt") {
        if (!arg(8)) return invalid;
        const bool encrypt=symbol=="BCryptEncrypt";const auto input=read(1,2);
        if (kind=="RSA") {
            if(arg(4) || u32(5) || (!encrypt && !value.private_key))return invalid;
            cng::CipherOptions options;options.padding=u32(9);
            if(options.padding==1 && input.size()!=(value.bits+7)/8)return 0xc0000206U;
            if(options.padding==4){
                if(!arg(3))return invalid;const auto unit=x86_?4U:8U;
                const auto pointer=memory.load(GuestAddress{arg(3)},x86_?32:64);
                if(!pointer)return invalid;const auto name=utf16_to_utf8(read_utf16(memory,GuestAddress{pointer},64));
                options.digest=EVP_get_digestbyname(name.c_str());if(!options.digest)return invalid;
                const auto label=memory.load(GuestAddress{arg(3)+unit},x86_?32:64),length=memory.load(GuestAddress{arg(3)+unit*2},32);
                if(length>output_limit || (length && !label))return invalid;options.label.resize(static_cast<std::size_t>(length));memory.read(GuestAddress{label},options.label);
            }else if(arg(3))return invalid;
            auto native=cng::decode(value.der,value.private_key);return output(cng::crypt(native.get(),input,options,encrypt,!arg(6)),6,7,8);
        }
        if (kind!="AES") return unsupported;
        if (arg(3)) return invalid;
        if (u32(9)&~1U) return invalid;
        const bool padding=u32(9)&1,cbc=value.algorithm.chaining=="ChainingModeCBC";
        if (cbc?(arg(4)?u32(5)!=16:u32(5)!=0):(arg(4)||u32(5))) return invalid;
        if ((!padding || !encrypt) && input.size()%16) return 0xc0000206U;
        if (!arg(6)) return output(Bytes(encrypt&&padding?(input.size()/16+1)*16:input.size()),6,7,8);
        Bytes iv(16);if (arg(4)) {memory.read(GuestAddress{arg(4)},iv);memory.check(GuestAddress{arg(4)},16,Access::Write);}
        std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)> context(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);
        const auto cipher_name="AES-"+std::to_string(value.bits)+(cbc?"-CBC":"-ECB");const auto* cipher=EVP_get_cipherbyname(cipher_name.c_str());
        if (!context || !cipher) return no_memory;
        Bytes bytes(input.size()+16);int first=0,last=0;
        if (!EVP_CipherInit_ex(context.get(),cipher,nullptr,value.secret.data(),cbc?iv.data():nullptr,encrypt?1:0) || !EVP_CIPHER_CTX_set_padding(context.get(),padding?1:0) || !EVP_CipherUpdate(context.get(),bytes.data(),&first,input.data(),static_cast<int>(input.size())) || !EVP_CipherFinal_ex(context.get(),bytes.data()+first,&last)) return invalid;
        bytes.resize(static_cast<std::size_t>(first+last));const auto result=output(bytes,6,7,8);
        if (!result && cbc && arg(4)) {
            const auto& chain=encrypt?bytes:input;
            if (chain.size()>=16) memory.write(GuestAddress{arg(4)},std::span(chain).last(16));
        }
        return result;
    }
    return unsupported;
    }catch(const cng::Error& error){return error.code==cng::no_memory?no_memory:error.code==cng::unsupported?unsupported:error.code==cng::bad_signature?bad_signature:invalid;}
    catch(const MemoryFault&){return invalid;}
    catch(const std::invalid_argument&){return invalid;}
    catch(const std::bad_alloc&){return no_memory;}
}
}
