#include "pe_signature.hpp"
#include "pe_signature_roots.hpp"
#include "sandbox_certificate_roots.hpp"
#include <algorithm>
#include <array>
#include <ctime>
#include <climits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>
#include <openssl/cms.h>
#include <openssl/ts.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
namespace x64arm {
namespace {
using Bytes=std::span<const std::uint8_t>;
template<class T,auto release>using Ref=std::unique_ptr<T,decltype(release)>;
struct Failure:std::runtime_error{std::uint32_t status;explicit Failure(const char* reason,std::uint32_t code=0x800b0003U):std::runtime_error(reason),status(code){}};
struct Reader {
    Bytes bytes;
    Bytes at(std::size_t offset,std::size_t size)const{if(offset>bytes.size() || size>bytes.size()-offset)throw Failure("資料截斷");return bytes.subspan(offset,size);}
    std::uint32_t number(std::size_t offset,unsigned size)const{std::uint32_t value=0;const auto data=at(offset,size);for(unsigned i=0;i<size;++i)value|=std::uint32_t{data[i]}<<(i*8);return value;}
};
Bytes sequence(Bytes input){
    const auto* cursor=input.data();long length=0;int tag=0,kind=0;
    if(input.size()>LONG_MAX || ASN1_get_object(&cursor,&length,&tag,&kind,static_cast<long>(input.size()))!=V_ASN1_CONSTRUCTED || tag!=V_ASN1_SEQUENCE || kind!=V_ASN1_UNIVERSAL || length<0 || static_cast<std::size_t>(length)!=input.size()-static_cast<std::size_t>(cursor-input.data()))throw Failure("ASN.1 SEQUENCE 不合法");
    return {cursor,static_cast<std::size_t>(length)};
}
Bytes first_tlv(Bytes& input){
    const auto* cursor=input.data();long length=0;int tag=0,kind=0;
    if(input.empty() || input.size()>LONG_MAX || (ASN1_get_object(&cursor,&length,&tag,&kind,static_cast<long>(input.size()))&0x81) || length<0)throw Failure("ASN.1 欄位不合法");
    const auto header=static_cast<std::size_t>(cursor-input.data());if(header>input.size() || static_cast<std::size_t>(length)>input.size()-header)throw Failure("ASN.1 欄位截斷");
    const auto result=input.first(header+static_cast<std::size_t>(length));input=input.subspan(result.size());return result;
}
Ref<X509_STORE,X509_STORE_free> store(std::string_view roots,bool timestamp=false,bool defaults=false){
    if(defaults && roots.empty())roots=pe_signature_detail::roots_pem;
    Ref<X509_STORE,X509_STORE_free> result(X509_STORE_new(),X509_STORE_free);if(!result || roots.size()>INT_MAX)throw Failure("建立信任存放區失敗");
    if(!add_certificate_root_pem(result.get(),roots))throw Failure("讀取信任根失敗");
    if(!X509_STORE_set_purpose(result.get(),timestamp?X509_PURPOSE_TIMESTAMP_SIGN:X509_PURPOSE_ANY))throw Failure("設定憑證用途失敗");return result;
}
bool oid(const ASN1_OBJECT* object,std::string_view expected){std::array<char,96> value{};const auto size=OBJ_obj2txt(value.data(),static_cast<int>(value.size()),object,1);return size>=0 && static_cast<std::size_t>(size)<value.size() && std::string_view(value.data(),static_cast<std::size_t>(size))==expected;}
bool eku(X509* certificate,std::string_view expected){
    Ref<EXTENDED_KEY_USAGE,EXTENDED_KEY_USAGE_free> usage(static_cast<EXTENDED_KEY_USAGE*>(X509_get_ext_d2i(certificate,NID_ext_key_usage,nullptr,nullptr)),EXTENDED_KEY_USAGE_free);
    if(!usage)return false;for(int i=0;i<sk_ASN1_OBJECT_num(usage.get());++i)if(oid(sk_ASN1_OBJECT_value(usage.get(),i),expected))return true;return false;
}
bool subject(X509* certificate,int nid,std::string_view expected){
    auto* name=X509_get_subject_name(certificate);int index=-1;
    while((index=X509_NAME_get_index_by_NID(name,nid,index))>=0){auto* entry=X509_NAME_get_entry(name,index);unsigned char* text=nullptr;const auto size=ASN1_STRING_to_UTF8(&text,X509_NAME_ENTRY_get_data(entry));
        const bool matches=size>=0 && std::string_view(reinterpret_cast<const char*>(text),static_cast<std::size_t>(size))==expected;OPENSSL_free(text);if(matches)return true;}
    return false;
}
Bytes signed_content(PKCS7* message){
    if(!PKCS7_type_is_signed(message) || !message->d.sign || !message->d.sign->contents)throw Failure("PKCS#7 非 SignedData");
    auto* inner=message->d.sign->contents;
    if(!oid(inner->type,"1.3.6.1.4.1.311.2.1.4") || !inner->d.other || inner->d.other->type!=V_ASN1_SEQUENCE)throw Failure("非 Authenticode 內容");
    auto* value=inner->d.other->value.sequence;const auto length=ASN1_STRING_length(value);if(length<=0)throw Failure("Authenticode 內容為空");return {ASN1_STRING_get0_data(value),static_cast<std::size_t>(length)};
}
bool verify(PKCS7* message,X509_STORE* trust,Bytes contents,bool indirect,int flags=0){
#if OPENSSL_VERSION_NUMBER < 0x30500000L
    if(indirect){const auto payload=sequence(contents);if(payload.size()>INT_MAX)return false;Ref<BIO,BIO_free> data(BIO_new_mem_buf(payload.data(),static_cast<int>(payload.size())),BIO_free);return data && PKCS7_verify(message,nullptr,trust,data.get(),nullptr,PKCS7_BINARY|flags)==1;}
#else
    (void)contents;(void)indirect;
#endif
    return PKCS7_verify(message,nullptr,trust,nullptr,nullptr,PKCS7_BINARY|flags)==1;
}
std::optional<std::time_t> timestamp_time(const ASN1_TIME* value){
    if(!value)return {};
    Ref<ASN1_TIME,ASN1_TIME_free> seconds(nullptr,ASN1_TIME_free);
    if(ASN1_STRING_type(value)==V_ASN1_GENERALIZEDTIME){
        const auto size=ASN1_STRING_length(value);if(size<15)return {};
        const std::string_view text(reinterpret_cast<const char*>(ASN1_STRING_get0_data(value)),static_cast<std::size_t>(size));
        if(text.back()!='Z')return {};
        if(size>15){
            // RFC 3161 允許小數秒；X.509 的 ASN1_TIME_to_tm 僅接受整秒。
            if(size<17 || text[14]!='.' || text[text.size()-2]=='0' || !std::all_of(text.begin()+15,text.end()-1,[](char c){return c>='0' && c<='9';}))return {};
            const auto whole=std::string(text.substr(0,14))+"Z";seconds.reset(ASN1_TIME_new());
            if(!seconds || ASN1_TIME_set_string_X509(seconds.get(),whole.c_str())!=1)return {};
            value=seconds.get();
        }
    }
    std::tm tm{};if(ASN1_TIME_to_tm(value,&tm)!=1)return {};const auto when=timegm(&tm);
    if(when<0 || when>std::time(nullptr)+300)return {};return when;
}
bool timestamp_chain(X509* certificate,STACK_OF(X509)* certificates,std::string_view roots,std::time_t when){
    auto trust=store(roots,true);
    Ref<X509_STORE_CTX,X509_STORE_CTX_free> chain(X509_STORE_CTX_new(),X509_STORE_CTX_free);
    if(!certificate || !chain || !X509_STORE_CTX_init(chain.get(),trust.get(),certificate,certificates))return false;
    X509_VERIFY_PARAM_set_time(X509_STORE_CTX_get0_param(chain.get()),when);
    return X509_STORE_CTX_set_purpose(chain.get(),X509_PURPOSE_TIMESTAMP_SIGN) && X509_verify_cert(chain.get())==1;
}
std::optional<std::time_t> timestamp(PKCS7* message,PKCS7_SIGNER_INFO* signer,std::string_view roots){
    if(!signer->unauth_attr)return {};
    for(int i=0;i<sk_X509_ATTRIBUTE_num(signer->unauth_attr);++i){auto* attribute=sk_X509_ATTRIBUTE_value(signer->unauth_attr,i);
        const bool legacy=oid(X509_ATTRIBUTE_get0_object(attribute),"1.2.840.113549.1.9.6");
        if(!legacy && !oid(X509_ATTRIBUTE_get0_object(attribute),"1.3.6.1.4.1.311.3.3.1"))continue;
        for(int n=0;n<X509_ATTRIBUTE_count(attribute);++n){try{
            auto* value=X509_ATTRIBUTE_get0_type(attribute,n);if(!value || value->type!=V_ASN1_SEQUENCE)continue;auto* data=value->value.sequence;const auto* cursor=ASN1_STRING_get0_data(data);const auto length=ASN1_STRING_length(data);if(length<=0)continue;
            if(legacy){
                const auto* start=cursor;Ref<PKCS7_SIGNER_INFO,PKCS7_SIGNER_INFO_free> counter(d2i_PKCS7_SIGNER_INFO(nullptr,&cursor,length),PKCS7_SIGNER_INFO_free);
                if(!counter || cursor!=start+length || !counter->auth_attr)continue;
                ASN1_TYPE* signing_time=nullptr;unsigned times{},digests{},types{};bool valid_type=true;
                for(int at=0;at<sk_X509_ATTRIBUTE_num(counter->auth_attr);++at){auto* item=sk_X509_ATTRIBUTE_value(counter->auth_attr,at);const auto id=OBJ_obj2nid(X509_ATTRIBUTE_get0_object(item));
                    if(id==NID_pkcs9_signingTime){++times;if(X509_ATTRIBUTE_count(item)==1)signing_time=X509_ATTRIBUTE_get0_type(item,0);}
                    if(id==NID_pkcs9_messageDigest)digests+=static_cast<unsigned>(X509_ATTRIBUTE_count(item));
                    if(id==NID_pkcs9_contentType){++types;auto* type=X509_ATTRIBUTE_count(item)==1?X509_ATTRIBUTE_get0_type(item,0):nullptr;valid_type=type && type->type==V_ASN1_OBJECT && OBJ_obj2nid(type->value.object)==NID_pkcs7_data;}
                }
                if(times!=1 || digests!=1 || types>1 || !valid_type || !signing_time || (signing_time->type!=V_ASN1_UTCTIME && signing_time->type!=V_ASN1_GENERALIZEDTIME))continue;
                const auto when=timestamp_time(signing_time->value.asn1_string);if(!when || !timestamp_chain(PKCS7_cert_from_signer_info(message,counter.get()),message->d.sign->cert,roots,*when))continue;
                const auto* md=EVP_get_digestbyobj(counter->digest_alg->algorithm);if(!md || EVP_MD_get_size(md)<20)continue;
                Ref<BIO,BIO_free_all> digest(BIO_new(BIO_f_md()),BIO_free_all);Ref<BIO,BIO_free> sink(BIO_new(BIO_s_null()),BIO_free);
                if(!digest || !sink || BIO_set_md(digest.get(),md)!=1 || !BIO_push(digest.get(),sink.get()))continue;(void)sink.release();
                const auto size=ASN1_STRING_length(signer->enc_digest);
                if(size<=0 || BIO_write(digest.get(),ASN1_STRING_get0_data(signer->enc_digest),size)!=size)continue;
                if(PKCS7_signatureVerify(digest.get(),message,counter.get(),PKCS7_cert_from_signer_info(message,counter.get()))==1)return when;
                continue;
            }
            // RFC 3161 使用 CMS CertificateChoices，可攜帶非 X.509 憑證項；
            // 舊 PKCS7 的單一 X509 型別會在解碼階段拒絕整份合法 token。
            const auto* begin=cursor;Ref<CMS_ContentInfo,CMS_ContentInfo_free> token(d2i_CMS_ContentInfo(nullptr,&cursor,length),CMS_ContentInfo_free);
            if(!token || cursor!=begin+length || OBJ_obj2nid(CMS_get0_type(token.get()))!=NID_pkcs7_signed || !oid(CMS_get0_eContentType(token.get()),"1.2.840.113549.1.9.16.1.4"))continue;
            auto** contents=CMS_get0_content(token.get());if(!contents || !*contents)continue;
            auto* content=*contents;cursor=ASN1_STRING_get0_data(content);begin=cursor;const auto content_size=ASN1_STRING_length(content);if(content_size<=0)continue;
            Ref<TS_TST_INFO,TS_TST_INFO_free> info(d2i_TS_TST_INFO(nullptr,&cursor,content_size),TS_TST_INFO_free);if(!info || cursor!=begin+content_size)continue;
            auto* imprint=TS_TST_INFO_get_msg_imprint(info.get());if(!imprint)continue;auto* algorithm=TS_MSG_IMPRINT_get_algo(imprint);const ASN1_OBJECT* object=nullptr;X509_ALGOR_get0(&object,nullptr,nullptr,algorithm);const auto* md=EVP_get_digestbyobj(object);if(!md)continue;
            std::array<unsigned char,EVP_MAX_MD_SIZE> hash{};unsigned size=0;auto* signature=signer->enc_digest;auto* expected=TS_MSG_IMPRINT_get_msg(imprint);
            if(EVP_Digest(ASN1_STRING_get0_data(signature),static_cast<std::size_t>(ASN1_STRING_length(signature)),hash.data(),&size,md,nullptr)!=1 || size!=static_cast<unsigned>(ASN1_STRING_length(expected)) || CRYPTO_memcmp(hash.data(),ASN1_STRING_get0_data(expected),size))continue;
            const auto when=timestamp_time(TS_TST_INFO_get_time(info.get()));if(!when)continue;
            auto* signers=CMS_get0_SignerInfos(token.get());if(!signers || sk_CMS_SignerInfo_num(signers)!=1)continue;
            auto* timestamp_signer=sk_CMS_SignerInfo_value(signers,0);
            const auto release=[](STACK_OF(X509)* list){sk_X509_pop_free(list,X509_free);};
            std::unique_ptr<STACK_OF(X509),decltype(release)> certificates(CMS_get1_certs(token.get()),release);X509* certificate=nullptr;
            for(int at=0;certificates && at<sk_X509_num(certificates.get());++at){auto* candidate=sk_X509_value(certificates.get(),at);if(!CMS_SignerInfo_cert_cmp(timestamp_signer,candidate)){certificate=candidate;break;}}
            if(!timestamp_chain(certificate,certificates.get(),roots,*when))continue;
            // CMS_verify 內建的鏈驗證固定使用 S/MIME 用途；時間戳改由上方完整驗證
            // TSA 用途、信任根與簽署時間，這裡仍驗證 TSTInfo 的內容與數位簽章。
            if(CMS_verify(token.get(),nullptr,nullptr,nullptr,nullptr,CMS_BINARY|CMS_NO_SIGNER_CERT_VERIFY)==1)return when;
        }catch(const Failure&){}ERR_clear_error();}
    }return {};
}
struct Pe {
    Reader file;std::size_t optional,entry,headers,certificate,certificate_size,table;unsigned sections;
    explicit Pe(Bytes bytes,bool unsigned_allowed=false):file{bytes}{
        if(file.number(0,2)!=0x5a4d)throw Failure("非 PE");const auto nt=file.number(60,4);if(file.number(nt,4)!=0x4550)throw Failure("非 PE");optional=nt+24;const auto magic=file.number(optional,2);const unsigned directories=magic==0x10b?96U:magic==0x20b?112U:0U;
        const auto optional_size=file.number(nt+20,2);if(!directories || optional_size<directories+40 || file.number(optional+directories-4,4)<5)throw Failure("PE 選用標頭不完整");entry=optional+directories+32;headers=file.number(optional+60,4);certificate=file.number(entry,4);certificate_size=file.number(entry+4,4);sections=file.number(nt+6,2);table=optional+optional_size;
        if(sections>4096 || headers<entry+8 || table+sections*40>headers || headers>bytes.size())throw Failure("PE 標頭範圍不合法");
        if(!certificate_size){if(!unsigned_allowed)throw Failure("未簽署",0x800b0100U);certificate=bytes.size();}file.at(certificate,certificate_size);if((certificate&7) || certificate<headers || certificate_size!=bytes.size()-certificate)throw Failure("憑證表位置不支援");
    }
    std::vector<std::uint8_t> digest(const EVP_MD* md)const{
        Ref<EVP_MD_CTX,EVP_MD_CTX_free> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);if(!context || EVP_DigestInit_ex(context.get(),md,nullptr)!=1)throw Failure("建立雜湊失敗");
        const auto add=[&](std::size_t offset,std::size_t size){const auto data=file.at(offset,size);if(EVP_DigestUpdate(context.get(),data.data(),data.size())!=1)throw Failure("計算雜湊失敗");};
        const auto checksum=optional+64;add(0,checksum);add(checksum+4,entry-checksum-4);add(entry+8,headers-entry-8);
        std::vector<std::pair<std::size_t,std::size_t>> raw;
        for(unsigned i=0;i<sections;++i){const auto size=file.number(table+i*40+16,4),offset=file.number(table+i*40+20,4);if(size)raw.emplace_back(offset,size);}
        std::sort(raw.begin(),raw.end());std::size_t end=headers,sum=headers;
        for(const auto& [offset,size]:raw){if(offset<end || offset>certificate || size>certificate-offset)throw Failure("PE section 重疊或越界");add(offset,size);end=offset+size;sum+=size;}
        if(sum>certificate || end>sum)throw Failure("非連續 PE 檔案配置不支援");add(sum,certificate-sum);
        std::array<unsigned char,EVP_MAX_MD_SIZE> output{};unsigned size=0;if(EVP_DigestFinal_ex(context.get(),output.data(),&size)!=1)throw Failure("完成雜湊失敗");return {output.begin(),output.begin()+size};
    }
    bool hash(const EVP_MD* md,Bytes expected)const{const auto output=digest(md);return output.size()==expected.size() && !CRYPTO_memcmp(output.data(),expected.data(),expected.size());}
};
std::uint32_t chain_status(int error){
    switch(error){case X509_V_OK:return 0;case X509_V_ERR_CERT_HAS_EXPIRED:case X509_V_ERR_CERT_NOT_YET_VALID:return 0x800b0101U;case X509_V_ERR_INVALID_PURPOSE:return 0x800b0110U;case X509_V_ERR_CERT_REVOKED:return 0x800b010cU;case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:return 0x800b010aU;default:return 0x800b0109U;}
}
std::string_view key_algorithm(int type){
    switch(type){case EVP_PKEY_RSA:case EVP_PKEY_RSA_PSS:return "RSA";case EVP_PKEY_EC:return "ECDSA";case EVP_PKEY_DSA:return "DSA";default:return {};}
}
std::string_view hash_algorithm(int type){
    switch(type){case NID_md5:return "MD5";case NID_sha1:return "SHA1";case NID_sha256:return "SHA256";case NID_sha384:return "SHA384";case NID_sha512:return "SHA512";default:return {};}
}
void check_strong_signature(const StrongSignaturePolicy& policy,EVP_PKEY* key,int digest){
    const auto algorithm=key?key_algorithm(EVP_PKEY_get_base_id(key)):std::string_view{};
    const auto hash=hash_algorithm(digest);
    const bool pair=std::any_of(policy.signature_hashes.begin(),policy.signature_hashes.end(),[&](const auto& allowed){return allowed.first==algorithm && allowed.second==hash;});
    const bool length=key && std::any_of(policy.key_lengths.begin(),policy.key_lengths.end(),[&](const auto& allowed){return allowed.first==algorithm && EVP_PKEY_get_bits(key)>=0 && static_cast<unsigned>(EVP_PKEY_get_bits(key))>=allowed.second;});
    if(!pair || !length)throw Failure("簽章演算法或金鑰長度不符合要求",0x80090008U);
}
void general_verify(PKCS7* message,PKCS7_SIGNER_INFO* signer,X509* certificate,Bytes contents,bool indirect,std::string_view roots,TrustVerification& result,const StrongSignaturePolicy* policy=nullptr,bool lifetime_signing=false,std::string_view code_roots={}){
    const auto encode=[](auto* value,auto function){const auto size=function(value,nullptr);if(size<=0)throw Failure("簽署者編碼失敗");std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));auto* cursor=bytes.data();(void)function(value,&cursor);return bytes;};
    const auto object_name=[](const ASN1_OBJECT* value){std::array<char,128> name{};const auto count=OBJ_obj2txt(name.data(),static_cast<int>(name.size()),value,1);if(count<=0 || static_cast<std::size_t>(count)>=name.size())throw Failure("簽署者演算法不合法");return std::string(name.data(),static_cast<std::size_t>(count));};
    result.issuer=encode(signer->issuer_and_serial->issuer,i2d_X509_NAME);auto* serial=signer->issuer_and_serial->serial;result.serial.assign(ASN1_STRING_get0_data(serial),ASN1_STRING_get0_data(serial)+ASN1_STRING_length(serial));std::reverse(result.serial.begin(),result.serial.end());
    result.signature.assign(ASN1_STRING_get0_data(signer->enc_digest),ASN1_STRING_get0_data(signer->enc_digest)+ASN1_STRING_length(signer->enc_digest));result.hash_oid=object_name(signer->digest_alg->algorithm);result.encryption_oid=object_name(signer->digest_enc_alg->algorithm);
    const auto attributes=[&](STACK_OF(X509_ATTRIBUTE)* source,auto& destination){if(!source)return;if(sk_X509_ATTRIBUTE_num(source)>1024)throw Failure("簽署者屬性過多");for(int i=0;i<sk_X509_ATTRIBUTE_num(source);++i){auto* item=sk_X509_ATTRIBUTE_value(source,i);TrustAttribute attribute;attribute.oid=object_name(X509_ATTRIBUTE_get0_object(item));for(int j=0;j<X509_ATTRIBUTE_count(item);++j)attribute.values.push_back(encode(X509_ATTRIBUTE_get0_type(item,j),i2d_ASN1_TYPE));destination.push_back(std::move(attribute));}};
    attributes(signer->auth_attr,result.authenticated_attributes);attributes(signer->unauth_attr,result.unauthenticated_attributes);
    auto trust=store(roots,false,roots.empty());if(!code_roots.empty() && !add_certificate_root_pem(trust.get(),code_roots))throw Failure("讀取程式碼簽章根失敗");const auto when=lifetime_signing?std::optional<std::time_t>{}:timestamp(message,signer,roots.empty()?pe_signature_detail::roots_pem:roots);const auto now=when.value_or(std::time(nullptr));result.verify_time=(static_cast<std::uint64_t>(now)+11644473600ULL)*10000000ULL;
    Ref<X509_STORE_CTX,X509_STORE_CTX_free> context(X509_STORE_CTX_new(),X509_STORE_CTX_free);if(!context || !X509_STORE_CTX_init(context.get(),trust.get(),certificate,message->d.sign->cert))throw Failure("建立憑證鏈失敗",0x8007000eU);
    X509_VERIFY_PARAM_set_time(X509_STORE_CTX_get0_param(context.get()),now);const bool trusted=X509_verify_cert(context.get())==1;
    const int error=X509_STORE_CTX_get_error(context.get());auto* chain=X509_STORE_CTX_get0_chain(context.get());
    for(int i=0;chain && i<sk_X509_num(chain);++i){auto* cert=sk_X509_value(chain,i);const auto size=i2d_X509(cert,nullptr);if(size<=0)throw Failure("憑證編碼失敗");TrustCertificate item;item.der.resize(static_cast<std::size_t>(size));auto* cursor=item.der.data();(void)i2d_X509(cert,&cursor);Ref<EVP_PKEY,EVP_PKEY_free> key(X509_get_pubkey(cert),EVP_PKEY_free);item.self_signed=X509_check_issued(cert,cert)==X509_V_OK && key && X509_verify(cert,key.get())==1;item.trusted_root=trusted && i==sk_X509_num(chain)-1;result.certificates.push_back(std::move(item));}
    if(!verify(message,trust.get(),contents,indirect,PKCS7_NOVERIFY))throw Failure("數位簽章不符",0x80096010U);
    if(!eku(certificate,"1.3.6.1.5.5.7.3.3"))throw Failure("憑證沒有程式碼簽章用途",0x800b0110U);
    if(!trusted)throw Failure("憑證鏈驗證失敗",chain_status(error));
    if(policy){
        Ref<EVP_PKEY,EVP_PKEY_free> key(X509_get_pubkey(certificate),EVP_PKEY_free);
        check_strong_signature(*policy,key.get(),OBJ_obj2nid(signer->digest_alg->algorithm));
        // 信任錨的自簽不是鏈中的待驗證簽章；其餘憑證依實際發行者金鑰核對。
        for(int i=0;chain && i+1<sk_X509_num(chain);++i){
            int digest{},type{},security{};std::uint32_t flags{};
            if(!X509_get_signature_info(sk_X509_value(chain,i),&digest,&type,&security,&flags))throw Failure("憑證簽章演算法不支援",0x80090008U);
            Ref<EVP_PKEY,EVP_PKEY_free> issuer(X509_get_pubkey(sk_X509_value(chain,i+1)),EVP_PKEY_free);
            check_strong_signature(*policy,issuer.get(),digest);
        }
    }
    result.status=0;
}
PePublisher authenticate(const Pe& pe,Bytes signature,std::string_view roots,TrustVerification* result=nullptr,const StrongSignaturePolicy* policy=nullptr,bool lifetime_signing=false,std::string_view code_roots={}){
    if(signature.size()>LONG_MAX)throw Failure("簽章過大");const auto* cursor=signature.data();Ref<PKCS7,PKCS7_free> message(d2i_PKCS7(nullptr,&cursor,static_cast<long>(signature.size())),PKCS7_free);
    if(!message || cursor<signature.data() || cursor>signature.data()+signature.size())throw Failure("PKCS#7 不合法");
    const auto padding=signature.subspan(static_cast<std::size_t>(cursor-signature.data()));if(padding.size()>7 || !std::all_of(padding.begin(),padding.end(),[](auto byte){return byte==0;}))throw Failure("PKCS#7 對齊資料不合法");
    const auto contents=signed_content(message.get());auto fields=sequence(contents);const auto image=first_tlv(fields);auto image_fields=sequence(image);const auto type=first_tlv(image_fields);const auto* type_cursor=type.data();Ref<ASN1_OBJECT,ASN1_OBJECT_free> image_type(d2i_ASN1_OBJECT(nullptr,&type_cursor,static_cast<long>(type.size())),ASN1_OBJECT_free);
    if(!image_type || !oid(image_type.get(),"1.3.6.1.4.1.311.2.1.15"))throw Failure("簽章內容非 PE image");const auto digest=first_tlv(fields);if(!fields.empty())throw Failure("Authenticode 額外欄位不合法");cursor=digest.data();Ref<X509_SIG,X509_SIG_free> info(d2i_X509_SIG(nullptr,&cursor,static_cast<long>(digest.size())),X509_SIG_free);if(!info || cursor!=digest.data()+digest.size())throw Failure("DigestInfo 不合法");
    const X509_ALGOR* algorithm=nullptr;const ASN1_OCTET_STRING* expected=nullptr;X509_SIG_get0(info.get(),&algorithm,&expected);const ASN1_OBJECT* object=nullptr;X509_ALGOR_get0(&object,nullptr,nullptr,algorithm);const auto* md=EVP_get_digestbyobj(object);
    if(!md || EVP_MD_get_size(md)<20 || !pe.hash(md,{ASN1_STRING_get0_data(expected),static_cast<std::size_t>(ASN1_STRING_length(expected))}))throw Failure("檔案雜湊不符",0x80096010U);
    auto* signers=message->d.sign->signer_info;if(!signers || sk_PKCS7_SIGNER_INFO_num(signers)!=1)throw Failure("簽署者數量不支援");auto* signer=sk_PKCS7_SIGNER_INFO_value(signers,0);auto* certificate=PKCS7_cert_from_signer_info(message.get(),signer);if(!certificate)throw Failure("缺少簽署者憑證",0x800b010aU);if(!result && !eku(certificate,"1.3.6.1.5.5.7.3.3"))throw Failure("憑證沒有程式碼簽章用途");
    if(result)general_verify(message.get(),signer,certificate,contents,true,roots,*result,policy,lifetime_signing,code_roots);
    auto trust=store(roots,false,result && roots.empty());if(const auto when=timestamp(message.get(),signer,roots))X509_VERIFY_PARAM_set_time(X509_STORE_get0_param(trust.get()),*when);
    if(!result && !verify(message.get(),trust.get(),contents,true))throw Failure("簽章或憑證鏈驗證失敗");
    if(eku(certificate,"1.3.6.1.4.1.311.76.3.1"))return PePublisher::Store;
    if(eku(certificate,"1.3.6.1.4.1.311.10.3.5") || eku(certificate,"1.3.6.1.4.1.311.10.3.5.1") || subject(certificate,NID_commonName,"Microsoft Windows Hardware Compatibility Publisher"))return PePublisher::Whql;
    if(subject(certificate,NID_organizationName,"Microsoft Corporation"))return PePublisher::Microsoft;if(result)return PePublisher::Untrusted;throw Failure("非 Microsoft 發行者");
}
}
PeSignature authenticate_pe(Bytes bytes,std::string_view roots){
    try{const Pe pe(bytes);std::string reason="沒有可驗證的 Authenticode 簽章";
        for(std::size_t at=pe.certificate;at<pe.certificate+pe.certificate_size;){const auto length=pe.file.number(at,4);if(length<8 || length>pe.certificate+pe.certificate_size-at)throw Failure("WIN_CERTIFICATE 不合法");
            if(pe.file.number(at+4,2)==0x200 && pe.file.number(at+6,2)==2)try{return {authenticate(pe,pe.file.at(at+8,length-8),roots),{}};}catch(const Failure& error){reason=error.what();ERR_clear_error();}
            const auto rounded=(static_cast<std::size_t>(length)+7)&~std::size_t{7};if(rounded>pe.certificate+pe.certificate_size-at)throw Failure("WIN_CERTIFICATE 對齊不合法");at+=rounded;
        }return {PePublisher::Untrusted,std::move(reason)};
    }catch(const Failure& error){ERR_clear_error();return {PePublisher::Untrusted,error.what()};}
}

