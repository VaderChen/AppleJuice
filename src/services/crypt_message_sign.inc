// PKCS#7 簽章、驗章與雜湊訊息的共用核心（由 windows_certificates.cpp 在訊息輔助函式之後引入）。
// 簽章：有已驗證屬性時自動加上 contentType 與 messageDigest 並簽屬性（不像 OpenSSL 預設那樣另加 signingTime）；沒有屬性時直接簽內容的雜湊。
// 驗章：有屬性時先比對 messageDigest，再以公鑰驗屬性的簽章；沒有屬性時驗內容雜湊的簽章。
using Context=std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)>;
struct MessageAttribute {std::string oid;std::vector<std::vector<std::uint8_t>> values;};
struct MessageSignerInput {cng::Key key{nullptr,EVP_PKEY_free};std::vector<std::uint8_t> issuer,serial;const EVP_MD* md{};std::vector<MessageAttribute> authenticated,unauthenticated;};
std::vector<std::uint8_t> message_digest(const EVP_MD* md,std::span<const std::uint8_t> content){
    std::vector<std::uint8_t> out(EVP_MAX_MD_SIZE);unsigned size=0;if(!EVP_Digest(content.data(),content.size(),out.data(),&size,md,nullptr))throw CertificateError{0x80090005U};out.resize(size);return out;
}
// 只帶簽發者與序號的 X509，供 SignerInfo 的 issuerAndSerialNumber；序號為 CryptoAPI 的小端序。
Certificate signer_identity(std::span<const std::uint8_t> issuer,std::vector<std::uint8_t> serial){
    Certificate cert(X509_new(),X509_free);const auto* cursor=issuer.data();std::unique_ptr<X509_NAME,decltype(&X509_NAME_free)> name(d2i_X509_NAME(nullptr,&cursor,static_cast<long>(issuer.size())),X509_NAME_free);
    if(!cert || !name || cursor!=issuer.data()+issuer.size())throw CertificateError{0x8009310b};std::reverse(serial.begin(),serial.end());
    std::unique_ptr<BIGNUM,decltype(&BN_free)> number(BN_bin2bn(serial.data(),static_cast<int>(serial.size()),nullptr),BN_free);
    if(!number || (!serial.empty() && (serial.front()&0x80)))throw CertificateError{0x80070057};
    std::unique_ptr<ASN1_INTEGER,decltype(&ASN1_INTEGER_free)> integer(BN_to_ASN1_INTEGER(number.get(),nullptr),ASN1_INTEGER_free);
    if(!integer || !X509_set_issuer_name(cert.get(),name.get()) || !X509_set_serialNumber(cert.get(),integer.get()))throw CertificateError{8};return cert;
}
void add_message_attribute(STACK_OF(X509_ATTRIBUTE)** list,const MessageAttribute& attribute){
    std::unique_ptr<ASN1_OBJECT,decltype(&ASN1_OBJECT_free)> object(OBJ_txt2obj(attribute.oid.c_str(),1),ASN1_OBJECT_free);if(!object)throw CertificateError{0x80091002};
    std::vector<std::vector<std::uint8_t>> values=attribute.values;std::sort(values.begin(),values.end());std::vector<std::uint8_t> set;for(const auto& value:values)set.insert(set.end(),value.begin(),value.end());
    auto content=encoded(object.get(),i2d_ASN1_OBJECT);const auto wrapped=der_container(V_ASN1_SET,set);content.insert(content.end(),wrapped.begin(),wrapped.end());const auto der=der_container(V_ASN1_SEQUENCE,content);
    const auto* cursor=der.data();std::unique_ptr<X509_ATTRIBUTE,decltype(&X509_ATTRIBUTE_free)> parsed(d2i_X509_ATTRIBUTE(nullptr,&cursor,static_cast<long>(der.size())),X509_ATTRIBUTE_free);
    if(!parsed || cursor!=der.data()+der.size() || !X509at_add1_attr(list,parsed.get()))throw CertificateError{0x80091002};
}
std::vector<std::uint8_t> sign_digest(EVP_PKEY* key,const EVP_MD* md,std::span<const std::uint8_t> digest){
    Context context(EVP_PKEY_CTX_new(key,nullptr),EVP_PKEY_CTX_free);std::size_t size=0;
    if(!context || EVP_PKEY_sign_init(context.get())<=0 || (EVP_PKEY_is_a(key,"RSA") && EVP_PKEY_CTX_set_rsa_padding(context.get(),RSA_PKCS1_PADDING)<=0) ||
       EVP_PKEY_CTX_set_signature_md(context.get(),md)<=0 || EVP_PKEY_sign(context.get(),nullptr,&size,digest.data(),digest.size())<=0)throw CertificateError{0x80090005U};
    std::vector<std::uint8_t> out(size);if(EVP_PKEY_sign(context.get(),out.data(),&size,digest.data(),digest.size())<=0)throw CertificateError{0x80090005U};out.resize(size);return out;
}
std::vector<std::uint8_t> sign_pkcs7(std::span<const std::uint8_t> content,bool detached,std::vector<MessageSignerInput>& signers,const std::vector<std::vector<std::uint8_t>>& certificates,const std::vector<std::vector<std::uint8_t>>& crls){
    SignedMessage message(PKCS7_new(),PKCS7_free);
    if(!message || !PKCS7_set_type(message.get(),NID_pkcs7_signed) || !PKCS7_content_new(message.get(),NID_pkcs7_data))throw CertificateError{8};
    if(detached){if(!PKCS7_set_detached(message.get(),1))throw CertificateError{8};}
    else if(!ASN1_OCTET_STRING_set(message->d.sign->contents->d.data,content.data(),static_cast<int>(content.size())))throw CertificateError{8};
    for(auto& signer:signers){
        auto identity=signer_identity(signer.issuer,signer.serial);auto* info=PKCS7_add_signature(message.get(),identity.get(),signer.key.get(),signer.md);if(!info){ERR_clear_error();throw CertificateError{0x80090008U};}
        const auto digest=message_digest(signer.md,content);
        if(!signer.authenticated.empty()){
            std::unique_ptr<ASN1_OCTET_STRING,decltype(&ASN1_OCTET_STRING_free)> value(ASN1_OCTET_STRING_new(),ASN1_OCTET_STRING_free);
            if(!value || !ASN1_OCTET_STRING_set(value.get(),digest.data(),static_cast<int>(digest.size())) ||
               !PKCS7_add_signed_attribute(info,NID_pkcs9_contentType,V_ASN1_OBJECT,OBJ_nid2obj(NID_pkcs7_data)))throw CertificateError{8};
            if(!PKCS7_add_signed_attribute(info,NID_pkcs9_messageDigest,V_ASN1_OCTET_STRING,value.get()))throw CertificateError{8};value.release();
            for(const auto& attribute:signer.authenticated)add_message_attribute(&info->auth_attr,attribute);
            if(PKCS7_SIGNER_INFO_sign(info)<=0){ERR_clear_error();throw CertificateError{0x80090005U};}
        }else{const auto signature=sign_digest(signer.key.get(),signer.md,digest);if(!ASN1_STRING_set(info->enc_digest,signature.data(),static_cast<int>(signature.size())))throw CertificateError{8};}
        for(const auto& attribute:signer.unauthenticated)add_message_attribute(&info->unauth_attr,attribute);
    }
    for(const auto& der:certificates){auto cert=decode_certificate(der);if(!PKCS7_add_certificate(message.get(),cert.get()))throw CertificateError{8};}
    for(const auto& der:crls){auto crl=decode_crl(der);if(!PKCS7_add_crl(message.get(),crl.get()))throw CertificateError{8};}
    return encoded(message.get(),i2d_PKCS7);
}
// 驗證第 index 個簽署者；content 為簽署內容（附加或分離）。
std::uint32_t verify_pkcs7_signer(PKCS7* message,int index,EVP_PKEY* key,std::span<const std::uint8_t> content){
    auto* signers=PKCS7_get_signer_info(message);if(!signers || index<0 || index>=sk_PKCS7_SIGNER_INFO_num(signers))return 0x8009100eU;
    auto* info=sk_PKCS7_SIGNER_INFO_value(signers,index);const auto* md=EVP_get_digestbyobj(info->digest_alg->algorithm);if(!md)return 0x80091002U;
    const auto digest=message_digest(md,content);const std::span signature(ASN1_STRING_get0_data(info->enc_digest),static_cast<std::size_t>(ASN1_STRING_length(info->enc_digest)));
    if(sk_X509_ATTRIBUTE_num(info->auth_attr)>0){
        auto* attribute=PKCS7_get_signed_attribute(info,NID_pkcs9_messageDigest);if(!attribute || attribute->type!=V_ASN1_OCTET_STRING)return 0x80091007U;
        const auto* value=attribute->value.octet_string;if(static_cast<std::size_t>(ASN1_STRING_length(value))!=digest.size() || !std::equal(digest.begin(),digest.end(),ASN1_STRING_get0_data(value)))return 0x80091007U;
        unsigned char* bytes=nullptr;const int length=ASN1_item_i2d(reinterpret_cast<ASN1_VALUE*>(info->auth_attr),&bytes,ASN1_ITEM_rptr(PKCS7_ATTR_VERIFY));if(length<=0)return 0x80091002U;
        std::unique_ptr<unsigned char,void(*)(unsigned char*)> owned(bytes,[](unsigned char* p){OPENSSL_free(p);});
        std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(),EVP_MD_CTX_free);EVP_PKEY_CTX* key_context=nullptr;
        if(!context || EVP_DigestVerifyInit(context.get(),&key_context,md,nullptr,key)<=0 || (EVP_PKEY_is_a(key,"RSA") && EVP_PKEY_CTX_set_rsa_padding(key_context,RSA_PKCS1_PADDING)<=0)){ERR_clear_error();return 0x80090006U;}
        const auto ok=EVP_DigestVerify(context.get(),signature.data(),signature.size(),bytes,static_cast<std::size_t>(length));ERR_clear_error();return ok==1?0U:0x80090006U;
    }
    Context context(EVP_PKEY_CTX_new(key,nullptr),EVP_PKEY_CTX_free);
    if(!context || EVP_PKEY_verify_init(context.get())<=0 || (EVP_PKEY_is_a(key,"RSA") && EVP_PKEY_CTX_set_rsa_padding(context.get(),RSA_PKCS1_PADDING)<=0) || EVP_PKEY_CTX_set_signature_md(context.get(),md)<=0){ERR_clear_error();return 0x80090006U;}
    const auto ok=EVP_PKEY_verify(context.get(),signature.data(),signature.size(),digest.data(),digest.size());ERR_clear_error();return ok==1?0U:0x80090006U;
}
// 第 index 個簽署者的簽發者 DER 與小端序序號。
std::pair<std::vector<std::uint8_t>,std::vector<std::uint8_t>> pkcs7_signer_id(PKCS7* message,int index){
    auto* signers=PKCS7_get_signer_info(message);if(!signers || index<0 || index>=sk_PKCS7_SIGNER_INFO_num(signers))throw CertificateError{0x8009100eU};
    auto* info=sk_PKCS7_SIGNER_INFO_value(signers,index);auto serial=asn1_content(encoded(info->issuer_and_serial->serial,i2d_ASN1_INTEGER));std::reverse(serial.begin(),serial.end());
    return {encoded(info->issuer_and_serial->issuer,i2d_X509_NAME),serial};
}
// 在訊息內含的憑證中找與第 index 個簽署者相同簽發者與序號的憑證。
std::optional<std::vector<std::uint8_t>> pkcs7_signer_certificate(PKCS7* message,int index,const std::vector<std::vector<std::uint8_t>>& extra={}){
    auto* signers=PKCS7_get_signer_info(message);if(!signers || index<0 || index>=sk_PKCS7_SIGNER_INFO_num(signers))return std::nullopt;auto* info=sk_PKCS7_SIGNER_INFO_value(signers,index);
    const auto matches=[&](X509* cert){return X509_NAME_cmp(X509_get_issuer_name(cert),info->issuer_and_serial->issuer)==0 && ASN1_INTEGER_cmp(X509_get0_serialNumber(cert),info->issuer_and_serial->serial)==0;};
    if(PKCS7_type_is_signed(message) && message->d.sign->cert)for(int i=0;i<sk_X509_num(message->d.sign->cert);++i){auto* cert=sk_X509_value(message->d.sign->cert,i);if(matches(cert))return encoded(cert,i2d_X509);}
    for(const auto& der:extra){auto cert=decode_certificate(der);if(matches(cert.get()))return der;}
    return std::nullopt;
}
// 雜湊訊息（PKCS#7 digestedData，版本 0、內容為 data）。
std::vector<std::uint8_t> hash_pkcs7(std::span<const std::uint8_t> content,bool detached,const EVP_MD* md){
    SignedMessage message(PKCS7_new(),PKCS7_free);SignedMessage inner(PKCS7_new(),PKCS7_free);
    if(!message || !inner || !PKCS7_set_type(message.get(),NID_pkcs7_digest) || !PKCS7_set_digest(message.get(),md) || !PKCS7_set_type(inner.get(),NID_pkcs7_data))throw CertificateError{8};
    if(detached){ASN1_OCTET_STRING_free(inner->d.data);inner->d.data=nullptr;}else if(!ASN1_OCTET_STRING_set(inner->d.data,content.data(),static_cast<int>(content.size())))throw CertificateError{8};
    if(!PKCS7_set_content(message.get(),inner.get()))throw CertificateError{8};inner.release();
    const auto digest=message_digest(md,content);auto* value=ASN1_OCTET_STRING_new();if(!value || !ASN1_OCTET_STRING_set(value,digest.data(),static_cast<int>(digest.size()))){ASN1_OCTET_STRING_free(value);throw CertificateError{8};}
    ASN1_OCTET_STRING_free(message->d.digest->digest);message->d.digest->digest=value;return encoded(message.get(),i2d_PKCS7);
}
