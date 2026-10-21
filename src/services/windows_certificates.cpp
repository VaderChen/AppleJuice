#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "certificate_packet.hpp"
#include "certificate_store.hpp"
#include "sandbox_certificate_roots.hpp"
#include "certificate_pfx.hpp"
#include "certificate_keys.hpp"
#include "ncrypt_key_store.hpp"
#include "x64arm/private_key_store.hpp"
#include <algorithm>
#include <chrono>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pkcs7.h>
#include <set>
#include <cstdlib>
#include <filesystem>
#include "idn_conversion.hpp"
namespace x64arm {
namespace {
using namespace certificate_detail;
using Certificate=std::unique_ptr<X509,decltype(&X509_free)>;
Certificate decode_certificate(const std::vector<std::uint8_t>& der){
    const auto* cursor=der.data();Certificate value(d2i_X509(nullptr,&cursor,static_cast<long>(der.size())),X509_free);
    if(!value || cursor!=der.data()+der.size()){ERR_clear_error();throw CertificateError{0x8009310b};}return value;
}
using RevocationList=std::unique_ptr<X509_CRL,decltype(&X509_CRL_free)>;
RevocationList decode_crl(const std::vector<std::uint8_t>& der){
    const auto* cursor=der.data();RevocationList value(d2i_X509_CRL(nullptr,&cursor,static_cast<long>(der.size())),X509_CRL_free);
    if(!value || cursor!=der.data()+der.size()){ERR_clear_error();throw CertificateError{0x8009310b};}return value;
}
using KeyUsages=std::vector<std::string>;
using ExtendedUsage=std::unique_ptr<EXTENDED_KEY_USAGE,decltype(&EXTENDED_KEY_USAGE_free)>;
KeyUsages usage_identifiers(const EXTENDED_KEY_USAGE* usage){
    KeyUsages result;const auto count=sk_ASN1_OBJECT_num(usage);if(count<0 || count>4096)throw CertificateError{8};
    for(int i=0;i<count;++i)result.push_back(oid_name(sk_ASN1_OBJECT_value(usage,i)));return result;
}
KeyUsages decode_usage(const std::vector<std::uint8_t>& der){
    const auto* cursor=der.data();ExtendedUsage usage(d2i_EXTENDED_KEY_USAGE(nullptr,&cursor,static_cast<long>(der.size())),EXTENDED_KEY_USAGE_free);
    if(!usage || cursor!=der.data()+der.size()){ERR_clear_error();throw CertificateError{0x8009310b};}return usage_identifiers(usage.get());
}
std::vector<std::uint8_t> encode_usage(const KeyUsages& identifiers){
    ExtendedUsage usage(sk_ASN1_OBJECT_new_null(),EXTENDED_KEY_USAGE_free);if(!usage)throw CertificateError{8};
    for(const auto& identifier:identifiers){
        std::unique_ptr<ASN1_OBJECT,decltype(&ASN1_OBJECT_free)> object(OBJ_txt2obj(identifier.c_str(),1),ASN1_OBJECT_free);
        if(!object){ERR_clear_error();throw CertificateError{0x8009310b};}if(!sk_ASN1_OBJECT_push(usage.get(),object.get()))throw CertificateError{8};object.release();
    }
    return encoded(usage.get(),i2d_EXTENDED_KEY_USAGE);
}
std::optional<KeyUsages> certificate_usages(const CertificateContext& context,const CertificateProperties& properties,std::uint32_t flags){
    if(flags&~6U || flags==6)throw CertificateError{87};
    std::optional<KeyUsages> extension,property;
    if(!(flags&4U)){
        auto certificate=decode_certificate(context.der);int critical=-1;
        ExtendedUsage usage(static_cast<EXTENDED_KEY_USAGE*>(X509_get_ext_d2i(certificate.get(),NID_ext_key_usage,&critical,nullptr)),EXTENDED_KEY_USAGE_free);
        if(usage)extension=usage_identifiers(usage.get());else if(critical!=-1){ERR_clear_error();throw CertificateError{0x8009310b};}
    }
    if(!(flags&2U))if(const auto found=properties.find(9);found!=properties.end())property=decode_usage(found->second);
    if(flags && !extension && !property)throw CertificateError{0x80092004};
    if(extension && property)std::erase_if(*extension,[&](const auto& oid){return std::find(property->begin(),property->end(),oid)==property->end();});
    return extension?extension:property;
}
CertificatePacket usage_packet(bool x86,const KeyUsages& usages,bool header){
    CertificatePacket packet(x86);const auto unit=packet.unit;
    if(header){packet.reserve(unit*2);packet.put(0,usages.size());}
    if(!usages.empty()){
        const auto table=packet.reserve(usages.size()*unit,unit);if(header)packet.point(unit,table);
        for(std::size_t i=0;i<usages.size();++i)packet.text(table+i*unit,usages[i]);
    }
    return packet;
}

std::vector<Certificate> certificate_roots(ReadOnlyFiles& files){
    const auto roots=sandbox_certificate_roots(files);if(roots.error)throw CertificateError{roots.error};
    std::unique_ptr<X509_STORE,decltype(&X509_STORE_free)> store(X509_STORE_new(),X509_STORE_free);
    if(!store || !add_certificate_root_pem(store.get(),roots.pem))throw CertificateError{8};
    const auto release=[](STACK_OF(X509)* stack){sk_X509_pop_free(stack,X509_free);};
    std::unique_ptr<STACK_OF(X509),decltype(release)> all(X509_STORE_get1_all_certs(store.get()),release);std::vector<Certificate> result;
    if(all){if(sk_X509_num(all.get())>8192)throw CertificateError{8};for(int i=0;i<sk_X509_num(all.get());++i){auto* certificate=sk_X509_value(all.get(),i);if(!X509_up_ref(certificate))throw CertificateError{8};result.emplace_back(certificate,X509_free);}}
    return result;
}
void resync_store(CertificateState& state,std::uint64_t handle,ReadOnlyFiles& files){
    auto& store=state.stores.at(handle);
    if(store.collection){for(const auto& sibling:store.siblings)resync_store(state,sibling.store,files);return;}
    if(!store.root_provider)throw CertificateError{50};
    // 重新讀取同一個持久化來源；舊 context 的 DER／屬性仍由自己的引用保留。
    auto roots=certificate_roots(files);std::map<std::uint64_t,std::vector<std::uint8_t>> items;std::map<std::uint64_t,std::uint64_t> properties;
    auto next=store.next_identity;std::vector<std::uint64_t> created;
    try {for(const auto& root:roots){auto der=encoded(root.get(),i2d_X509);const auto existing=std::find_if(store.items.begin(),store.items.end(),[&](const auto& item){return item.second==der;});
        const auto identity=existing==store.items.end()?next++:existing->first;std::uint64_t property;
        if(existing==store.items.end()){property=state.next_property_identity++;created.push_back(property);state.properties.emplace(property,CertificateProperties{});}else property=store.property_identities.at(identity);
        items.emplace(identity,std::move(der));properties.emplace(identity,property);
    }}catch(...){for(const auto property:created)state.properties.erase(property);throw;}
    store.items.swap(items);store.property_identities.swap(properties);store.next_identity=next;
}
void auto_resync_store(CertificateState& state,std::uint64_t handle,ReadOnlyFiles& files){
    auto& store=state.stores.at(handle);if(store.auto_resync)resync_store(state,handle,files);
    else for(const auto& sibling:store.siblings)auto_resync_store(state,sibling.store,files);
}
std::vector<std::uint8_t> certificate_tlv(std::uint8_t tag,std::span<const std::uint8_t> contents){
    std::vector<std::uint8_t> output{tag};if(contents.size()<128)output.push_back(static_cast<std::uint8_t>(contents.size()));else {std::vector<std::uint8_t> length;for(auto count=contents.size();count;count>>=8)length.push_back(static_cast<std::uint8_t>(count));output.push_back(static_cast<std::uint8_t>(128+length.size()));output.insert(output.end(),length.rbegin(),length.rend());}output.insert(output.end(),contents.begin(),contents.end());return output;
}
std::vector<std::uint8_t> asn1_content(std::vector<std::uint8_t> bytes){
    if(bytes.size()<2)throw CertificateError{0x8009310b};std::size_t start=2,length=bytes[1];
    if(length&0x80){const auto count=length&0x7f;if(!count || count>sizeof(std::size_t) || count>bytes.size()-2)throw CertificateError{0x8009310b};start+=count;length=0;for(std::size_t i=0;i<count;++i)length=(length<<8)|bytes[2+i];}
    if(length!=bytes.size()-start)throw CertificateError{0x8009310b};bytes.erase(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(start));return bytes;
}

std::optional<std::u16string> certificate_attribute(X509_NAME* name,std::string_view oid,bool legacy){
    for(int i=0;i<X509_NAME_entry_count(name);++i){auto* entry=X509_NAME_get_entry(name,i);if(oid_name(X509_NAME_ENTRY_get_object(entry))==oid)return certificate_string(X509_NAME_ENTRY_get_data(entry),legacy);}return std::nullopt;
}
#include "certificate_names.inc"
std::u16string certificate_rdn(X509_NAME* name,std::uint32_t flags){
    const auto format=flags&65535;if(format<1 || format>3)throw CertificateError{87};if(flags&~0x7a010003U)throw CertificateError{50};
    const auto label=[](const std::string& oid){
        for(const auto& item:certificate_name_labels)if(item.first==oid)return std::string(item.second);return "OID."+oid;
    };
    std::vector<std::u16string> groups;int previous=-1;
    for(int i=0;i<X509_NAME_entry_count(name);++i){auto* entry=X509_NAME_get_entry(name,i);const auto group=X509_NAME_ENTRY_set(entry);auto* value=X509_NAME_ENTRY_get_data(entry);std::u16string text;
        const auto oid=oid_name(X509_NAME_ENTRY_get_object(entry));if(format!=1)text=utf8_to_utf16_lossy(format==2?oid:label(oid))+u"=";
        if(ASN1_STRING_type(value)==V_ASN1_OCTET_STRING){static constexpr char16_t hex[]=u"0123456789abcdef";for(int j=0;j<ASN1_STRING_length(value);++j){const auto c=ASN1_STRING_get0_data(value)[j];text.push_back(hex[c>>4]);text.push_back(hex[c&15]);}}
        else {const auto decoded=certificate_string(value,(flags&0x10000U)!=0);const auto space=[](char16_t c){return c==u' ' || (c>=u'\t' && c<=u'\r');};
            const bool quoted=!(flags&0x10000000U) && (decoded.empty() || space(decoded.front()) || space(decoded.back()) || decoded.find_first_of(u",+=\"\\\n\r<>#;")!=decoded.npos);
            if(quoted)text+=u'"';for(auto c:decoded){text+=c;if(quoted && c==u'"')text+=c;}if(quoted)text+=u'"';}
        if(groups.empty() || previous!=group)groups.push_back(std::move(text));else {groups.back()+=(flags&0x20000000U)?u" ":format==1?u" + ":u"+ ";groups.back()+=text;}previous=group;
    }
    if(flags&0x02000000U)std::reverse(groups.begin(),groups.end());std::u16string output;
    for(const auto& group:groups){if(!output.empty())output+=(flags&0x08000000U)?u"\r\n":(flags&0x40000000U)?u"; ":u", ";output+=group;}return output;
}
std::u16string certificate_name(X509* cert,std::uint32_t type,std::uint32_t flags,std::string_view attribute,std::uint32_t format){
    if(flags&~0x10003U)throw CertificateError{50};if(type<1 || type>8)throw CertificateError{87};if((flags&2U) && type!=6)return {};
    const bool issuer=(flags&1U)!=0,legacy=(flags&0x10000U)!=0;auto* name=issuer?X509_get_issuer_name(cert):X509_get_subject_name(cert);
    int critical=-1;std::unique_ptr<GENERAL_NAMES,decltype(&GENERAL_NAMES_free)> alternatives(static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert,issuer?NID_issuer_alt_name:NID_subject_alt_name,&critical,nullptr)),GENERAL_NAMES_free);
    if(!alternatives && critical!=-1)throw CertificateError{0x8009310b};
    std::vector<std::u16string> emails,dns,urls,upns;X509_NAME* directory=nullptr;
    for(int i=0;alternatives && i<sk_GENERAL_NAME_num(alternatives.get());++i){const auto* entry=sk_GENERAL_NAME_value(alternatives.get(),i);
        if(entry->type==GEN_EMAIL)emails.push_back(certificate_string(entry->d.rfc822Name));else if(entry->type==GEN_DNS)dns.push_back(certificate_string(entry->d.dNSName));
        else if(entry->type==GEN_URI)urls.push_back(certificate_string(entry->d.uniformResourceIdentifier));else if(entry->type==GEN_DIRNAME && !directory)directory=entry->d.directoryName;
        else if(entry->type==GEN_OTHERNAME && oid_name(entry->d.otherName->type_id)=="1.3.6.1.4.1.311.20.2.3"){
            const auto* value=entry->d.otherName->value;switch(value->type){case V_ASN1_UTF8STRING:case V_ASN1_BMPSTRING:case V_ASN1_UNIVERSALSTRING:case V_ASN1_IA5STRING:case V_ASN1_PRINTABLESTRING:case V_ASN1_T61STRING:upns.push_back(certificate_string(value->value.asn1_string,legacy));break;default:throw CertificateError{0x8009310b};}}
    }
    auto* selected=X509_NAME_entry_count(name)==0 && directory?directory:name;
    const auto attr=[&](std::string_view oid){return certificate_attribute(selected,oid,legacy);};
    if(type==1){if(!emails.empty())return emails.front();return certificate_attribute(name,"1.2.840.113549.1.9.1",legacy).value_or(u"");}
    if(type==2)return certificate_rdn(selected,format|(legacy?0x10000U:0U));if(type==3)return attr(attribute).value_or(u"");
    if(type==4 || type==5){for(const auto* oid:{"2.5.4.3","2.5.4.11","2.5.4.10","1.2.840.113549.1.9.1"})if(auto value=attr(oid))return *value;
        if(!emails.empty())return emails.front();if(X509_NAME_entry_count(selected))return certificate_string(X509_NAME_ENTRY_get_data(X509_NAME_get_entry(selected,0)),legacy);return {};}
    if(type==7)return urls.empty()?u"":urls.front();if(type==8)return upns.empty()?u"":upns.front();
    if(dns.empty()){if(auto common=certificate_attribute(name,"2.5.4.3",legacy))dns.push_back(*common);}if(dns.empty())return {};if(!(flags&2U))return dns.front();
    std::u16string output;for(const auto& item:dns){output+=item;output.push_back(0);}return output;
}

