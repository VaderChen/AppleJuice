#include "certificate_pfx.hpp"
#include "certificate_packet.hpp"
#include "cng_asymmetric.hpp"
#include <openssl/pkcs12.h>
#include <openssl/err.h>
#include <functional>
#include <map>
#include <charconv>

namespace x64arm::certificate_detail {
PfxKey::~PfxKey(){OPENSSL_cleanse(der.data(),der.size());}
namespace {
void require_pfx(bool valid,std::uint32_t error=0x8009310b){if(!valid)throw CertificateError{error};}
using Container=std::unique_ptr<PKCS12,decltype(&PKCS12_free)>;
Container decode(std::span<const std::uint8_t> input){
    require_pfx(!input.empty() && input.size()<=16U*1024U*1024U);
    const auto* p=input.data();Container result(d2i_PKCS12(nullptr,&p,static_cast<long>(input.size())),PKCS12_free);
    require_pfx(result && p==input.data()+input.size());return result;
}
std::u16string text_attribute(const PKCS12_SAFEBAG* bag,int id){
    const auto* value=PKCS12_SAFEBAG_get0_attr(bag,id);if(!value)return {};
    require_pfx(value->type==V_ASN1_BMPSTRING || value->type==V_ASN1_UTF8STRING || value->type==V_ASN1_PRINTABLESTRING);
    return certificate_string(value->value.asn1_string);
}
std::vector<std::uint8_t> public_identity(EVP_PKEY* key){
    require_pfx(key!=nullptr);
    // 依共用 CNG 公鑰數值比對，避免同一條 EC 曲線的 named／explicit DER 編碼差異。
    try{return cng::blob(key,cng::algorithm(key),static_cast<unsigned>(EVP_PKEY_get_bits(key)),"PUBLICBLOB",false);}
    catch(const cng::Error& error){if(error.code!=cng::unsupported)throw;return cng::der(key,false);}
}
void extended_properties(const PKCS12_SAFEBAG* bag,CertificateProperties& properties){
    const auto* attributes=PKCS12_SAFEBAG_get0_attrs(bag);constexpr std::string_view prefix="1.3.6.1.4.1.311.17.3.";
    for(int i=0;i<sk_X509_ATTRIBUTE_num(attributes);++i){
        auto* attribute=sk_X509_ATTRIBUTE_value(attributes,i);const auto oid=oid_name(X509_ATTRIBUTE_get0_object(attribute));if(!oid.starts_with(prefix))continue;
        std::uint32_t id=0;const auto suffix=std::string_view(oid).substr(prefix.size());const auto parsed=std::from_chars(suffix.data(),suffix.data()+suffix.size(),id);
        require_pfx(parsed.ec==std::errc{} && parsed.ptr==suffix.data()+suffix.size() && id && id<=65535);
        // 外部序列化資料不可帶入程序內的金鑰 handle 或 provider 結構。
        if(id==1 || id==2 || id==5 || id==6 || id==78 || id==79)continue;
        require_pfx(X509_ATTRIBUTE_count(attribute)==1);const auto* value=X509_ATTRIBUTE_get0_type(attribute,0);require_pfx(value && value->type==V_ASN1_OCTET_STRING);
        const auto length=ASN1_STRING_length(value->value.octet_string);require_pfx(length>=0);const auto* data=ASN1_STRING_get0_data(value->value.octet_string);
        if(id==11 || id==13 || id==21 || id==30)require_pfx(length>=2 && !(length&1) && !data[length-1] && !data[length-2]);
        require_pfx(properties.size()<1024,8);properties.insert_or_assign(id,std::vector<std::uint8_t>(data,data+length));
    }
}
}
bool is_pfx(std::span<const std::uint8_t> input){
    try{auto value=decode(input);return bool(value);}catch(const CertificateError&){ERR_clear_error();return false;}
}
PfxBundle read_pfx(std::span<const std::uint8_t> input,const char* password,bool extended,bool password_only){
    ERR_clear_error();auto pfx=decode(input);
    if(PKCS12_mac_present(pfx.get()))require_pfx(PKCS12_verify_mac(pfx.get(),password,password?-1:0)==1,86);
    const auto release_safes=[](STACK_OF(PKCS7)* p){sk_PKCS7_pop_free(p,PKCS7_free);};
    std::unique_ptr<STACK_OF(PKCS7),decltype(release_safes)> safes(PKCS12_unpack_authsafes(pfx.get()),release_safes);
    require_pfx(bool(safes));require_pfx(sk_PKCS7_num(safes.get())<=4096,8);
    PfxBundle result;std::vector<std::vector<std::uint8_t>> certificate_keys;
    std::map<std::vector<std::uint8_t>,std::size_t> keys;unsigned visited=0;
    std::function<void(const STACK_OF(PKCS12_SAFEBAG)*,unsigned)> walk;
    walk=[&](const STACK_OF(PKCS12_SAFEBAG)* bags,unsigned depth){
        require_pfx(bags && depth<16,8);
        for(int i=0;i<sk_PKCS12_SAFEBAG_num(bags);++i){
            require_pfx(++visited<=4096,8);const auto* bag=sk_PKCS12_SAFEBAG_value(bags,i);const auto type=PKCS12_SAFEBAG_get_nid(bag);
            if(type==NID_safeContentsBag){walk(PKCS12_SAFEBAG_get0_safes(bag),depth+1);continue;}
            if(type==NID_certBag){
                if(password_only)continue;
                require_pfx(PKCS12_SAFEBAG_get_bag_nid(bag)==NID_x509Certificate,50);
                std::unique_ptr<X509,decltype(&X509_free)> cert(PKCS12_SAFEBAG_get1_cert(bag),X509_free);require_pfx(bool(cert));
                PfxCertificate value;value.der=encoded(cert.get(),i2d_X509);const auto friendly=text_attribute(bag,NID_friendlyName);
                if(!friendly.empty())value.properties.emplace(11,utf16_bytes(friendly));
                if(extended)extended_properties(bag,value.properties);
                certificate_keys.push_back(public_identity(X509_get0_pubkey(cert.get())));result.certificates.push_back(std::move(value));continue;
            }
            if(password_only && type!=NID_keyBag && type!=NID_pkcs8ShroudedKeyBag)continue;
            require_pfx(type==NID_keyBag || type==NID_pkcs8ShroudedKeyBag,50);
            std::unique_ptr<PKCS8_PRIV_KEY_INFO,decltype(&PKCS8_PRIV_KEY_INFO_free)> decrypted(nullptr,PKCS8_PRIV_KEY_INFO_free);
            const PKCS8_PRIV_KEY_INFO* info=nullptr;
            if(type==NID_pkcs8ShroudedKeyBag){decrypted.reset(PKCS12_decrypt_skey(bag,password,password?-1:0));require_pfx(bool(decrypted),86);info=decrypted.get();}
            else info=PKCS12_SAFEBAG_get0_p8inf(bag);
            if(password_only)continue;
            require_pfx(info!=nullptr);cng::Key key(EVP_PKCS82PKEY(info),EVP_PKEY_free);require_pfx(bool(key));cng::validate(key.get(),true);
            const auto pub=public_identity(key.get());PfxKey value;value.algorithm=cng::algorithm(key.get());value.bits=static_cast<std::uint32_t>(EVP_PKEY_get_bits(key.get()));
            value.der=cng::der(key.get(),true);value.provider=utf16_to_utf8(text_attribute(bag,NID_ms_csp_name));
            value.name=utf16_to_utf8(text_attribute(bag,NID_friendlyName));require_pfx(value.name.size()<=1024 && value.name.find('\0')==value.name.npos,87);
            value.machine=PKCS12_SAFEBAG_get0_attr(bag,NID_LocalKeySet)!=nullptr;
            const ASN1_TYPE* usage=PKCS12_SAFEBAG_get0_attr(bag,NID_key_usage);
            if(!usage)usage=PKCS12_get_attr_gen(PKCS8_pkey_get0_attrs(info),NID_key_usage);
            if(usage){
                require_pfx(usage->type==V_ASN1_BIT_STRING);const auto bit=[&](int n){return ASN1_BIT_STRING_get_bit(usage->value.bit_string,n)!=0;};
                value.usage=(bit(2)||bit(3)||bit(7)||bit(8)?1U:0U)|(bit(0)||bit(5)||bit(6)?2U:0U)|(bit(4)?4U:0U);
                value.spec=bit(2)||bit(3)?1U:2U;
            }
            if(const auto prior=keys.find(pub);prior!=keys.end()){
                const auto& old=result.keys[prior->second];require_pfx(old.der==value.der && old.provider==value.provider && old.name==value.name && old.spec==value.spec && old.usage==value.usage && old.machine==value.machine);
            }else {keys.emplace(pub,result.keys.size());result.keys.push_back(std::move(value));}
        }
    };
    try{
        for(int i=0;i<sk_PKCS7_num(safes.get());++i){
            auto* safe=sk_PKCS7_value(safes.get(),i);const bool encrypted=PKCS7_type_is_encrypted(safe);
            require_pfx(encrypted || PKCS7_type_is_data(safe),50);
            const auto release=[](STACK_OF(PKCS12_SAFEBAG)* p){sk_PKCS12_SAFEBAG_pop_free(p,PKCS12_SAFEBAG_free);};
            std::unique_ptr<STACK_OF(PKCS12_SAFEBAG),decltype(release)> bags(encrypted?PKCS12_unpack_p7encdata(safe,password,password?-1:0):PKCS12_unpack_p7data(safe),release);
            require_pfx(bool(bags),encrypted?86U:0x8009310bU);walk(bags.get(),0);
        }
        std::vector<bool> used(result.keys.size());
        for(std::size_t i=0;i<result.certificates.size();++i)if(const auto key=keys.find(certificate_keys[i]);key!=keys.end()){result.certificates[i].key=key->second;used[key->second]=true;}
        require_pfx(std::all_of(used.begin(),used.end(),[](bool value){return value;}),0x80090015);
    }catch(const cng::Error& e){throw CertificateError{e.code};}
    return result;
}
}
