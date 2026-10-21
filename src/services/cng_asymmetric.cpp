#include "cng_asymmetric.hpp"
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <algorithm>

namespace x64arm::cng {
void word(Bytes& out,std::uint32_t value){for(unsigned i=0;i<4;++i)out.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
std::uint32_t word(std::span<const std::uint8_t> data,std::size_t at){require(at<=data.size() && data.size()-at>=4,bad_data);std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t(data[at+i])<<(8*i);return value;}
Bytes number(const BIGNUM* value,unsigned width){Bytes out(width);require(BN_bn2binpad(value,out.data(),static_cast<int>(width))==static_cast<int>(width),bad_data);return out;}
Big component(EVP_PKEY* key,const char* name){BIGNUM* raw=nullptr;require(EVP_PKEY_get_bn_param(key,name,&raw)==1,bad_data);return Big(raw,BN_clear_free);}
void append_number(Bytes& out,EVP_PKEY* key,const char* name,unsigned width){auto value=component(key,name);auto bytes=number(value.get(),width);out.insert(out.end(),bytes.begin(),bytes.end());}
constexpr Curve curves[]{{"ECDSA_P256","prime256v1",256,0x31534345},{"ECDSA_P384","secp384r1",384,0x33534345},{"ECDSA_P521","secp521r1",521,0x35534345},{"ECDH_P256","prime256v1",256,0x314b4345},{"ECDH_P384","secp384r1",384,0x334b4345},{"ECDH_P521","secp521r1",521,0x354b4345}};
const Curve* curve(std::string_view name){const auto at=std::find_if(std::begin(curves),std::end(curves),[&](const auto& c){return name==c.algorithm;});return at==std::end(curves)?nullptr:at;}
bool rsa(std::string_view name){return name=="RSA" || name=="RSA_SIGN";}
bool supported(std::string_view name){return rsa(name) || curve(name);}
Bytes der(EVP_PKEY* key,bool priv){const auto count=priv?i2d_PrivateKey(key,nullptr):i2d_PUBKEY(key,nullptr);require(count>0,bad_data);Bytes out(static_cast<std::size_t>(count));auto* p=out.data();require((priv?i2d_PrivateKey(key,&p):i2d_PUBKEY(key,&p))==count,bad_data);return out;}
Key decode(std::span<const std::uint8_t> bytes,bool priv){
    require(!bytes.empty() && bytes.size()<=65536,bad_data);
    const auto* p=bytes.data();Key key(priv?d2i_AutoPrivateKey(nullptr,&p,static_cast<long>(bytes.size())):d2i_PUBKEY(nullptr,&p,static_cast<long>(bytes.size())),EVP_PKEY_free);
    require(key && p==bytes.data()+bytes.size(),bad_data);return key;
}
void validate(EVP_PKEY* key,bool priv){Context check(EVP_PKEY_CTX_new(key,nullptr),EVP_PKEY_CTX_free);require(bool(check),no_memory);require((priv?EVP_PKEY_pairwise_check(check.get()):EVP_PKEY_public_check(check.get()))==1,bad_data);}
std::string algorithm(EVP_PKEY* key){
    if(EVP_PKEY_is_a(key,"RSA"))return "RSA";
    if(EVP_PKEY_is_a(key,"EC")){char group[80]{};std::size_t length{};require(EVP_PKEY_get_utf8_string_param(key,OSSL_PKEY_PARAM_GROUP_NAME,group,sizeof(group),&length)==1,bad_data);for(const auto& c:curves)if(std::string_view(group)==c.group)return c.algorithm;}
    throw Error{unsupported};
}
Key from_blob(const Bytes& bytes,std::string_view type,std::string& name,bool& priv){
    const auto magic=word(bytes,0);const bool generic=type=="PUBLICBLOB" || type=="PRIVATEBLOB";
    std::unique_ptr<OSSL_PARAM_BLD,decltype(&OSSL_PARAM_BLD_free)> builder(OSSL_PARAM_BLD_new(),OSSL_PARAM_BLD_free);require(bool(builder),no_memory);
    std::vector<Big> values;std::size_t at=0;Bytes public_point;
    const auto add=[&](const char* field,unsigned width){require(width && at<=bytes.size() && width<=bytes.size()-at,bad_data);values.emplace_back(BN_bin2bn(bytes.data()+at,static_cast<int>(width),nullptr),BN_clear_free);at+=width;require(values.back() && OSSL_PARAM_BLD_push_BN(builder.get(),field,values.back().get()),bad_data);};
    const char* native=nullptr;
    if(magic==0x31415352 || magic==0x32415352 || magic==0x33415352){
        priv=magic!=0x31415352;const bool full=magic==0x33415352;require(type==(priv?(full?"RSAFULLPRIVATEBLOB":"RSAPRIVATEBLOB"):"RSAPUBLICBLOB") || (generic && type==(priv?"PRIVATEBLOB":"PUBLICBLOB")),bad_data);
        const auto bits=word(bytes,4),e=word(bytes,8),n=word(bytes,12),p=word(bytes,16),q=word(bytes,20);
        require(bits>=512 && bits<=16384 && bits%8==0 && n==bits/8 && e && e<=n && ((!priv && !p && !q) || (priv && p && q && p<=n && q<=n)),bad_data);
        const std::uint64_t expected=24ULL+e+n+(priv?p+q:0)+(full?2ULL*p+q+n:0);require(bytes.size()==expected,bad_data);
        at=24;add(OSSL_PKEY_PARAM_RSA_E,e);add(OSSL_PKEY_PARAM_RSA_N,n);
        if(priv){add(OSSL_PKEY_PARAM_RSA_FACTOR1,p);add(OSSL_PKEY_PARAM_RSA_FACTOR2,q);
            if(full){add(OSSL_PKEY_PARAM_RSA_EXPONENT1,p);add(OSSL_PKEY_PARAM_RSA_EXPONENT2,q);add(OSSL_PKEY_PARAM_RSA_COEFFICIENT1,p);add(OSSL_PKEY_PARAM_RSA_D,n);}
            else {Big pm(BN_dup(values[2].get()),BN_clear_free),qm(BN_dup(values[3].get()),BN_clear_free),totient(BN_new(),BN_clear_free);std::unique_ptr<BN_CTX,decltype(&BN_CTX_free)> ctx(BN_CTX_new(),BN_CTX_free);require(pm && qm && totient && ctx,no_memory);require(BN_sub_word(pm.get(),1) && BN_sub_word(qm.get(),1) && BN_mul(totient.get(),pm.get(),qm.get(),ctx.get()),bad_data);values.emplace_back(BN_mod_inverse(nullptr,values[0].get(),totient.get(),ctx.get()),BN_clear_free);require(values.back() && OSSL_PARAM_BLD_push_BN(builder.get(),OSSL_PKEY_PARAM_RSA_D,values.back().get()),bad_data);
                // RSA2 blob 僅含 n/e/p/q；補出 d 與 CRT 參數後再驗證整組金鑰。
                const auto d=values.back().get();values.emplace_back(BN_new(),BN_clear_free);require(values.back() && BN_mod(values.back().get(),d,pm.get(),ctx.get()) && OSSL_PARAM_BLD_push_BN(builder.get(),OSSL_PKEY_PARAM_RSA_EXPONENT1,values.back().get()),bad_data);values.emplace_back(BN_new(),BN_clear_free);require(values.back() && BN_mod(values.back().get(),d,qm.get(),ctx.get()) && OSSL_PARAM_BLD_push_BN(builder.get(),OSSL_PKEY_PARAM_RSA_EXPONENT2,values.back().get()),bad_data);values.emplace_back(BN_mod_inverse(nullptr,values[3].get(),values[2].get(),ctx.get()),BN_clear_free);require(values.back() && OSSL_PARAM_BLD_push_BN(builder.get(),OSSL_PKEY_PARAM_RSA_COEFFICIENT1,values.back().get()),bad_data);
            }
        }
        name="RSA";native="RSA";
    }else{
        const Curve* c=nullptr;for(const auto& candidate:curves)if(magic==candidate.magic || magic==candidate.magic+0x1000000){c=&candidate;break;}require(c!=nullptr,bad_data);priv=magic!=c->magic;const auto width=word(bytes,4);require(width==(c->bits+7)/8 && bytes.size()==8ULL+(priv?3ULL:2ULL)*width,bad_data);require(type==(priv?"ECCPRIVATEBLOB":"ECCPUBLICBLOB") || (generic && type==(priv?"PRIVATEBLOB":"PUBLICBLOB")),bad_data);
        public_point.push_back(4);public_point.insert(public_point.end(),bytes.begin()+8,bytes.begin()+8+2*width);require(OSSL_PARAM_BLD_push_utf8_string(builder.get(),OSSL_PKEY_PARAM_GROUP_NAME,c->group,0) && OSSL_PARAM_BLD_push_octet_string(builder.get(),OSSL_PKEY_PARAM_PUB_KEY,public_point.data(),public_point.size()),bad_data);at=8+2*width;if(priv)add(OSSL_PKEY_PARAM_PRIV_KEY,width);name=c->algorithm;native="EC";
    }
    std::unique_ptr<OSSL_PARAM,decltype(&OSSL_PARAM_free)> params(OSSL_PARAM_BLD_to_param(builder.get()),OSSL_PARAM_free);Context context(EVP_PKEY_CTX_new_from_name(nullptr,native,nullptr),EVP_PKEY_CTX_free);require(params && context,no_memory);EVP_PKEY* raw=nullptr;const bool ok=EVP_PKEY_fromdata_init(context.get())>0 && EVP_PKEY_fromdata(context.get(),&raw,priv?EVP_PKEY_KEYPAIR:EVP_PKEY_PUBLIC_KEY,params.get())>0;Key result(raw,EVP_PKEY_free);require(ok && result,bad_data);validate(result.get(),priv);return result;
}
Bytes blob(EVP_PKEY* key,std::string_view name,unsigned bits,std::string type,bool priv){
    if(type=="PUBLICBLOB")type=rsa(name)?"RSAPUBLICBLOB":"ECCPUBLICBLOB";
    if(type=="PRIVATEBLOB")type=rsa(name)?"RSAPRIVATEBLOB":"ECCPRIVATEBLOB";
    Bytes out;
    if(rsa(name)){
        require(type=="RSAPUBLICBLOB" || type=="RSAPRIVATEBLOB" || type=="RSAFULLPRIVATEBLOB",unsupported);const bool full=type=="RSAFULLPRIVATEBLOB";auto e=component(key,OSSL_PKEY_PARAM_RSA_E);const auto es=static_cast<unsigned>(BN_num_bytes(e.get())),n=(bits+7)/8;unsigned p=0,q=0;
        if(priv){auto factor=component(key,OSSL_PKEY_PARAM_RSA_FACTOR1),other=component(key,OSSL_PKEY_PARAM_RSA_FACTOR2);p=static_cast<unsigned>(BN_num_bytes(factor.get()));q=static_cast<unsigned>(BN_num_bytes(other.get()));}
        word(out,priv?(full?0x33415352:0x32415352):0x31415352);word(out,bits);word(out,es);word(out,n);word(out,p);word(out,q);auto exponent=number(e.get(),es);out.insert(out.end(),exponent.begin(),exponent.end());append_number(out,key,OSSL_PKEY_PARAM_RSA_N,n);
        if(priv){append_number(out,key,OSSL_PKEY_PARAM_RSA_FACTOR1,p);append_number(out,key,OSSL_PKEY_PARAM_RSA_FACTOR2,q);if(full){append_number(out,key,OSSL_PKEY_PARAM_RSA_EXPONENT1,p);append_number(out,key,OSSL_PKEY_PARAM_RSA_EXPONENT2,q);append_number(out,key,OSSL_PKEY_PARAM_RSA_COEFFICIENT1,p);append_number(out,key,OSSL_PKEY_PARAM_RSA_D,n);}}
    }else{
        const auto* c=curve(name);require(c && (type=="ECCPUBLICBLOB" || type=="ECCPRIVATEBLOB"),unsupported);const auto width=(bits+7)/8;word(out,c->magic+(priv?0x1000000:0));word(out,width);append_number(out,key,OSSL_PKEY_PARAM_EC_PUB_X,width);append_number(out,key,OSSL_PKEY_PARAM_EC_PUB_Y,width);if(priv)append_number(out,key,OSSL_PKEY_PARAM_PRIV_KEY,width);
    }
    return out;
}

Key generate(std::string_view name,unsigned bits){
    const auto* c=curve(name);require(c || rsa(name),unsupported);
    require(c?bits==c->bits:bits>=512 && bits<=16384 && bits%64==0);
    Context ctx(EVP_PKEY_CTX_new_from_name(nullptr,c?"EC":"RSA",nullptr),EVP_PKEY_CTX_free);
    require(bool(ctx),no_memory);require(EVP_PKEY_keygen_init(ctx.get())>0,bad_data);
    if(c)require(EVP_PKEY_CTX_set_group_name(ctx.get(),c->group)>0,bad_data);
    else require(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(),static_cast<int>(bits))>0,bad_data);
    EVP_PKEY* raw=nullptr;const auto status=EVP_PKEY_generate(ctx.get(),&raw);Key key(raw,EVP_PKEY_free);
    require(status>0 && key,bad_data);return key;
}
namespace {
Context signature_context(EVP_PKEY* key,std::size_t input_size,const SignatureOptions& options,bool signing){
    const bool is_rsa=EVP_PKEY_is_a(key,"RSA");
    require(is_rsa || EVP_PKEY_is_a(key,"EC"),unsupported);
    require(input_size!=0);
    if(is_rsa){
        require(options.padding==2 || options.padding==8,bad_flags);
        if(options.digest)require(input_size==static_cast<std::size_t>(EVP_MD_get_size(options.digest)));
        if(options.padding==8){
            require(options.digest!=nullptr);
            const auto encoded_size=static_cast<unsigned>((EVP_PKEY_get_bits(key)-1+7)/8);
            require(input_size+2<=encoded_size && options.salt<=encoded_size-input_size-2);
        }else if(!options.digest)require(input_size+11<=static_cast<std::size_t>(EVP_PKEY_get_size(key)));
    }else require(!options.padding && !options.digest,bad_flags);
    Context ctx(EVP_PKEY_CTX_new(key,nullptr),EVP_PKEY_CTX_free);require(bool(ctx),no_memory);
    require((signing?EVP_PKEY_sign_init(ctx.get()):EVP_PKEY_verify_init(ctx.get()))>0,bad_data);
    if(is_rsa){
        require(EVP_PKEY_CTX_set_rsa_padding(ctx.get(),options.padding==8?RSA_PKCS1_PSS_PADDING:RSA_PKCS1_PADDING)>0,bad_data);
        if(options.digest)require(EVP_PKEY_CTX_set_signature_md(ctx.get(),options.digest)>0,bad_data);
        if(options.padding==8)require(EVP_PKEY_CTX_set_rsa_pss_saltlen(ctx.get(),static_cast<int>(options.salt))>0,bad_data);
    }
    return ctx;
}
}
unsigned signature_size(EVP_PKEY* key){
    require(EVP_PKEY_is_a(key,"RSA") || EVP_PKEY_is_a(key,"EC"),unsupported);
    const auto width=static_cast<unsigned>((EVP_PKEY_get_bits(key)+7)/8);
    return EVP_PKEY_is_a(key,"RSA")?width:2*width;
}
Bytes sign(EVP_PKEY* key,std::span<const std::uint8_t> input,const SignatureOptions& options,bool size_only){
    auto ctx=signature_context(key,input.size(),options,true);
    // 長度查詢及短緩衝區不執行私鑰運算，也不消耗 ECDSA／PSS 隨機數。
    if(size_only)return Bytes(signature_size(key));
    std::size_t n=0;require(EVP_PKEY_sign(ctx.get(),nullptr,&n,input.data(),input.size())>0,bad_data);
    Bytes bytes(n);require(EVP_PKEY_sign(ctx.get(),bytes.data(),&n,input.data(),input.size())>0,bad_data);bytes.resize(n);
    if(EVP_PKEY_is_a(key,"RSA"))return bytes;
    const auto* p=bytes.data();std::unique_ptr<ECDSA_SIG,decltype(&ECDSA_SIG_free)> sig(d2i_ECDSA_SIG(nullptr,&p,static_cast<long>(bytes.size())),ECDSA_SIG_free);
    require(sig && p==bytes.data()+bytes.size(),bad_data);
    const auto width=signature_size(key)/2;auto r=number(ECDSA_SIG_get0_r(sig.get()),width),s=number(ECDSA_SIG_get0_s(sig.get()),width);
    r.insert(r.end(),s.begin(),s.end());return r;
}
void verify(EVP_PKEY* key,std::span<const std::uint8_t> input,std::span<const std::uint8_t> signature,const SignatureOptions& options){
    auto ctx=signature_context(key,input.size(),options,false);
    require(signature.size()==signature_size(key),bad_signature);
    Bytes encoded;
    if(EVP_PKEY_is_a(key,"EC")){
        const auto width=signature.size()/2;
        Big r(BN_bin2bn(signature.data(),static_cast<int>(width),nullptr),BN_clear_free),s(BN_bin2bn(signature.data()+width,static_cast<int>(width),nullptr),BN_clear_free);
        std::unique_ptr<ECDSA_SIG,decltype(&ECDSA_SIG_free)> sig(ECDSA_SIG_new(),ECDSA_SIG_free);require(r && s && sig,no_memory);
        require(ECDSA_SIG_set0(sig.get(),r.get(),s.get())==1,bad_data);(void)r.release();(void)s.release();
        const auto n=i2d_ECDSA_SIG(sig.get(),nullptr);require(n>0,bad_data);encoded.resize(static_cast<std::size_t>(n));auto* p=encoded.data();
        require(i2d_ECDSA_SIG(sig.get(),&p)==n,bad_data);signature=encoded;
    }
    require(EVP_PKEY_verify(ctx.get(),signature.data(),signature.size(),input.data(),input.size())==1,bad_signature);
}
Bytes crypt(EVP_PKEY* key,std::span<const std::uint8_t> input,const CipherOptions& options,bool encrypt,bool size_only){
    require(EVP_PKEY_is_a(key,"RSA"),unsupported);
    require(options.padding==1 || options.padding==2 || options.padding==4,bad_flags);
    const auto width=static_cast<std::size_t>(EVP_PKEY_get_size(key));
    require(encrypt?input.size()<=width:input.size()==width);
    if(options.padding==1)require(input.size()==width);
    if(options.padding==2 && encrypt)require(input.size()+11<=width);
    if(options.padding==4){require(options.digest!=nullptr);if(encrypt)require(input.size()+2ULL*static_cast<unsigned>(EVP_MD_get_size(options.digest))+2<=width);}
    else require(!options.digest && options.label.empty());
    Context ctx(EVP_PKEY_CTX_new(key,nullptr),EVP_PKEY_CTX_free);require(bool(ctx),no_memory);
    require((encrypt?EVP_PKEY_encrypt_init(ctx.get()):EVP_PKEY_decrypt_init(ctx.get()))>0,bad_data);
    require(EVP_PKEY_CTX_set_rsa_padding(ctx.get(),options.padding==1?RSA_NO_PADDING:options.padding==2?RSA_PKCS1_PADDING:RSA_PKCS1_OAEP_PADDING)>0,bad_data);
    if(options.padding==4){
        require(EVP_PKEY_CTX_set_rsa_oaep_md(ctx.get(),options.digest)>0 && EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.get(),options.digest)>0,bad_data);
        if(!options.label.empty()){
            auto* copy=static_cast<unsigned char*>(OPENSSL_memdup(options.label.data(),options.label.size()));require(copy!=nullptr,no_memory);
            if(EVP_PKEY_CTX_set0_rsa_oaep_label(ctx.get(),copy,static_cast<int>(options.label.size()))<=0){OPENSSL_free(copy);throw Error{bad_data};}
        }
    }
    if(encrypt && size_only)return Bytes(width);
    std::size_t n=0;
    require((encrypt?EVP_PKEY_encrypt(ctx.get(),nullptr,&n,input.data(),input.size()):EVP_PKEY_decrypt(ctx.get(),nullptr,&n,input.data(),input.size()))>0,bad_data);
    Bytes bytes(n);require((encrypt?EVP_PKEY_encrypt(ctx.get(),bytes.data(),&n,input.data(),input.size()):EVP_PKEY_decrypt(ctx.get(),bytes.data(),&n,input.data(),input.size()))>0,bad_data);
    bytes.resize(n);return bytes;
}
Bytes agreement(EVP_PKEY* key,EVP_PKEY* peer){
    Context ctx(EVP_PKEY_CTX_new(key,nullptr),EVP_PKEY_CTX_free);require(bool(ctx),no_memory);
    require(EVP_PKEY_derive_init(ctx.get())>0 && EVP_PKEY_derive_set_peer(ctx.get(),peer)>0,bad_data);
    std::size_t n=0;require(EVP_PKEY_derive(ctx.get(),nullptr,&n)>0,bad_data);Bytes bytes(n);
    require(EVP_PKEY_derive(ctx.get(),bytes.data(),&n)>0,bad_data);bytes.resize(n);
    // BCRYPT_KDF_RAW_SECRET 與既有 DH 路徑皆回傳 little-endian 整數。
    std::reverse(bytes.begin(),bytes.end());return bytes;
}
}