bool certificate_blob_property(std::uint32_t id){
    if(id>=0x8000 && id<=0xffff)return true;
    switch(id){case 3:case 4:case 9:case 11:case 12:case 13:case 15:case 19:case 20:case 21:case 23:case 24:case 25:case 26:case 28:case 29:case 30:case 64:case 65:case 70:case 76:case 80:case 81:case 82:case 107:return true;default:return false;}
}
std::vector<std::uint8_t> certificate_digest(std::span<const std::uint8_t> input,const EVP_MD* algorithm){
    if(!algorithm)throw CertificateError{50};std::vector<std::uint8_t> output(EVP_MAX_MD_SIZE);unsigned count=0;
    if(!EVP_Digest(input.data(),input.size(),output.data(),&count,algorithm,nullptr))throw CertificateError{0x80090020};output.resize(count);return output;
}
std::optional<std::vector<std::uint8_t>> certificate_computed_property(const CertificateContext& context,std::uint32_t id,bool crl=false){
    if(id==14)return std::vector<std::uint8_t>(4); // 目前只有不持久化的 memory store。
    if(id==3 || id==4 || id==107)return certificate_digest(context.der,id==3?EVP_sha1():id==4?EVP_md5():EVP_sha256());
    if(crl){if(id!=15)return std::nullopt;auto value=decode_crl(context.der);int digest_nid=0;
        if(!OBJ_find_sigid_algs(X509_CRL_get_signature_nid(value.get()),&digest_nid,nullptr))throw CertificateError{50};
        return certificate_digest(encoded(value.get(),i2d_re_X509_CRL_tbs),EVP_get_digestbynid(digest_nid));}
    if(id!=15 && id!=20 && id!=25 && id!=29 && id!=92 && id!=93)return std::nullopt;auto cert=decode_certificate(context.der);
    if(id==15){int digest_nid=0;if(!X509_get_signature_info(cert.get(),&digest_nid,nullptr,nullptr,nullptr))throw CertificateError{50};return certificate_digest(encoded(cert.get(),i2d_re_X509_tbs),EVP_get_digestbynid(digest_nid));}
    if(id==29)return certificate_digest(encoded(X509_get_subject_name(cert.get()),i2d_X509_NAME),EVP_md5());
    if(id==25)return certificate_digest(encoded(X509_get_X509_PUBKEY(cert.get()),i2d_X509_PUBKEY),EVP_md5());
    if(id==20){int critical=-1;std::unique_ptr<ASN1_OCTET_STRING,decltype(&ASN1_OCTET_STRING_free)> identifier(static_cast<ASN1_OCTET_STRING*>(X509_get_ext_d2i(cert.get(),NID_subject_key_identifier,&critical,nullptr)),ASN1_OCTET_STRING_free);
        if(identifier){const auto* data=ASN1_STRING_get0_data(identifier.get());return std::vector<std::uint8_t>(data,data+ASN1_STRING_length(identifier.get()));}
        if(critical!=-1)throw CertificateError{0x8009310b};return certificate_digest(encoded(X509_get_X509_PUBKEY(cert.get()),i2d_X509_PUBKEY),EVP_sha1());}
    auto* key=X509_get0_pubkey(cert.get());if(!key)throw CertificateError{0x8009310b};const auto length=EVP_PKEY_get_bits(key);if(length<=0)throw CertificateError{0x8009310b};
    if(id==92){std::vector<std::uint8_t> bytes(4);for(unsigned i=0;i<4;++i)bytes[i]=static_cast<std::uint8_t>(static_cast<unsigned>(length)>>(i*8));return bytes;}
    const auto type=EVP_PKEY_get_base_id(key);const char* algorithm=type==EVP_PKEY_RSA || type==EVP_PKEY_RSA_PSS?"RSA":type==EVP_PKEY_EC?"ECDSA":type==EVP_PKEY_DSA?"DSA":nullptr;
    if(!algorithm)throw CertificateError{50};return utf16_bytes(utf8_to_utf16_lossy(std::string(algorithm)+std::to_string(length)));
}