TrustVerification verify_authenticode(Bytes bytes,std::string_view roots,const AuthenticodeOptions& options){
    TrustVerification output;
    try{
        const Pe pe(bytes);std::vector<std::vector<std::uint8_t>> signatures;std::size_t total{};
        const auto append=[&](Bytes signature){
            if(signatures.size()>=64 || signature.size()>16U*1024U*1024U-total)throw Failure("簽章集合過大",0x8007000eU);
            total+=signature.size();signatures.emplace_back(signature.begin(),signature.end());
        };
        for(std::size_t at=pe.certificate;at<pe.certificate+pe.certificate_size;){
            const auto length=pe.file.number(at,4);if(length<8 || length>pe.certificate+pe.certificate_size-at)throw Failure("WIN_CERTIFICATE 不合法");
            if(pe.file.number(at+4,2)==0x200 && pe.file.number(at+6,2)==2)append(pe.file.at(at+8,length-8));
            const auto rounded=(static_cast<std::size_t>(length)+7)&~std::size_t{7};if(rounded>pe.certificate+pe.certificate_size-at)throw Failure("憑證表對齊不合法");at+=rounded;
        }
        // 次要簽章是未認證屬性中的完整 PKCS#7；列舉本身不授予信任。
        // 每一個被選取的簽章仍須重新核對原 PE 雜湊、數位簽章、用途及憑證鏈。
        for(std::size_t i=0;i<signatures.size();++i){
            const auto& encoded=signatures[i];const auto* cursor=encoded.data();
            Ref<PKCS7,PKCS7_free> message(d2i_PKCS7(nullptr,&cursor,static_cast<long>(encoded.size())),PKCS7_free);
            if(!message || !PKCS7_type_is_signed(message.get()) || !message->d.sign)throw Failure("PKCS#7 不合法");
            auto* signers=message->d.sign->signer_info;
            if(!signers || sk_PKCS7_SIGNER_INFO_num(signers)!=1)throw Failure("簽署者數量不支援");
            auto* signer=sk_PKCS7_SIGNER_INFO_value(signers,0);
            for(int a=0;signer->unauth_attr && a<sk_X509_ATTRIBUTE_num(signer->unauth_attr);++a){
                auto* attribute=sk_X509_ATTRIBUTE_value(signer->unauth_attr,a);
                if(!oid(X509_ATTRIBUTE_get0_object(attribute),"1.3.6.1.4.1.311.2.4.1"))continue;
                for(int n=0;n<X509_ATTRIBUTE_count(attribute);++n){
                    auto* value=X509_ATTRIBUTE_get0_type(attribute,n);
                    if(!value || (value->type!=V_ASN1_SEQUENCE && value->type!=V_ASN1_OCTET_STRING))throw Failure("次要簽章資料不合法");
                    auto* data=value->type==V_ASN1_SEQUENCE?value->value.sequence:value->value.octet_string;
                    const auto size=ASN1_STRING_length(data);if(size<=0)throw Failure("次要簽章資料為空");
                    append({ASN1_STRING_get0_data(data),static_cast<std::size_t>(size)});
                }
            }
        }
        output.signature_count=static_cast<std::uint32_t>(signatures.size());
        if(options.signature_index && *options.signature_index>=signatures.size())return output;
        for(std::size_t i=0;i<signatures.size();++i){
            if(options.signature_index && i!=*options.signature_index)continue;
            TrustVerification attempt;attempt.signature_count=output.signature_count;attempt.verified_signature_index=static_cast<std::uint32_t>(i);
            try{(void)authenticate(pe,signatures[i],roots,&attempt,options.strong_policy?&*options.strong_policy:nullptr,options.lifetime_signing,options.code_signing_roots);return attempt;}
            catch(const Failure& error){attempt.status=error.status;attempt.reason=error.what();output=std::move(attempt);ERR_clear_error();}
        }
    }catch(const Failure& error){output.status=error.status;output.reason=error.what();ERR_clear_error();}
    return output;
}
std::vector<std::uint8_t> catalog_file_hash(Bytes bytes,std::string_view algorithm){
    const std::string name(algorithm);const auto* md=EVP_get_digestbyname(name.c_str());if(!md || EVP_MD_get_size(md)<20)throw std::invalid_argument("不支援的目錄雜湊演算法");
    if(bytes.size()>=2 && bytes[0]==0x4d && bytes[1]==0x5a)return Pe(bytes,true).digest(md);
    std::vector<std::uint8_t> result(EVP_MAX_MD_SIZE);unsigned size=0;if(!EVP_Digest(bytes.data(),bytes.size(),result.data(),&size,md,nullptr))throw Failure("目錄雜湊失敗");result.resize(size);return result;
}
namespace {
Ref<PKCS7,PKCS7_free> catalog_message(Bytes bytes){const auto* cursor=bytes.data();Ref<PKCS7,PKCS7_free> value(d2i_PKCS7(nullptr,&cursor,static_cast<long>(bytes.size())),PKCS7_free);if(!value || cursor!=bytes.data()+bytes.size() || !PKCS7_type_is_signed(value.get()) || !value->d.sign || !value->d.sign->contents || !oid(value->d.sign->contents->type,"1.3.6.1.4.1.311.10.1"))throw Failure("目錄 PKCS#7 不合法");return value;}
Bytes catalog_content(PKCS7* message){const auto* inner=message->d.sign->contents;const auto* data=inner->d.other;if(!data || (data->type!=V_ASN1_SEQUENCE && data->type!=V_ASN1_OCTET_STRING))throw Failure("目錄 CTL 不合法");auto* value=data->type==V_ASN1_SEQUENCE?data->value.sequence:data->value.octet_string;const auto size=ASN1_STRING_length(value);if(size<=0)throw Failure("目錄 CTL 為空");return {ASN1_STRING_get0_data(value),static_cast<std::size_t>(size)};}
bool member(Bytes content,Bytes hash){
    auto fields=sequence(content);if(fields.empty())throw Failure("目錄 CTL 截斷");if(fields[0]==2)(void)first_tlv(fields);(void)sequence(first_tlv(fields));
    if(!fields.empty() && fields[0]==4)(void)first_tlv(fields);if(!fields.empty() && fields[0]==2)(void)first_tlv(fields);
    if(fields.empty() || (fields[0]!=23 && fields[0]!=24))throw Failure("目錄 CTL 時間不合法");(void)first_tlv(fields);if(!fields.empty() && (fields[0]==23 || fields[0]==24))(void)first_tlv(fields);
    (void)sequence(first_tlv(fields));if(fields.empty() || fields[0]!=0x30)return false;auto entries=sequence(first_tlv(fields));unsigned count=0;
    while(!entries.empty()){if(++count>65536)throw Failure("目錄 CTL 過大");auto entry=sequence(first_tlv(entries));const auto id=first_tlv(entry);const auto* cursor=id.data();Ref<ASN1_OCTET_STRING,ASN1_OCTET_STRING_free> value(d2i_ASN1_OCTET_STRING(nullptr,&cursor,static_cast<long>(id.size())),ASN1_OCTET_STRING_free);if(!value || cursor!=id.data()+id.size())throw Failure("目錄項目雜湊不合法");if(static_cast<std::size_t>(ASN1_STRING_length(value.get()))==hash.size() && !CRYPTO_memcmp(ASN1_STRING_get0_data(value.get()),hash.data(),hash.size()))return true;
    }return false;
}
}
bool certificate_self_signed(Bytes der){
    const auto* cursor=der.data();Ref<X509,X509_free> certificate(d2i_X509(nullptr,&cursor,static_cast<long>(der.size())),X509_free);
    if(!certificate){ERR_clear_error();return false;}
    auto* key=X509_get0_pubkey(certificate.get());const bool result=key && X509_NAME_cmp(X509_get_subject_name(certificate.get()),X509_get_issuer_name(certificate.get()))==0 && X509_verify(certificate.get(),key)==1;
    ERR_clear_error();return result;
}
bool is_catalog_file(Bytes bytes){try{const auto message=catalog_message(bytes);(void)catalog_content(message.get());return true;}catch(const Failure&){ERR_clear_error();return false;}}
bool catalog_contains(Bytes bytes,Bytes hash){try{const auto message=catalog_message(bytes);return member(catalog_content(message.get()),hash);}catch(const Failure&){ERR_clear_error();return false;}}
TrustVerification verify_catalog(Bytes bytes,Bytes hash,std::string_view roots,const AuthenticodeOptions& options){
    TrustVerification result;try{const auto message=catalog_message(bytes);const auto content=catalog_content(message.get());if(!member(content,hash))throw Failure("檔案雜湊不在簽署目錄中",0x80096010U);auto* signers=message->d.sign->signer_info;if(!signers || sk_PKCS7_SIGNER_INFO_num(signers)!=1)throw Failure("目錄簽署者不合法");result.signature_count=1;if(options.signature_index && *options.signature_index)return result;auto* signer=sk_PKCS7_SIGNER_INFO_value(signers,0);auto* certificate=PKCS7_cert_from_signer_info(message.get(),signer);if(!certificate)throw Failure("目錄缺少憑證",0x800b010aU);general_verify(message.get(),signer,certificate,content,message->d.sign->contents->d.other->type==V_ASN1_SEQUENCE,roots,result,options.strong_policy?&*options.strong_policy:nullptr,options.lifetime_signing,options.code_signing_roots);
    }catch(const Failure& error){result.status=error.status;result.reason=error.what();ERR_clear_error();}return result;
}

PeSignature authenticate_microsoft_pe(Bytes bytes){return authenticate_pe(bytes,pe_signature_detail::roots_pem);}
}