using SignedMessage=std::unique_ptr<PKCS7,decltype(&PKCS7_free)>;
#include "certificate_store_save.inc"
SignedMessage decode_message(std::span<const std::uint8_t> bytes){
    if(bytes.empty())throw CertificateError{0x8009310b};
    const auto* cursor=bytes.data();SignedMessage message(d2i_PKCS7(nullptr,&cursor,static_cast<long>(bytes.size())),PKCS7_free);
    if(!message || cursor!=bytes.data()+bytes.size()){ERR_clear_error();throw CertificateError{0x8009310b};}
    if(!PKCS7_type_is_data(message.get()) && !PKCS7_type_is_signed(message.get()))throw CertificateError{50};
    return message;
}
std::vector<std::uint8_t> message_content(PKCS7* message){
    auto* inner=PKCS7_type_is_signed(message)?message->d.sign->contents:message;
    if(!inner)return {};
    if(PKCS7_type_is_data(inner)){if(!inner->d.data)return {};const auto* data=ASN1_STRING_get0_data(inner->d.data);const auto size=ASN1_STRING_length(inner->d.data);if(size<0)throw CertificateError{0x8009310b};if(!size)return {};return {data,data+size};}
    // PKCS7 的 d 是 tagged union；已知的巢狀訊息不能當作 ASN1_TYPE 讀取。
    switch(OBJ_obj2nid(inner->type)){
    case NID_pkcs7_signed:case NID_pkcs7_enveloped:case NID_pkcs7_signedAndEnveloped:case NID_pkcs7_digest:case NID_pkcs7_encrypted:throw CertificateError{50};
    default:if(inner->d.other)return encoded(inner->d.other,i2d_ASN1_TYPE);
    }
    throw CertificateError{50};
}
void message_attributes(CertificatePacket& packet,std::size_t field,const STACK_OF(X509_ATTRIBUTE)* attributes){
    const auto count=attributes?sk_X509_ATTRIBUTE_num(attributes):0;if(count<0 || count>4096)throw CertificateError{8};
    packet.put(field,static_cast<unsigned>(count));if(!count)return;
    const auto array=packet.reserve(static_cast<std::size_t>(count)*3*packet.unit,packet.unit);packet.point(field+packet.unit,array);
    for(int i=0;i<count;++i){auto* attribute=sk_X509_ATTRIBUTE_value(attributes,i);const auto at=array+static_cast<std::size_t>(i)*3*packet.unit;
        packet.text(at,oid_name(X509_ATTRIBUTE_get0_object(attribute)));const auto values=X509_ATTRIBUTE_count(attribute);if(values<0 || values>4096)throw CertificateError{8};packet.put(at+packet.unit,static_cast<unsigned>(values));
        if(values){const auto blobs=packet.reserve(static_cast<std::size_t>(values)*2*packet.unit,packet.unit);packet.point(at+2*packet.unit,blobs);for(int n=0;n<values;++n)packet.blob(blobs+static_cast<std::size_t>(n)*2*packet.unit,encoded(X509_ATTRIBUTE_get0_type(attribute,n),i2d_ASN1_TYPE));}
    }
}
CertificatePacket message_parameter(bool x86,PKCS7* message,std::uint32_t kind,std::uint32_t index){
    CertificatePacket packet(x86);const auto unit=packet.unit;const bool signed_data=PKCS7_type_is_signed(message);
    const auto value=[&](std::uint32_t number){packet.reserve(4);packet.put(0,number);};
    const auto* signers=signed_data?message->d.sign->signer_info:nullptr;
    const auto signer_count=signers?sk_PKCS7_SIGNER_INFO_num(signers):0;
    if(kind==1){value(signed_data?2U:1U);return packet;}
    if(kind==2){packet.data(message_content(message));return packet;}
    if(kind==29){packet.data(encoded(message,i2d_PKCS7));return packet;}
    if(kind==4){auto* inner=signed_data?message->d.sign->contents:message;const auto name=oid_name(inner->type);packet.bytes.assign(name.begin(),name.end());packet.bytes.push_back(0);return packet;}
    if(!signed_data)throw CertificateError{0x80091004};
    if(kind==5){value(static_cast<unsigned>(signer_count));return packet;}
    if(kind==30){value(static_cast<unsigned>(ASN1_INTEGER_get(message->d.sign->version)));return packet;}
    const auto* certificates=message->d.sign->cert;const auto* crls=message->d.sign->crl;
    const auto certificate_count=certificates?sk_X509_num(certificates):0,crl_count=crls?sk_X509_CRL_num(crls):0;
    if(kind==11 || kind==13){value(static_cast<unsigned>(kind==11?certificate_count:crl_count));return packet;}
    if(kind==12 || kind==14){if(index>=static_cast<unsigned>(kind==12?certificate_count:crl_count))throw CertificateError{0x80091008};packet.data(kind==12?encoded(sk_X509_value(certificates,static_cast<int>(index)),i2d_X509):encoded(sk_X509_CRL_value(crls,static_cast<int>(index)),i2d_X509_CRL));return packet;}
    if(kind<6 || kind>10)throw CertificateError{50};
    if(index>=static_cast<unsigned>(signer_count))throw CertificateError{0x80091008};auto* signer=sk_PKCS7_SIGNER_INFO_value(signers,static_cast<int>(index));
    if(kind==8){packet.reserve(3*unit);packet.algorithm(0,signer->digest_alg);return packet;}
    if(kind==9 || kind==10){packet.reserve(2*unit);message_attributes(packet,0,kind==9?signer->auth_attr:signer->unauth_attr);return packet;}
    auto serial=asn1_content(encoded(signer->issuer_and_serial->serial,i2d_ASN1_INTEGER));std::reverse(serial.begin(),serial.end());
    if(kind==7){packet.reserve(x86?112:208);packet.blob(unit,serial);packet.blob(6*unit,encoded(signer->issuer_and_serial->issuer,i2d_X509_NAME));return packet;}
    packet.reserve(17*unit);packet.put(0,static_cast<unsigned>(ASN1_INTEGER_get(signer->version)));
    packet.blob(unit,encoded(signer->issuer_and_serial->issuer,i2d_X509_NAME));packet.blob(3*unit,serial);
    packet.algorithm(5*unit,signer->digest_alg);packet.algorithm(8*unit,signer->digest_enc_alg);
    packet.blob(11*unit,std::span(ASN1_STRING_get0_data(signer->enc_digest),static_cast<std::size_t>(ASN1_STRING_length(signer->enc_digest))));
    message_attributes(packet,13*unit,signer->auth_attr);message_attributes(packet,15*unit,signer->unauth_attr);return packet;
}
std::vector<std::uint8_t> embedded_message(std::span<const std::uint8_t> bytes){
    const auto number=[&](std::size_t at,unsigned width){if(at>bytes.size() || width>bytes.size()-at)throw CertificateError{0x80092009};std::uint32_t value=0;for(unsigned i=0;i<width;++i)value|=std::uint32_t{bytes[at+i]}<<(8*i);return value;};
    if(number(0,2)!=0x5a4d)throw CertificateError{0x80092009};const auto pe=static_cast<std::size_t>(number(60,4));if(number(pe,4)!=0x4550)throw CertificateError{0x80092009};
    const auto optional=pe+24;const auto magic=number(optional,2);const unsigned directories=magic==0x10b?96U:magic==0x20b?112U:0U;
    if(!directories || number(pe+20,2)<directories+40 || number(optional+directories-4,4)<5)throw CertificateError{0x80092009};
    const auto begin=static_cast<std::size_t>(number(optional+directories+32,4)),size=static_cast<std::size_t>(number(optional+directories+36,4));
    if(!size || (begin&7) || begin>bytes.size() || size>bytes.size()-begin)throw CertificateError{0x80092009};
    for(auto at=begin;at<begin+size;){const auto length=static_cast<std::size_t>(number(at,4));if(length<8 || length>begin+size-at)throw CertificateError{0x80092009};
        if(number(at+4,2)==0x200 && number(at+6,2)==2)return {bytes.begin()+static_cast<std::ptrdiff_t>(at+8),bytes.begin()+static_cast<std::ptrdiff_t>(at+length)};
        at+=(length+7)&~std::size_t{7};
    }
    throw CertificateError{0x80092009};
}
CertificatePacket certificate_packet(bool x86,X509* cert,const std::vector<std::uint8_t>& der,std::uint64_t store){
    CertificatePacket packet(x86);const unsigned unit=packet.unit;packet.reserve(x86?20:40,unit);const auto info=packet.reserve(x86?112:208,unit);
    packet.put(0,1);packet.point(unit,packet.data(der));packet.put(2*unit,der.size());packet.point(3*unit,info);packet.put(4*unit,store,unit);
    packet.put(info,static_cast<std::uint64_t>(X509_get_version(cert)));
    auto serial=asn1_content(encoded(X509_get0_serialNumber(cert),i2d_ASN1_INTEGER));std::reverse(serial.begin(),serial.end());packet.blob(info+unit,serial);
    packet.algorithm(info+3*unit,X509_get0_tbs_sigalg(cert));packet.blob(info+6*unit,encoded(X509_get_issuer_name(cert),i2d_X509_NAME));
    packet.put(info+8*unit,certificate_time(X509_get0_notBefore(cert)),8);packet.put(info+8*unit+8,certificate_time(X509_get0_notAfter(cert)),8);
    packet.blob(info+8*unit+16,encoded(X509_get_subject_name(cert),i2d_X509_NAME));
    const auto public_key=info+10*unit+16;X509_ALGOR* algorithm{};if(!X509_PUBKEY_get0_param(nullptr,nullptr,nullptr,&algorithm,X509_get_X509_PUBKEY(cert)))throw CertificateError{0x8009310b};packet.algorithm(public_key,algorithm);packet.bits(public_key+3*unit,X509_get0_pubkey_bitstr(cert));
    const ASN1_BIT_STRING *issuer{},*subject{};X509_get0_uids(cert,&issuer,&subject);packet.bits(info+(x86?80:144),issuer);packet.bits(info+(x86?92:168),subject);
    packet.extensions(info+(x86?104:192),info+(x86?108:200),X509_get_ext_count(cert),[&](int i){return X509_get_ext(cert,i);});
    return packet;
}
CertificatePacket crl_packet(bool x86,X509_CRL* crl,const std::vector<std::uint8_t>& der,std::uint64_t store){
    CertificatePacket packet(x86);const unsigned unit=packet.unit;packet.reserve(5*unit,unit);const auto info=packet.reserve(x86?56:96,unit);
    packet.put(0,1);packet.point(unit,packet.data(der));packet.put(2*unit,der.size());packet.point(3*unit,info);packet.put(4*unit,store,unit);
    const auto version=X509_CRL_get_version(crl);if(version<0 || version>1)throw CertificateError{0x8009310b};packet.put(info,static_cast<unsigned>(version));
    const X509_ALGOR* algorithm{};X509_CRL_get0_signature(crl,nullptr,&algorithm);packet.algorithm(info+unit,algorithm);
    packet.blob(info+4*unit,encoded(X509_CRL_get_issuer(crl),i2d_X509_NAME));
    packet.put(info+6*unit,certificate_time(X509_CRL_get0_lastUpdate(crl)),8);
    if(const auto* next=X509_CRL_get0_nextUpdate(crl))packet.put(info+6*unit+8,certificate_time(next),8);
    auto* revoked=X509_CRL_get_REVOKED(crl);const auto count=revoked?sk_X509_REVOKED_num(revoked):0;
    if(count<0 || count>65536)throw CertificateError{8};packet.put(info+6*unit+16,static_cast<unsigned>(count));
    if(count){const auto size=x86?24U:40U;const auto table=packet.reserve(static_cast<std::size_t>(count)*size,unit);packet.point(info+(x86?44:72),table);
        for(int i=0;i<count;++i){auto* entry=sk_X509_REVOKED_value(revoked,i);const auto at=table+static_cast<std::size_t>(i)*size;
            auto serial=asn1_content(encoded(X509_REVOKED_get0_serialNumber(entry),i2d_ASN1_INTEGER));std::reverse(serial.begin(),serial.end());packet.blob(at,serial);
            packet.put(at+2*unit,certificate_time(X509_REVOKED_get0_revocationDate(entry)),8);
            packet.extensions(at+2*unit+8,at+(x86?20:32),X509_REVOKED_get_ext_count(entry),[&](int n){return X509_REVOKED_get_ext(entry,n);});}
    }
    packet.extensions(info+(x86?48:80),info+(x86?52:88),X509_CRL_get_ext_count(crl),[&](int i){return X509_CRL_get_ext(crl,i);});return packet;
}
void copy_store_crls(CertificateState& state,CertificateStore& store,const std::vector<std::vector<std::uint8_t>>& crls){
    if(crls.size()>4096)throw CertificateError{8};
    for(const auto& der:crls){(void)decode_crl(der);const auto identity=store.next_identity++,property=state.next_property_identity++;
        store.crls.emplace(identity,der);store.crl_property_identities.emplace(identity,property);state.properties.emplace(property,CertificateProperties{});}
}
std::vector<std::vector<std::uint8_t>> message_crls(PKCS7* message){
    std::vector<std::vector<std::uint8_t>> result;if(!PKCS7_type_is_signed(message) || !message->d.sign->crl)return result;
    const auto count=sk_X509_CRL_num(message->d.sign->crl);if(count<0 || count>4096)throw CertificateError{8};result.reserve(static_cast<std::size_t>(count));
    for(int i=0;i<count;++i)result.push_back(encoded(sk_X509_CRL_value(message->d.sign->crl,i),i2d_X509_CRL));return result;
}
std::uint32_t trust_error(int error){
    switch(error){
    case X509_V_OK:return 0;
    case X509_V_ERR_CERT_NOT_YET_VALID:case X509_V_ERR_CERT_HAS_EXPIRED:case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:return 1;
    case X509_V_ERR_CERT_REVOKED:return 4;
    case X509_V_ERR_INVALID_PURPOSE:return 0x10;
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:case X509_V_ERR_CERT_UNTRUSTED:return 0x20;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:return 0x10000;
    case X509_V_ERR_UNABLE_TO_GET_CRL:case X509_V_ERR_CRL_HAS_EXPIRED:case X509_V_ERR_CRL_NOT_YET_VALID:return 0x1000040;
    case X509_V_ERR_PATH_LENGTH_EXCEEDED:case X509_V_ERR_INVALID_CA:return 0x400;
    case X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION:return 0x8000000;
    case X509_V_ERR_INVALID_POLICY_EXTENSION:case X509_V_ERR_NO_EXPLICIT_POLICY:return 0x200;
    case X509_V_ERR_CERT_REJECTED:return 0x4000000;
    case X509_V_ERR_EE_KEY_TOO_SMALL:case X509_V_ERR_CA_KEY_TOO_SMALL:case X509_V_ERR_CA_MD_TOO_WEAK:return 0x100008;
    default:return 8;
    }
}
bool crl_verification_error(int error){
    switch(error){case X509_V_ERR_CERT_REVOKED:case X509_V_ERR_UNABLE_TO_GET_CRL:case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE:
    case X509_V_ERR_CRL_SIGNATURE_FAILURE:case X509_V_ERR_CRL_NOT_YET_VALID:case X509_V_ERR_CRL_HAS_EXPIRED:
    case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD:case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD:
    case X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER:case X509_V_ERR_KEYUSAGE_NO_CRL_SIGN:case X509_V_ERR_UNHANDLED_CRITICAL_CRL_EXTENSION:
    case X509_V_ERR_DIFFERENT_CRL_SCOPE:case X509_V_ERR_CRL_PATH_VALIDATION_ERROR:return true;default:return false;}
}
struct Verification {std::map<int,std::uint32_t> errors;bool exclude_root{};};
int collect_certificate_errors(int valid,X509_STORE_CTX* context){
    if(!valid){auto& verification=*static_cast<Verification*>(X509_STORE_CTX_get_app_data(context));const auto error=X509_STORE_CTX_get_error(context);auto* certificate=X509_STORE_CTX_get_current_cert(context);
        if(!(verification.exclude_root && crl_verification_error(error) && certificate && X509_check_issued(certificate,certificate)==X509_V_OK))
            verification.errors[X509_STORE_CTX_get_error_depth(context)]|=trust_error(error);}
    // 保留所有驗證錯誤，讓客體透過 TrustStatus 判斷；此值不代表信任成功。
    return 1;
}
bool certificate_usage(X509* cert,const std::vector<std::string>& requested,bool any){
    if(requested.empty())return true;int critical=-1;std::unique_ptr<EXTENDED_KEY_USAGE,decltype(&EXTENDED_KEY_USAGE_free)> usages(static_cast<EXTENDED_KEY_USAGE*>(X509_get_ext_d2i(cert,NID_ext_key_usage,&critical,nullptr)),EXTENDED_KEY_USAGE_free);
    if(!usages)return critical==-1;
    std::vector<std::string> actual;for(int i=0;i<sk_ASN1_OBJECT_num(usages.get());++i)actual.push_back(oid_name(sk_ASN1_OBJECT_value(usages.get(),i)));
    if(std::find(actual.begin(),actual.end(),"2.5.29.37.0")!=actual.end())return true;
    const auto matches=[&](const auto& oid){return std::find(actual.begin(),actual.end(),oid)!=actual.end();};return any?std::any_of(requested.begin(),requested.end(),matches):std::all_of(requested.begin(),requested.end(),matches);
}
bool certificate_host_matches(X509* cert,std::u16string_view name){
    const auto utf8=utf16_to_utf8(name);std::unique_ptr<ASN1_OCTET_STRING,decltype(&ASN1_OCTET_STRING_free)> ip(a2i_IPADDRESS(utf8.c_str()),ASN1_OCTET_STRING_free);
    if(ip)return X509_check_ip(cert,ASN1_STRING_get0_data(ip.get()),static_cast<std::size_t>(ASN1_STRING_length(ip.get())),0)==1;ERR_clear_error();
    const auto converted=idn_detail::convert(name,true,0,idn_detail::Profile::certificate_uts46);
    if(!converted)return false;const auto ascii=utf16_to_utf8(*converted);
    return X509_check_host(cert,ascii.data(),ascii.size(),X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS,nullptr)==1;
}
}
void certificate_detail::refresh_store(CertificateState& state,std::uint64_t handle,bool automatic,ReadOnlyFiles& files){if(automatic)auto_resync_store(state,handle,files);else resync_store(state,handle,files);}
std::span<const WindowsServiceDescriptor> certificate_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"crypt32.dll","PFXIsPFXBlob",1},{"crypt32.dll","PFXVerifyPassword",3},{"crypt32.dll","PFXImportCertStore",3},
        {"crypt32.dll","CryptInstallOIDFunctionAddress",6},{"crypt32.dll","CryptInitOIDFunctionSet",2},
        {"crypt32.dll","CryptGetOIDFunctionAddress",6},{"crypt32.dll","CryptFreeOIDFunctionAddress",2},
        {"crypt32.dll","CryptQueryObject",11},{"crypt32.dll","CryptMsgOpenToDecode",6},{"crypt32.dll","CryptMsgUpdate",4},
        {"crypt32.dll","CryptMsgGetParam",5},{"crypt32.dll","CryptMsgDuplicate",1},{"crypt32.dll","CryptMsgClose",1},
        {"crypt32.dll","CertVerifyCertificateChainPolicy",4},
        {"crypt32.dll","CertFindCertificateInStore",6},{"crypt32.dll","CertGetIssuerCertificateFromStore",4},
        {"crypt32.dll","CryptVerifyCertificateSignatureEx",8},{"crypt32.dll","CryptVerifyCertificateSignature",5},
        {"crypt32.dll","CertGetCertificateContextProperty",4},{"crypt32.dll","CertSetCertificateContextProperty",4},{"crypt32.dll","CertEnumCertificateContextProperties",2},
        {"crypt32.dll","CertGetNameStringA",6},{"crypt32.dll","CertGetNameStringW",6},
        {"crypt32.dll","CertNameToStrA",5},{"crypt32.dll","CertNameToStrW",5},
        {"crypt32.dll","CertStrToNameA",7},{"crypt32.dll","CertStrToNameW",7},
        {"crypt32.dll","CertCompareCertificateName",3},{"crypt32.dll","CertFindExtension",3},
        {"crypt32.dll","CertGetIntendedKeyUsage",4},{"crypt32.dll","CertVerifyTimeValidity",2},
        {"crypt32.dll","CertCreateCertificateChainEngine",2},{"crypt32.dll","CertFreeCertificateChainEngine",1},{"crypt32.dll","CertResyncCertificateChainEngine",1},
        {"crypt32.dll","CertOpenSystemStoreA",2},{"crypt32.dll","CertOpenSystemStoreW",2},
        {"crypt32.dll","CertGetCertificateChain",8},{"crypt32.dll","CertFreeCertificateChain",1},{"crypt32.dll","CertDuplicateCertificateChain",1},
        {"crypt32.dll","CertOpenStore",5},{"crypt32.dll","CertDuplicateStore",1},{"crypt32.dll","CertCloseStore",2},
        {"crypt32.dll","CertSaveStore",6,0,0,0x2e},
        {"crypt32.dll","CertAddStoreToCollection",4},{"crypt32.dll","CertRemoveStoreFromCollection",2},{"crypt32.dll","CertControlStore",4},
        {"crypt32.dll","CertCreateCertificateContext",3},{"crypt32.dll","CertDuplicateCertificateContext",1},{"crypt32.dll","CertFreeCertificateContext",1},
        {"crypt32.dll","CertGetEnhancedKeyUsage",4},{"crypt32.dll","CertSetEnhancedKeyUsage",2},
        {"crypt32.dll","CertAddEnhancedKeyUsageIdentifier",2},{"crypt32.dll","CertRemoveEnhancedKeyUsageIdentifier",2},{"crypt32.dll","CertGetValidUsages",5},
        {"crypt32.dll","CertAddCertificateContextToStore",4},{"crypt32.dll","CertAddEncodedCertificateToStore",6},
        {"crypt32.dll","CertEnumCertificatesInStore",2},{"crypt32.dll","CertDeleteCertificateFromStore",1},
        {"crypt32.dll","CertCreateCRLContext",3},{"crypt32.dll","CertDuplicateCRLContext",1},{"crypt32.dll","CertFreeCRLContext",1},
        {"crypt32.dll","CertEnumCRLsInStore",2},{"crypt32.dll","CertDeleteCRLFromStore",1},
        {"crypt32.dll","CertAddCRLContextToStore",4},{"crypt32.dll","CertAddEncodedCRLToStore",6},
        {"crypt32.dll","CertGetCRLContextProperty",4},{"crypt32.dll","CertSetCRLContextProperty",4},{"crypt32.dll","CertEnumCRLContextProperties",2},
        {"crypt32.dll","CertVerifyCRLTimeValidity",2},{"crypt32.dll","CertVerifyCRLRevocation",4}
    };return table;
}
std::uint64_t WindowsServices::create_certificate(GuestMemory& memory,const std::vector<std::uint8_t>& der,std::uint64_t store,std::uint64_t identity,std::uint64_t source_store,std::uint64_t source_identity,bool crl){
    auto& certs=runtime_.certificates;auto& contexts=crl?certs.crl_contexts:certs.contexts;
    if(contexts.size()>=4096)throw CertificateError{8};auto packet=[&](){if(crl){auto value=decode_crl(der);return crl_packet(x86_,value.get(),der,store);}auto value=decode_certificate(der);return certificate_packet(x86_,value.get(),der,store);}();
    std::uint64_t properties=0;
    if(!source_store){source_store=store;source_identity=identity;if(const auto found=certs.stores.find(store);found!=certs.stores.end() && found->second.collection){
        for(const auto& item:store_entries(certs,store,0,crl))if(item.identity==identity){source_store=item.store;source_identity=item.source_identity;break;}}}
    if(const auto found=certs.stores.find(source_store);found!=certs.stores.end()){const auto& identities=crl?found->second.crl_property_identities:found->second.property_identities;if(const auto item=identities.find(source_identity);item!=identities.end())properties=item->second;}
    const bool new_properties=!properties;if(new_properties){properties=certs.next_property_identity++;certs.properties.emplace(properties,CertificateProperties{});}
    std::uint64_t pointer=0;
    try{pointer=allocate_heap(memory,packet.bytes.size());if(!pointer)throw CertificateError{8};packet.relocate(pointer);memory.write(GuestAddress{pointer},packet.bytes);
        contexts.emplace(pointer,CertificateContext{der,store,identity,properties,1,source_store,source_identity});return pointer;
    }catch(...){if(new_properties)certs.properties.erase(properties);if(pointer)(void)free_heap(memory,pointer,GuestState{});throw;}
}
bool WindowsServices::free_certificate(GuestState& state,GuestMemory& memory,std::uint64_t pointer,bool crl){
    if(!pointer)return true;auto& certs=runtime_.certificates;auto& contexts=crl?certs.crl_contexts:certs.contexts;const auto context=contexts.find(pointer);if(context==contexts.end())return false;
    if(!--context->second.references){contexts.erase(context);(void)free_heap(memory,pointer,state);prune_stores(certs);prune_certificate_properties();}
    return true;
}
std::uint64_t WindowsServices::build_certificate_chain(GuestState& state,GuestMemory& memory,std::uint64_t engine_handle,std::uint64_t leaf_context,std::uint64_t time,std::uint64_t additional,const std::vector<std::string>& requested,bool usage_any,std::uint32_t flags,bool cache_only){
    auto& certs=runtime_.certificates;const unsigned unit=x86_?4:8;
    const auto context=certs.contexts.find(leaf_context);if(context==certs.contexts.end())throw CertificateError{87};
    const CertificateEngine* engine=nullptr;if(engine_handle>1){const auto found=certs.engines.find(engine_handle);if(found==certs.engines.end())throw CertificateError{6};engine=&found->second;}
    const auto revocation=flags&0x70000000U;
    auto leaf=decode_certificate(context->second.der);std::vector<Certificate> issuers;std::vector<RevocationList> crls;
    const auto add_store=[&](std::uint64_t handle,bool require_open){if(!handle)return;const auto store=certs.stores.find(handle);if(store==certs.stores.end() || (require_open && !store->second.references))throw CertificateError{87};for(const auto& item:store_entries(certs,handle))issuers.push_back(decode_certificate(*item.der));
        if(revocation)for(const auto& item:store_entries(certs,handle,0,true))crls.push_back(decode_crl(*item.der));};
    if(!cache_only){add_store(context->second.store,false);if(additional!=context->second.store)add_store(additional,true);}
    if(engine){add_store(engine->root,false);add_store(engine->other,false);for(const auto handle:engine->additional)add_store(handle,false);}
    std::unique_ptr<X509_STORE,decltype(&X509_STORE_free)> trusted(X509_STORE_new(),X509_STORE_free);
    std::unique_ptr<X509_STORE_CTX,decltype(&X509_STORE_CTX_free)> verify(X509_STORE_CTX_new(),X509_STORE_CTX_free);
    const auto free_stack=[](STACK_OF(X509)* values){sk_X509_free(values);};std::unique_ptr<STACK_OF(X509),decltype(free_stack)> untrusted(sk_X509_new_null(),free_stack);
    if(!trusted || !verify || !untrusted)throw CertificateError{8};
    if(!engine || !engine->root){const auto roots=sandbox_certificate_roots(filesystem_);if(roots.error)throw CertificateError{roots.error};if(!add_certificate_root_pem(trusted.get(),roots.pem))throw CertificateError{8};}
    else {
        const auto defaults=engine->exclusive?std::vector<Certificate>{}:certificate_roots(filesystem_);
        for(const auto& item:store_entries(certs,engine->root)){
            auto root=decode_certificate(*item.der);X509* anchor=nullptr;
            if(engine->exclusive){if(X509_check_issued(root.get(),root.get())==X509_V_OK)anchor=root.get();}
            else for(const auto& candidate:defaults)if(X509_cmp(root.get(),candidate.get())==0){anchor=candidate.get();break;}
            if(anchor && !X509_STORE_add_cert(trusted.get(),anchor))throw CertificateError{8};
        }
    }
    for(const auto& issuer:issuers)if(!sk_X509_push(untrusted.get(),issuer.get()))throw CertificateError{8};
    for(const auto& crl:crls)if(!X509_STORE_add_crl(trusted.get(),crl.get()))throw CertificateError{8};
    if(!X509_STORE_CTX_init(verify.get(),trusted.get(),leaf.get(),untrusted.get()))throw CertificateError{8};

    X509_VERIFY_PARAM_set_time(X509_STORE_CTX_get0_param(verify.get()),static_cast<std::time_t>(time/10000000)-11644473600LL);
    X509_VERIFY_PARAM_set_depth(X509_STORE_CTX_get0_param(verify.get()),64);X509_VERIFY_PARAM_set_flags(X509_STORE_CTX_get0_param(verify.get()),X509_V_FLAG_CHECK_SS_SIGNATURE);
    if(revocation)X509_VERIFY_PARAM_set_flags(X509_STORE_CTX_get0_param(verify.get()),X509_V_FLAG_CRL_CHECK|(revocation==0x10000000U?0:X509_V_FLAG_CRL_CHECK_ALL)|X509_V_FLAG_USE_DELTAS|X509_V_FLAG_EXTENDED_CRL_SUPPORT);
    Verification verification;verification.exclude_root=revocation==0x40000000U;X509_STORE_CTX_set_app_data(verify.get(),&verification);X509_STORE_CTX_set_verify_cb(verify.get(),collect_certificate_errors);
    const auto verified=X509_verify_cert(verify.get());if(verified<=0 && verification.errors.empty())verification.errors[0]|=trust_error(X509_STORE_CTX_get_error(verify.get()))|0x10000U;
    auto* chain=X509_STORE_CTX_get0_chain(verify.get());const auto length=chain?sk_X509_num(chain):0;if(length<=0 || length>64)throw CertificateError{0x800b010a};
    CertificatePacket packet(x86_);const unsigned chain_size=x86_?56:72,simple_size=x86_?32:40,element_size=x86_?32:56;packet.reserve(chain_size,unit);const auto simple=packet.reserve(simple_size,unit),chain_array=packet.reserve(unit,unit),element_array=packet.reserve(static_cast<std::size_t>(length)*unit,unit);
    packet.put(0,chain_size);packet.put(12,1);packet.point(16,chain_array);packet.point(chain_array,simple);packet.put(x86_?36:48,flags);
    packet.put(simple,simple_size);packet.put(simple+12,static_cast<unsigned>(length));packet.point(simple+16,element_array);CertificateChain held;std::uint32_t errors=0;std::uint64_t output=0;
    try {
        for(int i=0;i<length;++i){auto* cert=sk_X509_value(chain,i);auto status=verification.errors[i];if(!certificate_usage(cert,requested,usage_any))status|=0x10;
            const bool self_signed=X509_check_issued(cert,cert)==X509_V_OK;
            errors|=status;std::uint64_t pointer=0;
            if(i==0){if(context->second.references==UINT32_MAX)throw CertificateError{8};++context->second.references;pointer=leaf_context;}
            else pointer=create_certificate(memory,encoded(cert,i2d_X509));held.contexts.push_back(pointer);const auto element=packet.reserve(element_size,unit);packet.point(element_array+static_cast<std::size_t>(i)*unit,element);packet.put(element,element_size);packet.put(element+unit,pointer,unit);packet.put(element+2*unit,status);packet.put(element+2*unit+4,self_signed?8U:0U);
        }
        packet.put(4,errors);packet.put(simple+4,errors);output=allocate_heap(memory,packet.bytes.size());if(!output)throw CertificateError{8};packet.relocate(output);memory.write(GuestAddress{output},packet.bytes);certs.chains.emplace(output,std::move(held));
    }catch(...){for(auto pointer:held.contexts)(void)free_certificate(state,memory,pointer);if(output)(void)free_heap(memory,output,state);throw;}
    ERR_clear_error();return output;
}
std::optional<std::uint32_t> WindowsServices::invoke_certificate(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const auto u32=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};auto& certs=runtime_.certificates;std::uint64_t result=0;
    const bool crl_api=symbol.find("CRL")!=symbol.npos;auto& contexts=crl_api?certs.crl_contexts:certs.contexts;
    const auto fail=[&](std::uint32_t code){last_error_=code;result=0;};
    const auto read_der=[&](std::uint32_t encoding,std::uint64_t pointer,std::uint32_t size){if((encoding&0xffff)!=1)throw CertificateError{0x80070057};if(!pointer || !size)throw CertificateError{0x8009310b};if(size>output_limit)throw CertificateError{8};std::vector<std::uint8_t> der(size);memory.read(GuestAddress{pointer},der);return der;};
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto blob=[&](std::uint64_t address){if(!address)throw CertificateError{87};const auto size=memory.load(GuestAddress{address},32),pointer=memory.load(GuestAddress{address+unit},bits);if(size>output_limit)throw CertificateError{8};std::vector<std::uint8_t> value(static_cast<std::size_t>(size));if(size){if(!pointer)throw CertificateError{87};memory.read(GuestAddress{pointer},value);}return value;};
    const auto extension=[&](std::string_view oid,std::uint64_t count,std::uint64_t pointer){if(count>4096 || (count && !pointer))throw CertificateError{87};for(std::uint64_t i=0;i<count;++i){const auto entry=pointer+i*unit*4;const auto name=memory.load(GuestAddress{entry},bits);if(name && read_ansi(memory,name,1024)==oid)return entry;}return UINT64_C(0);};
    const auto public_key=[&](std::uint64_t address){
        if(!address)throw CertificateError{87};const auto oid_pointer=memory.load(GuestAddress{address},bits);if(!oid_pointer)throw CertificateError{87};const auto oid=read_ansi(memory,oid_pointer,1024);
        std::unique_ptr<ASN1_OBJECT,decltype(&ASN1_OBJECT_free)> object(OBJ_txt2obj(oid.c_str(),1),ASN1_OBJECT_free);if(!object)throw CertificateError{0x8009310b};auto algorithm=encoded(object.get(),i2d_ASN1_OBJECT);const auto parameters=blob(address+unit);algorithm.insert(algorithm.end(),parameters.begin(),parameters.end());
        auto key=blob(address+3*unit);const auto unused=memory.load(GuestAddress{address+5*unit},32);if(unused>7 || key.empty() || (unused && (key.back()&((1U<<unused)-1U))))throw CertificateError{0x8009310b};key.insert(key.begin(),static_cast<std::uint8_t>(unused));
        auto info=certificate_tlv(48,algorithm);const auto key_bits=certificate_tlv(3,key);info.insert(info.end(),key_bits.begin(),key_bits.end());const auto der=certificate_tlv(48,info);const auto* cursor=der.data();
        std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> output(d2i_PUBKEY(nullptr,&cursor,static_cast<long>(der.size())),EVP_PKEY_free);if(!output || cursor!=der.data()+der.size())throw CertificateError{0x8009310b};return output;
    };
    try {
        #include "certificate_pfx_import.inc"
        if(symbol=="CryptInstallOIDFunctionAddress" || symbol=="CryptInitOIDFunctionSet"){
            const bool install=symbol=="CryptInstallOIDFunctionAddress";
            const auto pointer=arg(install?2:0);const auto count=install?u32(3):0U,flags=u32(install?5:1);
            if(!pointer || (flags&~(install?1U:0U)) || count>65536 || (count && !arg(4)))throw CertificateError{87};
            auto name=read_ansi(memory,pointer,1024);if(name.empty())throw CertificateError{87};
            for(auto& c:name)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');
            auto found=std::find_if(certs.oid_sets.begin(),certs.oid_sets.end(),[&](const auto& item){return item.second.name==name;});
            std::vector<CryptOidFunction> incoming;incoming.reserve(count);
            for(unsigned n=0;n<count;++n){
                const auto at=arg(4)+std::uint64_t(n)*unit*2,oid=memory.load(GuestAddress{at},bits),address=memory.load(GuestAddress{at+unit},bits);
                if(!address)throw CertificateError{87};
                auto key=oid<=65535?"#"+std::to_string(oid):"s:"+read_ansi(memory,oid,1024);
                for(auto& c:key)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');
                incoming.push_back({std::move(key),address,arg(0),u32(1)&65535U});
            }
            if(found==certs.oid_sets.end()){
                if(certs.oid_sets.size()>=1024)throw CertificateError{8};
                found=certs.oid_sets.emplace(runtime_.allocate_wait_handle(),CryptOidFunctionSet{std::move(name),{}}).first;
            }
            auto& functions=found->second.functions;if(count>65536-functions.size())throw CertificateError{8};
            // 先驗證完整輸入，再一次插入；BEFORE 仍保留同一批陣列的順序。
            functions.insert(flags&1U?functions.begin():functions.end(),incoming.begin(),incoming.end());
            if(install && count)if(auto module=runtime_.loaded_modules.find(arg(0));module!=runtime_.loaded_modules.end())module->second.pinned=true;
            result=install?1:found->first;
        }else if(symbol=="CryptGetOIDFunctionAddress"){
            if(!arg(4) || !arg(5) || (u32(3)&~1U))throw CertificateError{87};
            memory.check(GuestAddress{arg(4)},unit,Access::Write);memory.check(GuestAddress{arg(5)},unit,Access::Write);
            memory.store(GuestAddress{arg(4)},bits,0);memory.store(GuestAddress{arg(5)},bits,0);
            const auto set=certs.oid_sets.find(arg(0));if(set==certs.oid_sets.end())throw CertificateError{6};
            auto key=arg(2)<=65535?"#"+std::to_string(arg(2)):"s:"+read_ansi(memory,arg(2),1024);
            for(auto& c:key)if(c>='A' && c<='Z')c=static_cast<char>(c-'A'+'a');
            const auto found=std::find_if(set->second.functions.begin(),set->second.functions.end(),[&](const auto& item){return item.encoding==(u32(1)&65535U) && item.oid==key;});
            if(found==set->second.functions.end())throw CertificateError{2};
            if(certs.oid_addresses.size()>=65536)throw CertificateError{8};
            const auto handle=runtime_.allocate_wait_handle();certs.oid_addresses.emplace(handle,*found);
            memory.store(GuestAddress{arg(4)},bits,found->address);memory.store(GuestAddress{arg(5)},bits,handle);result=1;
        }else if(symbol=="CryptFreeOIDFunctionAddress"){
            if(u32(1))throw CertificateError{87};
            if(arg(0) && !certs.oid_addresses.erase(arg(0)))throw CertificateError{6};result=1;
        }else if(symbol=="CryptMsgOpenToDecode"){
            if((u32(0)&0xffff0000U)!=0x10000U || u32(1) || arg(3) || arg(4) || arg(5))throw CertificateError{50};
            if(u32(2)>2)throw CertificateError{50};if(certs.messages.size()>=128)throw CertificateError{8};
            CryptMessage message;message.type=u32(2);result=runtime_.allocate_wait_handle();certs.messages.emplace(result,std::move(message));
        }else if(symbol=="CryptMsgDuplicate" || symbol=="CryptMsgClose" || symbol=="CryptMsgUpdate" || symbol=="CryptMsgGetParam"){
            const auto found=certs.messages.find(arg(0));if(found==certs.messages.end())throw CertificateError{6};auto& message=found->second;
            if(symbol=="CryptMsgClose"){if(!--message.references)certs.messages.erase(found);result=1;}
            else if(symbol=="CryptMsgDuplicate"){if(message.references==UINT32_MAX)throw CertificateError{8};++message.references;result=arg(0);}
            else if(symbol=="CryptMsgUpdate"){
                if(message.finalized)throw CertificateError{0x80091001};if(u32(2)>output_limit-message.encoded.size())throw CertificateError{8};
                if(u32(2) && !arg(1))throw CertificateError{87};std::vector<std::uint8_t> input(u32(2));memory.read(GuestAddress{arg(1)},input);
                message.encoded.insert(message.encoded.end(),input.begin(),input.end());
                if(u32(3)){auto parsed=decode_message(message.encoded);const auto type=PKCS7_type_is_signed(parsed.get())?2U:1U;if(message.type && message.type!=type)throw CertificateError{0x80091004};message.type=type;message.finalized=true;}
                result=1;
            }else {
                if(!arg(4))throw CertificateError{87};if(!message.finalized)throw CertificateError{0x80091001};
                auto parsed=decode_message(message.encoded);auto packet=message_parameter(x86_,parsed.get(),u32(1),u32(2));
                const auto capacity=memory.load(GuestAddress{arg(4)},32);memory.store(GuestAddress{arg(4)},32,packet.bytes.size());
                if(arg(3)){if(capacity<packet.bytes.size())throw CertificateError{234};packet.relocate(arg(3));memory.write(GuestAddress{arg(3)},packet.bytes);}result=1;
            }
        }else if(symbol=="CryptQueryObject"){
            if((u32(0)!=1 && u32(0)!=2) || !arg(1) || !u32(2) || !u32(3) || u32(4))throw CertificateError{87};
            for(unsigned i=5;i<=10;++i)if(arg(i))memory.check(GuestAddress{arg(i)},i<8?4:unit,Access::Write);
            for(unsigned i=8;i<=10;++i)if(arg(i))memory.store(GuestAddress{arg(i)},bits,0);
            if(!(u32(3)&2))throw CertificateError{50};
            std::vector<std::uint8_t> input;
            if(u32(0)==2)input=blob(arg(1));else {const auto file=filesystem_.open(utf16_to_utf8(read_utf16(memory,GuestAddress{arg(1)})));if(file.error)throw CertificateError{file.error};input=*file.bytes;}
            if(input.empty() || input.size()>output_limit)throw CertificateError{input.empty()?0x80092009U:8U};
            std::uint32_t content=0,encoding=1;SignedMessage parsed(nullptr,PKCS7_free);std::vector<std::vector<std::uint8_t>> certificates,crls;
            if(u32(2)&2){try{auto cert=decode_certificate(input);(void)cert;content=1;certificates.push_back(input);}catch(const CertificateError&){ERR_clear_error();}}
            if(!content && (u32(2)&8)){try{auto crl=decode_crl(input);(void)crl;content=3;crls.push_back(input);}catch(const CertificateError&){ERR_clear_error();}}
            if(!content && (u32(2)&0x700)){
                auto bytes=input;const bool embedded=bytes.size()>2 && bytes[0]=='M' && bytes[1]=='Z';
                try{if(embedded){if(!(u32(2)&0x400))throw CertificateError{0x80092009};bytes=embedded_message(bytes);}parsed=decode_message(bytes);
                    content=embedded?10U:PKCS7_type_is_signed(parsed.get())?8U:9U;if(!(u32(2)&(1U<<content)))throw CertificateError{0x80092009};
                    if(embedded && !PKCS7_type_is_signed(parsed.get()))throw CertificateError{0x80092009};input=std::move(bytes);encoding=0x10001;
                    if(PKCS7_type_is_signed(parsed.get())){
                        crls=message_crls(parsed.get());
                        if(parsed->d.sign->cert){const auto count=sk_X509_num(parsed->d.sign->cert);if(count>4096)throw CertificateError{8};for(int i=0;i<count;++i)certificates.push_back(encoded(sk_X509_value(parsed->d.sign->cert,i),i2d_X509));}
                    }
                }catch(const CertificateError& error){if(error.code==8 || error.code==50)throw;throw CertificateError{0x80092009};}
            }
            if(!content)throw CertificateError{0x80092009};
            if((arg(8) && certs.stores.size()>=128) || (arg(9) && parsed && certs.messages.size()>=128))throw CertificateError{8};
            std::uint64_t store_handle=0,message_handle=0,context=0;
            try {
                if(arg(8)){
                    CertificateStore store;store.readonly=true;for(const auto& der:certificates){const auto identity=store.next_identity++,property=certs.next_property_identity++;store.items.emplace(identity,der);store.property_identities.emplace(identity,property);certs.properties.emplace(property,CertificateProperties{});}
                    copy_store_crls(certs,store,crls);
                    store_handle=runtime_.allocate_wait_handle();certs.stores.emplace(store_handle,std::move(store));
                }
                if(arg(10) && content==1)context=create_certificate(memory,input,store_handle,store_handle?1:0);
                if(arg(10) && content==3)context=create_certificate(memory,input,store_handle,store_handle?1:0,0,0,true);
                if(arg(9) && parsed){CryptMessage message;message.encoded=input;message.type=PKCS7_type_is_signed(parsed.get())?2U:1U;message.finalized=true;message_handle=runtime_.allocate_wait_handle();certs.messages.emplace(message_handle,std::move(message));}
                if(arg(5))memory.store(GuestAddress{arg(5)},32,encoding);if(arg(6))memory.store(GuestAddress{arg(6)},32,content);if(arg(7))memory.store(GuestAddress{arg(7)},32,1);
                if(arg(8))memory.store(GuestAddress{arg(8)},bits,store_handle);if(arg(9))memory.store(GuestAddress{arg(9)},bits,message_handle);if(arg(10))memory.store(GuestAddress{arg(10)},bits,context);result=1;
            }catch(...){if(context)(void)free_certificate(state,memory,context,content==3);if(store_handle)certs.stores.erase(store_handle);if(message_handle)certs.messages.erase(message_handle);prune_certificate_properties();throw;}
        }else if(symbol=="CertCreateCertificateChainEngine"){
            if(!arg(1))throw CertificateError{87};memory.store(GuestAddress{arg(1)},bits,0);if(!arg(0))throw CertificateError{87};
            const auto size=memory.load(GuestAddress{arg(0)},32);const unsigned legacy=x86_?40:64,extended=x86_?48:80,full=x86_?52:88;
            if(size!=legacy && size!=extended && size!=full)throw CertificateError{87};memory.check(GuestAddress{arg(0)},static_cast<std::size_t>(size),Access::Read);
            CertificateEngine engine;engine.root=memory.load(GuestAddress{arg(0)+unit},bits);engine.other=memory.load(GuestAddress{arg(0)+3*unit},bits);
            const auto trust=memory.load(GuestAddress{arg(0)+2*unit},bits),count=memory.load(GuestAddress{arg(0)+4*unit},32),array=memory.load(GuestAddress{arg(0)+5*unit},bits);
            const auto flags=memory.load(GuestAddress{arg(0)+6*unit},32);if(trust || (flags&~0x203dU))throw CertificateError{50};if(count>128 || (count && !array))throw CertificateError{87};
            if(size>=extended){const auto exclusive=memory.load(GuestAddress{arg(0)+legacy},bits),people=memory.load(GuestAddress{arg(0)+legacy+unit},bits);if(people)throw CertificateError{50};if(exclusive){if(engine.root)throw CertificateError{87};engine.root=exclusive;engine.exclusive=true;}}
            if(size==full && memory.load(GuestAddress{arg(0)+extended},32))throw CertificateError{50};
            std::set<std::uint64_t> retained;
            const auto retain=[&](std::uint64_t handle){if(!handle)return;const auto store=certs.stores.find(handle);if(store==certs.stores.end() || !store->second.references)throw CertificateError{6};if(store->second.engine_references==UINT32_MAX)throw CertificateError{8};retained.insert(handle);};
            retain(engine.root);retain(engine.other);for(std::uint64_t i=0;i<count;++i){const auto handle=memory.load(GuestAddress{array+i*unit},bits);if(!handle)throw CertificateError{87};retain(handle);engine.additional.push_back(handle);}
            if(engine.root && !engine.exclusive){
                const auto roots=certificate_roots(filesystem_);for(const auto& item:store_entries(certs,engine.root)){auto candidate=decode_certificate(*item.der);
                    if(std::none_of(roots.begin(),roots.end(),[&](const auto& root){return X509_cmp(root.get(),candidate.get())==0;}))throw CertificateError{87};}
            }
            if(certs.engines.size()>=128)throw CertificateError{8};engine.retained.assign(retained.begin(),retained.end());const auto handle=runtime_.allocate_wait_handle();
            certs.engines.emplace(handle,std::move(engine));for(const auto store:retained)++certs.stores.at(store).engine_references;memory.store(GuestAddress{arg(1)},bits,handle);result=1;
        }else if(symbol=="CertFreeCertificateChainEngine" || symbol=="CertResyncCertificateChainEngine"){
            const auto found=certs.engines.find(arg(0));
            if(symbol=="CertResyncCertificateChainEngine" && arg(0)<=1)result=1;
            else if(found==certs.engines.end())throw CertificateError{6};
            else if(symbol=="CertResyncCertificateChainEngine")result=1; // 每次建鏈重新讀取保留的 store，沒有過期快取。
            else {for(const auto handle:found->second.retained)--certs.stores.at(handle).engine_references;
                certs.engines.erase(found);prune_stores(certs);prune_certificate_properties();}
        }else if(symbol=="CertVerifyCertificateChainPolicy"){
            const auto policy=arg(0);if(policy!=1 && policy!=4 && policy!=5)throw CertificateError{50};
            if(!arg(1) || !arg(3) || memory.load(GuestAddress{arg(1)},32)<(x86_?56U:72U) || memory.load(GuestAddress{arg(3)},32)<16+unit)throw CertificateError{87};
            memory.check(GuestAddress{arg(3)+4},12,Access::Write);if(memory.load(GuestAddress{arg(3)+16},bits))throw CertificateError{50};
            std::uint32_t flags=0,ssl_flags=0,auth=2;std::uint64_t extra=0,hostname=0;
            if(arg(2)){if(memory.load(GuestAddress{arg(2)},32)<8+unit)throw CertificateError{87};flags=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+4},32));extra=memory.load(GuestAddress{arg(2)+8},bits);}
            if(policy==5){if((flags&~0xc0000000U) || extra)throw CertificateError{50};}
            else {if(flags&~0x2fffU)throw CertificateError{50};if(extra && policy!=4)throw CertificateError{50};}
            if(policy==4 && extra){if(memory.load(GuestAddress{extra},32)<(x86_?16U:24U))throw CertificateError{87};auth=static_cast<std::uint32_t>(memory.load(GuestAddress{extra+4},32));ssl_flags=static_cast<std::uint32_t>(memory.load(GuestAddress{extra+8},32));hostname=memory.load(GuestAddress{extra+(x86_?12:16)},bits);
                if(auth!=1 && auth!=2)throw CertificateError{87};if(ssl_flags&~0x3380U)throw CertificateError{50};
                if(ssl_flags&0x80U)flags|=0xf00U;if(ssl_flags&0x100U)flags|=0x10U;if(ssl_flags&0x200U)flags|=0x20U;if(ssl_flags&0x2000U)flags|=7U;
            }
            struct PolicyElement {std::uint32_t errors,chain,index;std::uint64_t context;bool root;};std::vector<PolicyElement> elements;
            const auto chain_count=memory.load(GuestAddress{arg(1)+12},32),chain_array=memory.load(GuestAddress{arg(1)+16},bits);if(!chain_count || chain_count>64 || !chain_array)throw CertificateError{87};std::uint32_t chain_union=0;
            for(std::uint32_t c=0;c<chain_count;++c){const auto simple=memory.load(GuestAddress{chain_array+c*unit},bits);if(!simple || memory.load(GuestAddress{simple},32)<(x86_?32U:40U))throw CertificateError{87};
                const auto count=memory.load(GuestAddress{simple+12},32),array=memory.load(GuestAddress{simple+16},bits);if(!count || count>128 || !array)throw CertificateError{87};std::uint32_t element_union=0;
                for(std::uint32_t i=0;i<count;++i){const auto element=memory.load(GuestAddress{array+i*unit},bits);if(!element || memory.load(GuestAddress{element},32)<(x86_?32U:56U))throw CertificateError{87};
                    const auto context=memory.load(GuestAddress{element+unit},bits);if(!context)throw CertificateError{87};const auto errors=static_cast<std::uint32_t>(memory.load(GuestAddress{element+2*unit},32));element_union|=errors;
                    const auto info=memory.load(GuestAddress{element+2*unit+4},32);elements.push_back({errors,c,i,context,(info&8U)!=0});
                }
                const auto errors=static_cast<std::uint32_t>(memory.load(GuestAddress{simple+4},32));elements.push_back({errors&~element_union,c,UINT32_MAX,0,false});chain_union|=errors|element_union;
            }
            elements.push_back({static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+4},32))&~chain_union,UINT32_MAX,UINT32_MAX,0,false});
            const auto decode_context=[&](std::uint64_t pointer){return decode_certificate(read_der(static_cast<std::uint32_t>(memory.load(GuestAddress{pointer},32)),memory.load(GuestAddress{pointer+unit},bits),static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+2*unit},32))));};
            std::uint32_t error=0,error_chain=UINT32_MAX,error_element=UINT32_MAX;
            const auto reject=[&](std::uint32_t code,const PolicyElement& element){error=code;error_chain=element.chain;error_element=element.index;};
            if(policy==5){std::map<std::uint32_t,std::uint64_t> subordinate_cas;
                for(const auto& element:elements){if(!element.context)continue;auto cert=decode_context(element.context);int critical=-1;
                    std::unique_ptr<BASIC_CONSTRAINTS,decltype(&BASIC_CONSTRAINTS_free)> constraints(static_cast<BASIC_CONSTRAINTS*>(X509_get_ext_d2i(cert.get(),NID_basic_constraints,&critical,nullptr)),BASIC_CONSTRAINTS_free);
                    if(!constraints){if(critical!=-1){reject(0x80096019,element);break;}for(int i=0;i<X509_get_ext_count(cert.get());++i)if(oid_name(X509_EXTENSION_get_object(X509_get_ext(cert.get(),i)))=="2.5.29.10")throw CertificateError{50};continue;}
                    const bool ca=constraints->ca!=0;const auto expected=flags&0xc0000000U;
                    if((element.index && !ca) || (!element.index && ((expected==0x80000000U && !ca) || (expected==0x40000000U && ca)))){reject(0x80096019,element);break;}
                    if(constraints->pathlen){std::uint64_t limit=0;if(!ASN1_INTEGER_get_uint64(&limit,constraints->pathlen) || subordinate_cas[element.chain]>limit){reject(0x80096019,element);break;}}
                    if(ca && X509_NAME_cmp(X509_get_subject_name(cert.get()),X509_get_issuer_name(cert.get()))!=0)++subordinate_cas[element.chain];
                }
            }else {
                for(auto& element:elements){auto& errors=element.errors;
                    if(flags&1U)errors&=~1U;if(flags&2U)errors&=~0x20000U;if(flags&4U)errors&=~2U;if(flags&8U)errors&=~0x400U;if(flags&0x10U)errors&=~0x10020U;if(flags&0x20U)errors&=~0x80010U;if(flags&0x40U)errors&=~0xf800U;if(flags&0x80U)errors&=~0x200U;if(flags&0x2000U)errors&=~0x8000000U;
                    const auto revocation_ignore=element.index==UINT32_MAX?((flags&0xf00U)==0xf00U):element.root?(flags&0x800U):element.index==0?(flags&(element.chain?0x200U:0x100U)):(flags&0x400U);
                    if(revocation_ignore)errors&=~0x1000040U;if(ssl_flags&0x80U)errors&=~4U;
                }
                static constexpr std::pair<std::uint32_t,std::uint32_t> mappings[]={{0x4000000,0x800b0111},{4,0x800b010c},{0x40008,0x80096004},{0x100000,0x80096004},{0x20,0x800b0109},{0x10080,0x800b010a},{0x20001,0x800b0101},{2,0x800b0102},{0x80010,0x800b0110},{0x400,0x80096019},{0x200,0x800b0113},{0xf800,0x800b0114},{0x8000100,0x800b0105},{0x1000000,0x80092013},{0x40,0x80092012}};
                for(const auto& [mask,code]:mappings){for(const auto& element:elements)if(element.errors&mask){reject(code,element);break;}if(error)break;}
                if(!error)for(const auto& element:elements)if(element.errors){reject(0x800b0108,element);break;}
                if(!error && policy==4){for(const auto& element:elements){if(!element.context)continue;auto cert=decode_context(element.context);
                        if(!(flags&0x20U) && !certificate_usage(cert.get(),{auth==1?"1.3.6.1.5.5.7.3.2":"1.3.6.1.5.5.7.3.1"},false)){reject(0x800b0110,element);break;}
                        if(element.chain==0 && element.index==0 && auth==2 && hostname && !(ssl_flags&0x1000U) && !certificate_host_matches(cert.get(),read_utf16(memory,GuestAddress{hostname},1024))){reject(0x800b010f,element);break;}
                    }
                }
            }
            memory.store(GuestAddress{arg(3)+4},32,error);memory.store(GuestAddress{arg(3)+8},32,error_chain);memory.store(GuestAddress{arg(3)+12},32,error_element);result=1;
        }else if(symbol=="CertFindCertificateInStore" || symbol=="CertGetIssuerCertificateFromStore"){
            const bool issuer_search=symbol=="CertGetIssuerCertificateFromStore";const auto previous_pointer=arg(issuer_search?2:5);std::uint64_t previous=0;bool valid_previous=true;
            if(previous_pointer){const auto old=certs.contexts.find(previous_pointer);valid_previous=old!=certs.contexts.end() && old->second.store==arg(0);if(valid_previous)previous=old->second.identity;(void)free_certificate(state,memory,previous_pointer);}
            const auto store=certs.stores.find(arg(0));if(!valid_previous || store==certs.stores.end() || !store->second.references)throw CertificateError{0x80070057};
            if(!previous)auto_resync_store(certs,arg(0),filesystem_);
            const auto type=issuer_search?12U:u32(3)>>16,field=issuer_search?0U:u32(3)&65535;const auto parameter=arg(issuer_search?1:4);std::uint32_t checks=0;
            if(issuer_search){if(!arg(3))throw CertificateError{87};checks=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));if(checks&~0x30007U)throw CertificateError{0x80070057};checks&=7;}
            else {if((u32(1)&65535)!=1)throw CertificateError{0x80070057};if(u32(2))throw CertificateError{50};}
            if((type==2 || type==7 || type==8)?(field!=4 && field!=7):field!=0)throw CertificateError{0x80070057};
            if(type && type!=21 && !parameter)throw CertificateError{0x80070057};
            std::vector<std::uint8_t> wanted,wanted_serial;std::u16string substring;Certificate subject(nullptr,X509_free);std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> wanted_key(nullptr,EVP_PKEY_free);std::uint32_t property=0;
            if(type==1 || type==2 || type==4 || type==14 || type==15 || type==18)wanted=blob(parameter);
            else if(type==5){property=static_cast<std::uint32_t>(memory.load(GuestAddress{parameter},32));if(!property || property>65535)throw CertificateError{0x80070057};}
            else if(type==6)wanted_key=public_key(parameter);
            else if(type==7 || type==8){substring=type==8?read_utf16(memory,GuestAddress{parameter}):utf8_to_utf16_lossy(read_ansi(memory,parameter));for(auto& c:substring)c=unicode_case_unit(c,false);}
            else if(type==11){wanted=blob(parameter+6*unit);wanted_serial=blob(parameter+unit);}
            else if(type==12 || type==13){const auto input=certs.contexts.find(parameter);if(input==certs.contexts.end())throw CertificateError{87};subject=decode_certificate(input->second.der);wanted=input->second.der;}
            else if(type!=0 && type!=21)throw CertificateError{50};
            if(issuer_search && X509_NAME_cmp(X509_get_subject_name(subject.get()),X509_get_issuer_name(subject.get()))==0)throw CertificateError{0x80092007};
            const auto now=runtime_.system_filetime();
            bool after=!previous;for(const auto& item:store_entries(certs,arg(0))){if(!after){if(item.identity==previous){after=true;continue;}if(!store->second.collection && item.identity>previous)after=true;else continue;}const auto properties=item.properties;auto& values=certs.properties.at(properties);bool matches=false;
                if(type==0){matches=store->second.enum_archived || !values.contains(19);}else if(type==21)matches=false; // 公開 DER store 沒有私鑰。
                else if(type==13)matches=*item.der==wanted;
                else if(type==1 || type==4 || type==5 || type==14 || type==15 || type==18){const auto id=type==1?3U:type==4?4U:type==14?15U:type==15?20U:type==18?25U:property;
                    auto value=values.find(id);if(value==values.end())if(auto computed=certificate_computed_property(CertificateContext{*item.der,arg(0),item.identity,properties},id))value=values.emplace(id,std::move(*computed)).first;
                    matches=value!=values.end() && (type==5 || value->second==wanted);
                }else {auto cert=decode_certificate(*item.der);auto* name=field==4?X509_get_issuer_name(cert.get()):X509_get_subject_name(cert.get());
                    if(type==2)matches=encoded(name,i2d_X509_NAME)==wanted;
                    else if(type==6){auto* key=X509_get0_pubkey(cert.get());matches=key && EVP_PKEY_eq(key,wanted_key.get())==1;}
                    else if(type==7 || type==8){auto text=certificate_rdn(name,1);for(auto& c:text)c=unicode_case_unit(c,false);matches=text.find(substring)!=text.npos;}
                    else if(type==11){auto serial=asn1_content(encoded(X509_get0_serialNumber(cert.get()),i2d_ASN1_INTEGER));std::reverse(serial.begin(),serial.end());matches=encoded(X509_get_issuer_name(cert.get()),i2d_X509_NAME)==wanted && serial==wanted_serial;}
                    else if(type==12){matches=X509_check_issued(cert.get(),subject.get())==X509_V_OK;
                        if(matches && issuer_search){if((checks&1U) && X509_verify(subject.get(),X509_get0_pubkey(cert.get()))==1)checks&=~1U;
                            if((checks&2U) && now>=certificate_time(X509_get0_notBefore(subject.get())) && now<=certificate_time(X509_get0_notAfter(subject.get())))checks&=~2U;if(checks&4U)checks|=0x10000U;}}
                }
                if(matches){result=create_certificate(memory,*item.der,arg(0),item.identity);break;}
            }
            if(issuer_search){if(!result && (checks&5U))checks|=0x20000U;memory.store(GuestAddress{arg(3)},32,checks);}if(!result)throw CertificateError{0x80092004};ERR_clear_error();
        }else if(symbol=="CryptVerifyCertificateSignatureEx" || symbol=="CryptVerifyCertificateSignature"){
            const bool extended=symbol.ends_with("Ex");if((u32(1)&65535)!=1)throw CertificateError{0x80070057};if(arg(0) || (extended && (arg(7) || (u32(6)&~1U))))throw CertificateError{50};
            const auto subject_type=extended?u32(2):1U,issuer_type=extended?u32(4):1U;const auto subject_pointer=arg(extended?3:2),issuer_pointer=arg(extended?5:4);std::vector<std::uint8_t> der;
            if(!extended)der=read_der(u32(1),subject_pointer,u32(3));else if(subject_type==1)der=blob(subject_pointer);
            else if(subject_type==2){const auto context=certs.contexts.find(subject_pointer);if(context==certs.contexts.end())throw CertificateError{87};der=context->second.der;}
            else if(subject_type==3){if(!subject_pointer)throw CertificateError{87};der=read_der(u32(1),memory.load(GuestAddress{subject_pointer+unit},bits),static_cast<std::uint32_t>(memory.load(GuestAddress{subject_pointer+2*unit},32)));}
            else throw CertificateError{50};
            std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key(nullptr,EVP_PKEY_free);Certificate issuer(nullptr,X509_free);
            if(issuer_type==1)key=public_key(issuer_pointer);
            else if(issuer_type==2 || issuer_type==3){auto pointer=issuer_pointer;if(issuer_type==3){const auto chain=certs.chains.find(pointer);if(chain==certs.chains.end() || chain->second.contexts.empty())throw CertificateError{87};pointer=chain->second.contexts.front();}
                const auto context=certs.contexts.find(pointer);if(context==certs.contexts.end())throw CertificateError{87};issuer=decode_certificate(context->second.der);key.reset(X509_get_pubkey(issuer.get()));if(!key)throw CertificateError{0x8009310b};}
            else throw CertificateError{50};
            Certificate certificate(nullptr,X509_free);std::unique_ptr<X509_CRL,decltype(&X509_CRL_free)> crl(nullptr,X509_CRL_free);std::unique_ptr<X509_REQ,decltype(&X509_REQ_free)> request(nullptr,X509_REQ_free);const auto* cursor=der.data();int signature_nid=0,verified=-1;
            if(subject_type!=3){certificate.reset(d2i_X509(nullptr,&cursor,static_cast<long>(der.size())));if(!certificate || cursor!=der.data()+der.size())certificate.reset();}
            if(!certificate && subject_type!=2){ERR_clear_error();cursor=der.data();crl.reset(d2i_X509_CRL(nullptr,&cursor,static_cast<long>(der.size())));if(!crl || cursor!=der.data()+der.size())crl.reset();}
            if(!certificate && !crl && subject_type==1){ERR_clear_error();cursor=der.data();request.reset(d2i_X509_REQ(nullptr,&cursor,static_cast<long>(der.size())));if(!request || cursor!=der.data()+der.size())request.reset();}
            if(certificate){signature_nid=X509_get_signature_nid(certificate.get());verified=X509_verify(certificate.get(),key.get());}
            else if(crl){signature_nid=X509_CRL_get_signature_nid(crl.get());verified=X509_CRL_verify(crl.get(),key.get());}
            else if(request){signature_nid=X509_REQ_get_signature_nid(request.get());verified=X509_REQ_verify(request.get(),key.get());}
            else throw CertificateError{0x8009310b};
            int digest_nid=0,key_nid=0;if(extended && (u32(6)&1U) && OBJ_find_sigid_algs(signature_nid,&digest_nid,&key_nid) && (digest_nid==NID_md2 || digest_nid==NID_md4))throw CertificateError{0x80090008};
            if(verified!=1)throw CertificateError{0x80090006};ERR_clear_error();result=1;
        }else if(symbol=="CertGetValidUsages"){
            if(u32(0)>4096 || (u32(0) && !arg(1)) || !arg(2) || !arg(4))throw CertificateError{87};
            memory.check(GuestAddress{arg(2)},4,Access::Write);memory.check(GuestAddress{arg(4)},4,Access::Write);const auto capacity=memory.load(GuestAddress{arg(4)},32);
            std::optional<KeyUsages> shared;
            for(unsigned i=0;i<u32(0);++i){const auto pointer=memory.load(GuestAddress{arg(1)+i*unit},bits);const auto context=certs.contexts.find(pointer);if(context==certs.contexts.end())throw CertificateError{87};
                auto usages=certificate_usages(context->second,certs.properties.at(context->second.property_identity),0);if(!usages)continue;
                if(!shared)shared=std::move(usages);else std::erase_if(*shared,[&](const auto& oid){return std::find(usages->begin(),usages->end(),oid)==usages->end();});
            }
            auto packet=usage_packet(x86_,shared.value_or(KeyUsages{}),false);memory.store(GuestAddress{arg(2)},32,shared?static_cast<std::uint32_t>(shared->size()):UINT32_MAX);memory.store(GuestAddress{arg(4)},32,packet.bytes.size());
            if(arg(3)){if(capacity<packet.bytes.size())throw CertificateError{234};packet.relocate(arg(3));memory.write(GuestAddress{arg(3)},packet.bytes);}last_error_=0;result=1;
        }else if(symbol=="CertGetEnhancedKeyUsage" || symbol=="CertSetEnhancedKeyUsage" || symbol=="CertAddEnhancedKeyUsageIdentifier" || symbol=="CertRemoveEnhancedKeyUsageIdentifier"){
            const auto context=certs.contexts.find(arg(0));if(context==certs.contexts.end())throw CertificateError{87};auto& properties=certs.properties.at(context->second.property_identity);
            if(symbol=="CertGetEnhancedKeyUsage"){
                if(!arg(3))throw CertificateError{87};memory.check(GuestAddress{arg(3)},4,Access::Write);const auto capacity=memory.load(GuestAddress{arg(3)},32);
                const auto usages=certificate_usages(context->second,properties,u32(1));auto packet=usage_packet(x86_,usages.value_or(KeyUsages{}),true);memory.store(GuestAddress{arg(3)},32,packet.bytes.size());
                if(arg(2)){if(capacity<packet.bytes.size())throw CertificateError{234};packet.relocate(arg(2));memory.write(GuestAddress{arg(2)},packet.bytes);}last_error_=usages?0U:0x80092004U;result=1;
            }else if(symbol=="CertSetEnhancedKeyUsage"){
                if(!arg(1))properties.erase(9);
                else {const auto count=memory.load(GuestAddress{arg(1)},32),table=memory.load(GuestAddress{arg(1)+unit},bits);if(count>4096 || (count && !table))throw CertificateError{87};KeyUsages usages;
                    for(std::uint64_t i=0;i<count;++i){const auto pointer=memory.load(GuestAddress{table+i*unit},bits);if(!pointer)throw CertificateError{87};usages.push_back(read_ansi(memory,pointer,1024));}
                    if(!properties.contains(9) && properties.size()>=1024)throw CertificateError{8};properties.insert_or_assign(9,encode_usage(usages));
                }result=1;
            }else {
                if(!arg(1))throw CertificateError{87};const auto identifier=read_ansi(memory,arg(1),1024);(void)encode_usage({identifier});
                auto found=properties.find(9);auto usages=found==properties.end()?KeyUsages{}:decode_usage(found->second);const auto existing=std::find(usages.begin(),usages.end(),identifier);
                if(symbol=="CertAddEnhancedKeyUsageIdentifier"){
                    if(existing==usages.end()){if(usages.size()>=4096 || (!properties.contains(9) && properties.size()>=1024))throw CertificateError{8};usages.push_back(identifier);properties.insert_or_assign(9,encode_usage(usages));}
                }else if(existing!=usages.end()){usages.erase(existing);if(usages.empty())properties.erase(9);else properties.insert_or_assign(9,encode_usage(usages));}
                result=1;
            }
        }else if(symbol=="CertGetCertificateContextProperty" || symbol=="CertSetCertificateContextProperty" || symbol=="CertEnumCertificateContextProperties" || symbol=="CertGetCRLContextProperty" || symbol=="CertSetCRLContextProperty" || symbol=="CertEnumCRLContextProperties"){
            const auto context=contexts.find(arg(0));if(context==contexts.end())throw CertificateError{87};auto& properties=certs.properties.at(context->second.property_identity);const auto id=u32(1);
            if(symbol.starts_with("CertEnum")){const auto next=properties.upper_bound(id);result=next==properties.end()?0:next->first;}
            else if(!id || id>65535)throw CertificateError{0x80070057};
            else if(id==1 || id==2 || id==5 || id==6){if(crl_api)throw CertificateError{0x80070057};result=certificate_key_property(memory,arg,symbol);}
            else if(symbol.starts_with("CertSet")){
                if(u32(2)&~0xc0000000U)throw CertificateError{50};if(!certificate_blob_property(id) && id!=27)throw CertificateError{50};
                if(!arg(3))properties.erase(id);else {auto data=id==27?std::vector<std::uint8_t>(8):blob(arg(3));if(id==27)memory.read(GuestAddress{arg(3)},data);
                    if((id==11 || id==13 || id==21 || id==30) && (data.size()<2 || (data.size()&1) || data[data.size()-1] || data[data.size()-2]))throw CertificateError{87};
                    if(!properties.contains(id) && properties.size()>=1024)throw CertificateError{8};properties.insert_or_assign(id,std::move(data));}result=1;
            }else {if(!arg(3))throw CertificateError{87};const auto capacity=memory.load(GuestAddress{arg(3)},32);auto found=properties.find(id);std::optional<std::vector<std::uint8_t>> computed;
                if(found==properties.end()){computed=certificate_computed_property(context->second,id,crl_api);if(!computed)throw CertificateError{0x80092004};if(id!=14)found=properties.emplace(id,*computed).first;}
                const auto& data=found==properties.end()?*computed:found->second;memory.store(GuestAddress{arg(3)},32,data.size());
                if(arg(2)){if(capacity<data.size())throw CertificateError{234};if(!data.empty())memory.write(GuestAddress{arg(2)},data);}result=1;
            }
        }else if(symbol=="CertVerifyCRLTimeValidity"){
            const auto when=arg(0)?memory.load(GuestAddress{arg(0)},64):runtime_.system_filetime();
            memory.check(GuestAddress{arg(1)},x86_?56:96,Access::Read);
            const auto first=memory.load(GuestAddress{arg(1)+6*unit},64),next=memory.load(GuestAddress{arg(1)+6*unit+8},64);
            result=when<first?UINT32_MAX:when>next?1:0;
        }else if(symbol=="CertVerifyCRLRevocation"){
            if((u32(0)&65535)!=1 || !arg(1) || u32(2)>4096 || (u32(2) && !arg(3)))throw CertificateError{87};
            auto serial=blob(arg(1)+unit);while(serial.size()>1 && !serial.back())serial.pop_back();const auto issuer=blob(arg(1)+6*unit);result=1;
            memory.check(GuestAddress{arg(3)},static_cast<std::size_t>(u32(2))*unit,Access::Read);
            for(unsigned i=0;i<u32(2) && result;++i){const auto info=memory.load(GuestAddress{arg(3)+static_cast<std::uint64_t>(i)*unit},bits);
                memory.check(GuestAddress{info},x86_?56:96,Access::Read);if(blob(info+4*unit)!=issuer)continue;
                const auto count=memory.load(GuestAddress{info+6*unit+16},32),entries=memory.load(GuestAddress{info+(x86_?44:72)},bits);const auto size=x86_?24U:40U;
                if(count>65536 || (count && !entries))throw CertificateError{87};memory.check(GuestAddress{entries},static_cast<std::size_t>(count)*size,Access::Read);
                for(std::uint64_t n=0;n<count;++n){auto revoked=blob(entries+n*size);while(revoked.size()>1 && !revoked.back())revoked.pop_back();if(revoked==serial){result=0;break;}}
            }
        }else if(symbol=="CertStrToNameA" || symbol=="CertStrToNameW"){
            if((u32(0)&65535)!=1 || !arg(1) || arg(3) || !arg(5))throw CertificateError{87};
            memory.check(GuestAddress{arg(5)},4,Access::Write);if(arg(6))memory.check(GuestAddress{arg(6)},unit,Access::Write);
            const bool wide=symbol.ends_with('W');const auto text=wide?read_utf16(memory,GuestAddress{arg(1)},65536):utf8_to_utf16_lossy(read_ansi(memory,arg(1),65536));
            try {
                const auto der=encode_certificate_name(text,u32(2));const auto capacity=memory.load(GuestAddress{arg(5)},32);
                if(arg(6))memory.store(GuestAddress{arg(6)},bits,0);memory.store(GuestAddress{arg(5)},32,der.size());
                if(arg(4)){if(capacity<der.size())throw CertificateError{234};memory.write(GuestAddress{arg(4)},der);}result=1;
            }catch(const CertificateNameError& error){
                if(arg(6)){const auto prefix=std::u16string_view(text).substr(0,error.offset);const auto offset=wide?2*error.offset:utf16_to_utf8(prefix).size();memory.store(GuestAddress{arg(6)},bits,arg(1)+offset);}
                throw CertificateError{error.code};
            }
        }else if(symbol=="CertGetNameStringA" || symbol=="CertGetNameStringW" || symbol=="CertNameToStrA" || symbol=="CertNameToStrW"){
            const bool context_name=symbol.starts_with("CertGet"),wide=symbol.ends_with('W');const auto output=arg(context_name?4:3);const auto capacity=u32(context_name?5:4);std::u16string text;
            try {
                if(context_name){const auto context=certs.contexts.find(arg(0));if(context==certs.contexts.end())throw CertificateError{87};auto cert=decode_certificate(context->second.der);
                    std::string attribute;std::uint32_t format=1;if(u32(1)==3){if(!arg(3))throw CertificateError{87};attribute=read_ansi(memory,arg(3),1024);}else if(u32(1)==2 && arg(3))format=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));
                    text=certificate_name(cert.get(),u32(1),u32(2),attribute,format);
                    if(u32(1)==5 && !(u32(2)&2U)){const auto& properties=certs.properties.at(context->second.property_identity);if(const auto found=properties.find(11);found!=properties.end()){text.clear();const auto& data=found->second;for(std::size_t i=0;i+1<data.size();i+=2){const auto c=static_cast<char16_t>(data[i]|(static_cast<unsigned>(data[i+1])<<8));if(!c)break;text.push_back(c);}}}
                }else {if((u32(0)&65535)!=1)throw CertificateError{87};const auto der=blob(arg(1));const auto* cursor=der.data();std::unique_ptr<X509_NAME,decltype(&X509_NAME_free)> name(d2i_X509_NAME(nullptr,&cursor,static_cast<long>(der.size())),X509_NAME_free);
                    if(!name || cursor!=der.data()+der.size())throw CertificateError{0x8009310b};text=certificate_rdn(name.get(),u32(2));}
            }catch(const CertificateError& error){last_error_=error.code;text.clear();}
            auto bytes=wide?utf16_bytes(text):std::vector<std::uint8_t>{};if(!wide){const auto utf8=utf16_to_utf8(text);bytes.assign(utf8.begin(),utf8.end());bytes.push_back(0);}
            const unsigned width=wide?2:1;result=bytes.size()/width;if(output && capacity){const auto count=std::min<std::uint64_t>(capacity,result);bytes.resize(static_cast<std::size_t>(count)*width);std::fill(bytes.end()-width,bytes.end(),0);memory.write(GuestAddress{output},bytes);result=count;}
        }else if(symbol=="CertCompareCertificateName"){
            if((u32(0)&65535)!=1)throw CertificateError{87};result=blob(arg(1))==blob(arg(2));
        }else if(symbol=="CertFindExtension"){
            if(!arg(0))throw CertificateError{87};result=extension(read_ansi(memory,arg(0),1024),u32(1),arg(2));
        }else if(symbol=="CertVerifyTimeValidity"){
            if(!arg(1))throw CertificateError{87};const auto time=arg(0)?memory.load(GuestAddress{arg(0)},64):runtime_.system_filetime();
            const auto before=memory.load(GuestAddress{arg(1)+8*unit},64),after=memory.load(GuestAddress{arg(1)+8*unit+8},64);result=time<before?UINT32_MAX:time>after?1U:0U;
        }else if(symbol=="CertGetIntendedKeyUsage"){
            if((u32(0)&65535)!=1 || !arg(1) || (u32(3) && !arg(2)) || u32(3)>65536)throw CertificateError{87};memory.check(GuestAddress{arg(2)},u32(3),Access::Write);
            const auto count=memory.load(GuestAddress{arg(1)+(x86_?104:192)},32),pointer=memory.load(GuestAddress{arg(1)+(x86_?108:200)},bits);
            auto entry=extension("2.5.29.15",count,pointer);bool legacy=false;if(!entry){entry=extension("2.5.29.2",count,pointer);legacy=true;}
            std::vector<std::uint8_t> output(u32(3));if(!entry){last_error_=0;}
            else{const auto der=blob(entry+2*unit);const auto* cursor=der.data();const ASN1_BIT_STRING* usage=nullptr;
                std::unique_ptr<ASN1_BIT_STRING,decltype(&ASN1_BIT_STRING_free)> modern(nullptr,ASN1_BIT_STRING_free);
                const auto release=[](STACK_OF(ASN1_TYPE)* values){sk_ASN1_TYPE_pop_free(values,ASN1_TYPE_free);};std::unique_ptr<STACK_OF(ASN1_TYPE),decltype(release)> fields(nullptr,release);
                if(!legacy){modern.reset(d2i_ASN1_BIT_STRING(nullptr,&cursor,static_cast<long>(der.size())));usage=modern.get();if(!usage)throw CertificateError{0x8009310b};}
                else{fields.reset(d2i_ASN1_SEQUENCE_ANY(nullptr,&cursor,static_cast<long>(der.size())));if(!fields)throw CertificateError{0x8009310b};
                    for(int i=0;i<sk_ASN1_TYPE_num(fields.get());++i){const auto* field=sk_ASN1_TYPE_value(fields.get(),i);if(field->type==V_ASN1_BIT_STRING){if(usage)throw CertificateError{0x8009310b};usage=field->value.bit_string;}}}
                if(cursor!=der.data()+der.size())throw CertificateError{0x8009310b};
                if(usage){const auto size=ASN1_STRING_length(usage);if(size<0)throw CertificateError{0x8009310b};const auto copied=std::min<std::size_t>(output.size(),static_cast<unsigned>(size));if(copied)std::copy_n(ASN1_STRING_get0_data(usage),copied,output.begin());result=1;}else last_error_=0;
            }
            if(!output.empty())memory.write(GuestAddress{arg(2)},output);
        }else if(symbol=="CertFreeCertificateChain" || symbol=="CertDuplicateCertificateChain"){
            const auto chain=certs.chains.find(arg(0));
            if(chain==certs.chains.end()){if(arg(0) || symbol=="CertDuplicateCertificateChain")fail(87);}
            else if(symbol=="CertDuplicateCertificateChain"){if(chain->second.references==UINT32_MAX)fail(8);else {++chain->second.references;result=arg(0);}}
            else (void)free_certificate_chain(state,memory,arg(0));
        }else if(symbol=="CertGetCertificateChain"){
            if(!arg(7))throw CertificateError{87};memory.store(GuestAddress{arg(7)},x86_?32:64,0);
            const auto context=certs.contexts.find(arg(1));if(context==certs.contexts.end() || !arg(4) || arg(6))throw CertificateError{87};if(arg(0)>1 && !certs.engines.contains(arg(0)))throw CertificateError{6};
            const auto flags=u32(5),revocation=flags&0x70000000U;if((flags&~0xf8003945U) || (revocation && (revocation&(revocation-1))))throw CertificateError{50};
            const unsigned usage=x86_?4:8,usage_size=x86_?12:24;const auto size=memory.load(GuestAddress{arg(4)},32);
            if(size<usage+usage_size || size>4096)throw CertificateError{87};memory.check(GuestAddress{arg(4)},static_cast<std::size_t>(size),Access::Read);
            const auto usage_type=memory.load(GuestAddress{arg(4)+usage},32),count=memory.load(GuestAddress{arg(4)+usage+unit},32),oids=memory.load(GuestAddress{arg(4)+usage+2*unit},x86_?32:64);
            if(usage_type>1 || count>64 || (count && !oids))throw CertificateError{87};std::vector<std::string> requested;
            for(std::uint64_t i=0;i<count;++i)requested.push_back(read_ansi(memory,memory.load(GuestAddress{oids+i*unit},x86_?32:64),1024));
            if(size>=usage+2*usage_size && memory.load(GuestAddress{arg(4)+usage+usage_size+unit},32))throw CertificateError{50};
            if(size>=(x86_?36U:64U) && memory.load(GuestAddress{arg(4)+(x86_?32U:60U)},32))throw CertificateError{50};
            if(size>=(x86_?48U:88U) && memory.load(GuestAddress{arg(4)+(x86_?44U:80U)},x86_?32:64))throw CertificateError{50};
            const auto chain=build_certificate_chain(state,memory,arg(0),arg(1),arg(2)?memory.load(GuestAddress{arg(2)},64):runtime_.system_filetime(),arg(3),requested,usage_type==1,flags,false);
            memory.store(GuestAddress{arg(7)},bits,chain);result=1;
#include "certificate_store_save_handler.inc"
        }else if(symbol=="CertOpenStore" || symbol=="CertOpenSystemStoreA" || symbol=="CertOpenSystemStoreW"){
            const bool simple=symbol!="CertOpenStore";const auto provider=!simple && arg(0)>65535?read_ansi(memory,arg(0),128):std::string{};
            const bool system=simple || arg(0)==9 || arg(0)==10 || provider=="System";
            const bool memory_provider=!simple && (arg(0)==2 || provider=="Memory");
            const bool message_provider=!simple && arg(0)==1,pkcs7_provider=!simple && (arg(0)==5 || provider=="PKCS7");
            const bool collection_provider=!simple && (arg(0)==11 || provider=="Collection");
            if(!system && !memory_provider && !message_provider && !pkcs7_provider && !collection_provider)fail(50);
            else if(certs.stores.size()>=128)fail(8);
            else if(message_provider || pkcs7_provider){
                if(u32(1)!=0x10001 || arg(2) || (u32(3)&~0x8205U))throw CertificateError{50};
                std::vector<std::uint8_t> input;
                if(message_provider){const auto found=certs.messages.find(arg(4));if(found==certs.messages.end())throw CertificateError{6};if(!found->second.finalized)throw CertificateError{0x80091001};input=found->second.encoded;}
                else input=blob(arg(4));
                auto message=decode_message(input);if(!PKCS7_type_is_signed(message.get()))throw CertificateError{0x80091004};
                CertificateStore store;store.readonly=(u32(3)&0x8000U)!=0;store.enum_archived=(u32(3)&0x200U)!=0;
                if(message->d.sign->cert){const auto count=sk_X509_num(message->d.sign->cert);if(count>4096)throw CertificateError{8};for(int i=0;i<count;++i){const auto identity=store.next_identity++,property=certs.next_property_identity++;store.items.emplace(identity,encoded(sk_X509_value(message->d.sign->cert,i),i2d_X509));store.property_identities.emplace(identity,property);certs.properties.emplace(property,CertificateProperties{});}}
                copy_store_crls(certs,store,message_crls(message.get()));
                result=runtime_.allocate_wait_handle();certs.stores.emplace(result,std::move(store));
            }
            else if(system){
                const auto flags=simple?0x10000U:u32(3);const auto location=flags&0xff0000U;const auto pointer=simple?arg(1):arg(4);
                if((simple?arg(0):arg(2)) || (flags&~0x3c000U) || (location!=0x10000U && location!=0x20000U))throw CertificateError{50};if(!pointer)throw CertificateError{87};
                auto name=(simple?symbol.ends_with('W'):arg(0)!=9)?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer));for(auto& c:name)c=unicode_case_unit(c,true);
                if(name!=u"ROOT")throw CertificateError{50};
                CertificateStore store;store.readonly=true;store.root_provider=true;const auto roots=certificate_roots(filesystem_);
                for(const auto& root:roots){const auto identity=store.next_identity++,property=certs.next_property_identity++;store.items.emplace(identity,encoded(root.get(),i2d_X509));store.property_identities.emplace(identity,property);certs.properties.emplace(property,CertificateProperties{});}
                result=runtime_.allocate_wait_handle();certs.stores.emplace(result,std::move(store));
            }else if(arg(2) || (u32(3)&~0xb205U))fail(50);
            else {result=runtime_.allocate_wait_handle();CertificateStore store;store.collection=collection_provider;store.readonly=(u32(3)&0x8000U)!=0;store.enum_archived=(u32(3)&0x200U)!=0;certs.stores.emplace(result,std::move(store));}
        }else if(symbol=="CertAddStoreToCollection"){
            const auto collection=certs.stores.find(arg(0));if(collection==certs.stores.end() || !collection->second.references || !collection->second.collection)throw CertificateError{0x80070057};
            if(u32(2)&~1U)throw CertificateError{87};if(!arg(1))result=1;
            else {const auto sibling=certs.stores.find(arg(1));if(sibling==certs.stores.end() || !sibling->second.references)throw CertificateError{0x80070057};
                if(store_contains(certs,arg(1),arg(0)))throw CertificateError{87};if(collection->second.siblings.size()>=128 || sibling->second.collection_references==UINT32_MAX)throw CertificateError{8};
                auto& value=collection->second;const CertificateSibling edge{arg(1),value.next_sibling_identity,u32(3),(u32(2)&1U)!=0};
                const auto at=std::find_if(value.siblings.begin(),value.siblings.end(),[&](const auto& item){return edge.priority>item.priority;});
                value.siblings.insert(at,edge);++value.next_sibling_identity;++sibling->second.collection_references;result=1;
            }
        }else if(symbol=="CertRemoveStoreFromCollection"){
            const auto collection=certs.stores.find(arg(0));if(collection!=certs.stores.end() && collection->second.collection){auto& siblings=collection->second.siblings;
                const auto found=std::find_if(siblings.begin(),siblings.end(),[&](const auto& item){return item.store==arg(1);});
                if(found!=siblings.end()){--certs.stores.at(found->store).collection_references;siblings.erase(found);prune_stores(certs);prune_certificate_properties();}}
        }else if(symbol=="CertControlStore"){
            const auto store=certs.stores.find(arg(0));if(store==certs.stores.end() || !store->second.references)throw CertificateError{6};
            const auto control=u32(2),flags=u32(1);if(!control || control>5 || (flags&~(control==3?3U:control==2?1U:0U)))throw CertificateError{87};
            if(control==1){if(arg(3))throw CertificateError{50};resync_store(certs,arg(0),filesystem_);prune_certificate_properties();result=1;}
            else if(control==4){if(arg(3))throw CertificateError{87};resync_store(certs,arg(0),filesystem_);store->second.auto_resync=true;prune_certificate_properties();result=1;}
            else if(control==3){if(arg(3))throw CertificateError{87};throw CertificateError{store->second.root_provider?0x80070005U:50U};}
            else throw CertificateError{50}; // 原生 ROOT 尚未提供外部變更事件後端，不登錄虛假的通知。
        }else if(symbol=="CertDuplicateStore"){
            const auto store=certs.stores.find(arg(0));if(store==certs.stores.end() || !store->second.references)fail(6);else if(store->second.references==UINT32_MAX)fail(8);else {++store->second.references;result=arg(0);}
        }else if(symbol=="CertCloseStore"){
            const auto store=certs.stores.find(arg(0));if(store==certs.stores.end() || !store->second.references)fail(6);else if(u32(1)&~3U)fail(87);
            else {const auto handle=arg(0);--store->second.references;std::vector<std::pair<std::uint64_t,bool>> active;
                for(const bool crl:{false,true})for(const auto& [pointer,context]:crl?certs.crl_contexts:certs.contexts)if(context.store==handle)active.emplace_back(pointer,crl);
                if(u32(1)&1U){for(const auto& [pointer,crl]:active){(crl?certs.crl_contexts:certs.contexts).at(pointer).references=1;(void)free_certificate(state,memory,pointer,crl);}active.clear();}
                prune_stores(certs);
                prune_certificate_properties();if((u32(1)&2U) && !active.empty())fail(0x8009200f);else result=1;}
        }else if(symbol=="CertCreateCertificateContext" || symbol=="CertCreateCRLContext")result=create_certificate(memory,read_der(u32(0),arg(1),u32(2)),0,0,0,0,crl_api);
        else if(symbol=="CertDuplicateCertificateContext" || symbol=="CertDuplicateCRLContext"){
            const auto context=contexts.find(arg(0));if(context==contexts.end())fail(87);else if(context->second.references==UINT32_MAX)fail(8);else {++context->second.references;result=arg(0);}
        }else if(symbol=="CertFreeCertificateContext" || symbol=="CertFreeCRLContext"){if(free_certificate(state,memory,arg(0),crl_api))result=1;else fail(87);}
        else if(symbol=="CertEnumCertificatesInStore" || symbol=="CertEnumCRLsInStore"){
            std::uint64_t previous=0;bool valid=true;std::shared_ptr<const std::vector<std::uint64_t>> order;std::size_t cursor=0;
            if(arg(1)){const auto context=contexts.find(arg(1));if(context==contexts.end())valid=false;else {if(context->second.store!=arg(0))valid=false;else {previous=context->second.identity;order=context->second.enumeration;cursor=context->second.enumeration_cursor;}(void)free_certificate(state,memory,arg(1),crl_api);}}
            const auto store=certs.stores.find(arg(0));
            if(!valid || store==certs.stores.end() || !store->second.references)fail(crl_api?0x80070057:87);
            else {if(!previous)auto_resync_store(certs,arg(0),filesystem_);const auto entries=store_entries(certs,arg(0),0,crl_api);
                if(!order){auto snapshot=std::make_shared<std::vector<std::uint64_t>>();snapshot->reserve(entries.size());bool after=!previous;
                    for(const auto& item:entries){if(!after){if(item.identity==previous){after=true;continue;}if(!store->second.collection && item.identity>previous)after=true;else continue;}snapshot->push_back(item.identity);}order=std::move(snapshot);}
                // 前一項被刪除時仍沿原列舉順序前進；不複製每個 context 的尾端清單。
                for(;cursor<order->size();++cursor){const auto item=std::find_if(entries.begin(),entries.end(),[&](const auto& value){return value.identity==(*order)[cursor];});
                    if(item==entries.end() || (!store->second.enum_archived && certs.properties.at(item->properties).contains(19)))continue;
                    result=create_certificate(memory,*item->der,arg(0),item->identity,item->store,item->source_identity,crl_api);
                    contexts.at(result).enumeration=order;contexts.at(result).enumeration_cursor=cursor+1;break;
                }if(!result)fail(0x80092004);}
        }else if(symbol=="CertDeleteCertificateFromStore" || symbol=="CertDeleteCRLFromStore"){
            const auto context=contexts.find(arg(0));if(context==contexts.end())fail(87);
            else {const auto source=context->second.source_store?context->second.source_store:context->second.store,identity=context->second.source_store?context->second.source_identity:context->second.identity;const auto store=certs.stores.find(source);if(store==certs.stores.end())result=1;else if(store->second.readonly)fail(0x80070005);else {(crl_api?store->second.crls:store->second.items).erase(identity);(crl_api?store->second.crl_property_identities:store->second.property_identities).erase(identity);result=1;}(void)free_certificate(state,memory,arg(0),crl_api);}
        }else if(symbol=="CertAddCertificateContextToStore" || symbol=="CertAddEncodedCertificateToStore" || symbol=="CertAddCRLContextToStore" || symbol=="CertAddEncodedCRLToStore"){
            const bool encoded_input=symbol.starts_with("CertAddEncoded");const auto disposition=u32(encoded_input?4:2);const auto output=arg(encoded_input?5:3);if(output)memory.store(GuestAddress{output},x86_?32:64,0);
            const auto store=certs.stores.find(arg(0));if(store==certs.stores.end() || !store->second.references)throw CertificateError{87};if(store->second.readonly)throw CertificateError{0x80070005};if(!disposition || disposition>7)throw CertificateError{0x80070057};
            std::vector<std::uint8_t> der;CertificateProperties incoming;
            if(encoded_input)der=read_der(u32(1),arg(2),u32(3));else {const auto context=contexts.find(arg(1));if(context==contexts.end())throw CertificateError{87};der=context->second.der;incoming=certs.properties.at(context->second.property_identity);incoming.erase(5);}
            if(crl_api)(void)decode_crl(der);else (void)decode_certificate(der);std::vector<StoreTarget> targets;write_targets(certs,arg(0),targets);if(targets.empty())throw CertificateError{0x80070005};
            std::uint32_t last=0x80070005;bool added=false;
            for(const auto& target:targets){try {
                auto& destination=certs.stores.at(target.store);auto& items=crl_api?destination.crls:destination.items;auto& identities=crl_api?destination.crl_property_identities:destination.property_identities;
                auto values=incoming;auto existing=items.end();
                const auto same=[&](const auto& old){if(crl_api){auto a=decode_crl(der),b=decode_crl(old);return X509_NAME_cmp(X509_CRL_get_issuer(a.get()),X509_CRL_get_issuer(b.get()))==0 && (X509_CRL_get_ext_by_NID(a.get(),NID_delta_crl,-1)>=0)==(X509_CRL_get_ext_by_NID(b.get(),NID_delta_crl,-1)>=0);}auto a=decode_certificate(der),b=decode_certificate(old);return X509_issuer_and_serial_cmp(a.get(),b.get())==0;};
                const auto date=[&](const auto& value){if(crl_api)return certificate_time(X509_CRL_get0_lastUpdate(decode_crl(value).get()));return certificate_time(X509_get0_notBefore(decode_certificate(value).get()));};
                if(disposition!=4)for(auto it=items.begin();it!=items.end();++it)if(same(it->second)){existing=it;break;}
                if(existing!=items.end() && (disposition==1 || (disposition>=6 && date(der)<=date(existing->second))))throw CertificateError{0x80092005};
                const bool reuse=existing!=items.end() && disposition==2;const auto identity=reuse?existing->first:destination.next_identity;
                if(!reuse && items.size()>=4096)throw CertificateError{8};const auto& selected=reuse?existing->second:der;
                if(existing!=items.end() && (reuse || disposition==5 || disposition==7)){auto inherited=certs.properties.at(identities.at(existing->first));if(!reuse)inherited.erase(5);if(reuse){for(auto& [id,value]:values)inherited.insert_or_assign(id,std::move(value));values=std::move(inherited);}else for(auto& [id,value]:inherited)values.insert_or_assign(id,std::move(value));}
                // 先配置可移入的節點，再發布 context／store，失敗不刪掉原有資料。
                std::map<std::uint64_t,std::vector<std::uint8_t>> pending_items;std::map<std::uint64_t,std::uint64_t> pending_ids;
                if(!reuse){pending_items.emplace(identity,der);pending_ids.emplace(identity,0);}
                const auto view=target_identity(certs,target,identity);std::uint64_t context=0;bool committed=false;
                try{
                    if(output)context=create_certificate(memory,selected,arg(0),view,target.store,identity,crl_api);
                    const auto properties=reuse?identities.at(identity):context?contexts.at(context).property_identity:certs.next_property_identity++;
                    if(context || reuse)certs.properties.at(properties).swap(values);else certs.properties.emplace(properties,std::move(values));
                    if(!reuse){pending_ids.at(identity)=properties;if(existing!=items.end()){identities.erase(existing->first);items.erase(existing);}
                        items.insert(pending_items.extract(identity));identities.insert(pending_ids.extract(identity));++destination.next_identity;}
                    committed=true;prune_certificate_properties();if(output)memory.store(GuestAddress{output},x86_?32:64,context);
                }catch(...){if(context && !committed)(void)free_certificate(state,memory,context,crl_api);throw;}
                result=1;added=true;break;
            }catch(const CertificateError& error){last=error.code;}}
            if(!added)throw CertificateError{last};
        }else throw HostCallFault("尚未實作憑證服務："+std::string(symbol));
    }catch(const CertificateError& error){fail(error.code);}catch(const MemoryFault&){fail(87);}catch(const std::bad_alloc&){fail(8);}
    arg.return_to(state,result);return std::nullopt;
}
}
