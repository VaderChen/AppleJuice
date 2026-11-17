// crypt32 憑證簽署（由 windows_certificates.cpp 的 invoke_certificate 引入，雜湊與簽章沿用 crypt_message_sign.inc，編碼沿用 CryptEncodeObjectEx）：
// - CryptSignCertificate：以簽章演算法 OID 對應的雜湊計算 ToBeSigned，以 CSP 金鑰（dwKeySpec）或 NCrypt 金鑰（CERT_NCRYPT_KEY_SPEC）簽署；
//   輸出為 CERT_SIGNED_CONTENT_INFO.Signature 的反轉位元組順序（RSA 與 CryptSignHash 相同）。OID 為雜湊演算法時只輸出雜湊值、不使用金鑰。
//   只查詢大小時回報該金鑰的最大簽章長度（ECDSA 的 DER 長度每次不同）。CSP handle 須支援該雜湊（例如 PROV_RSA_FULL 不支援 SHA-2），否則 NTE_BAD_ALGID。
// - CryptSignAndEncodeCertificate：以 CryptEncodeObjectEx 編碼 lpszStructType 的結構、簽署後組成 CERT_SIGNED_CONTENT_INFO 並以 X509_CERT 編碼。
// - CryptHashToBeSigned：解析已簽署內容（SEQUENCE { ToBeSigned, AlgorithmIdentifier, BIT STRING }），以簽章演算法的雜湊計算 ToBeSigned。
if(symbol=="CryptSignCertificate" || symbol=="CryptSignAndEncodeCertificate" || symbol=="CryptHashToBeSigned"){
    constexpr std::uint32_t bad_algorithm=0x80090008U;
    // 簽章演算法 OID → 雜湊與金鑰類型；純雜湊 OID 的金鑰類型為 0。
    const auto algorithm_of=[&](const std::string& oid)->std::pair<const EVP_MD*,int>{
        std::unique_ptr<ASN1_OBJECT,decltype(&ASN1_OBJECT_free)> object(OBJ_txt2obj(oid.c_str(),1),ASN1_OBJECT_free);if(!object){ERR_clear_error();throw CertificateError{bad_algorithm};}
        const int nid=OBJ_obj2nid(object.get());int digest=0,key=0;
        if(nid!=NID_undef && OBJ_find_sigid_algs(nid,&digest,&key)){const auto* md=EVP_get_digestbynid(digest);if(!md || key==NID_undef)throw CertificateError{bad_algorithm};return {md,key};}
        if(const auto* md=EVP_get_digestbyobj(object.get()))return {md,0};
        ERR_clear_error();throw CertificateError{bad_algorithm};
    };
    // CSP handle 與 CryptCreateHash 相同只接受該提供者支援的雜湊；NCrypt 金鑰不受此限。
    const auto check_provider=[&](std::uint64_t handle,const EVP_MD* md){
        const auto provider=runtime_.crypt_providers.find(handle);if(provider==runtime_.crypt_providers.end())return;
        const auto type=EVP_MD_get_type(md);const std::uint32_t id=type==NID_md5?0x8003U:type==NID_sha1?0x8004U:type==NID_sha256?0x800cU:type==NID_sha384?0x800dU:type==NID_sha512?0x800eU:0U;
        if(!id || !legacy_crypto::algorithm_for(id,provider->second))throw CertificateError{bad_algorithm};
    };
    const auto signature_oid=[&](std::uint64_t identifier){if(!identifier)throw CertificateError{87};const auto pointer=memory.load(GuestAddress{identifier},bits);if(!pointer)throw CertificateError{87};return read_ansi(memory,pointer,1024);};
    const auto sign=[&](std::uint64_t handle,std::uint32_t spec,const std::string& oid,std::span<const std::uint8_t> content,bool size_only){
        const auto [md,key_type]=algorithm_of(oid);if(handle && spec!=UINT32_MAX)check_provider(handle,md);if(!key_type)return message_digest(md,content);
        if(!handle)throw CertificateError{87};std::vector<std::uint8_t> der;
        if(spec==UINT32_MAX){const auto found=runtime_.storage_crypto_keys.find(handle);if(found==runtime_.storage_crypto_keys.end())throw CertificateError{6};
            if(!found->second.finalized || !found->second.private_key)throw CertificateError{0x8009000bU};if(!(found->second.usage&2U))throw CertificateError{0x80090010U};der=found->second.der;}
        else{const auto found=runtime_.legacy_crypto_user_keys.find({handle,spec});
            if(found==runtime_.legacy_crypto_user_keys.end())throw CertificateError{runtime_.crypt_providers.contains(handle)?0x8009000dU:0x80090001U};if(!found->second.private_key)throw CertificateError{0x8009000dU};der=found->second.der;}
        const auto* cursor=der.data();std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key(d2i_AutoPrivateKey(nullptr,&cursor,static_cast<long>(der.size())),EVP_PKEY_free);
        if(!key || cursor!=der.data()+der.size()){ERR_clear_error();throw CertificateError{0x80090003U};}
        if(key_type==NID_rsaEncryption?!EVP_PKEY_is_a(key.get(),"RSA"):key_type==NID_X9_62_id_ecPublicKey?!EVP_PKEY_is_a(key.get(),"EC"):key_type==NID_dsa?!EVP_PKEY_is_a(key.get(),"DSA"):true)throw CertificateError{bad_algorithm};
        if(size_only)return std::vector<std::uint8_t>(static_cast<std::size_t>(EVP_PKEY_get_size(key.get())));
        auto signature=sign_digest(key.get(),md,message_digest(md,content));std::reverse(signature.begin(),signature.end());return signature;
    };
    if(symbol=="CryptSignCertificate"){
        // (hCryptProvOrNCryptKey, dwKeySpec, dwCertEncodingType, pbEncodedToBeSigned, cbEncodedToBeSigned, pSignatureAlgorithm, pvHashAuxInfo, pbSignature, pcbSignature)
        if(!arg(8))throw CertificateError{87};const auto oid=signature_oid(arg(5));if(u32(4)>output_limit)throw CertificateError{8};
        std::vector<std::uint8_t> content(u32(4));if(!content.empty()){if(!arg(3))throw CertificateError{87};memory.read(GuestAddress{arg(3)},content);}
        const auto signature=sign(arg(0),u32(1),oid,content,!arg(7));const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(8)},32));
        memory.store(GuestAddress{arg(8)},32,signature.size());if(arg(7)){if(capacity<signature.size())throw CertificateError{234};if(!signature.empty())memory.write(GuestAddress{arg(7)},signature);}
        result=1;
    }else if(symbol=="CryptSignAndEncodeCertificate"){
        // (hCryptProvOrNCryptKey, dwKeySpec, dwCertEncodingType, lpszStructType, pvStructInfo, pSignatureAlgorithm, pvHashAuxInfo, pbEncoded, pcbEncoded)
        if(!arg(8))throw CertificateError{87};const auto oid=signature_oid(arg(5));GuestScratch scratch(*this,memory,state);
        const auto size_pointer=scratch.allocate(4);if(!size_pointer)throw CertificateError{8};memory.store(GuestAddress{size_pointer},32,0);
        const auto encode=[&](std::uint64_t type,std::uint64_t info,std::uint64_t out,std::uint64_t size){if(!(nested_service(state,memory,"crypt32.dll","CryptEncodeObjectEx",{u32(2),type,info,0,0,out,size})&UINT32_MAX))throw CertificateError{last_error_?last_error_:0x8009310bU};};
        encode(arg(3),arg(4),0,size_pointer);const auto tbs_size=static_cast<std::uint32_t>(memory.load(GuestAddress{size_pointer},32));if(!tbs_size || tbs_size>output_limit)throw CertificateError{8};
        const auto tbs=scratch.allocate(tbs_size);if(!tbs)throw CertificateError{8};encode(arg(3),arg(4),tbs,size_pointer);
        std::vector<std::uint8_t> content(static_cast<std::uint32_t>(memory.load(GuestAddress{size_pointer},32)));memory.read(GuestAddress{tbs},content);
        const auto signature=sign(arg(0),u32(1),oid,content,!arg(7));
        // CERT_SIGNED_CONTENT_INFO：ToBeSigned、SignatureAlgorithm（沿用呼叫端的 OID 與參數）、Signature（CRYPT_BIT_BLOB）。
        const auto info=scratch.allocate(x86_?32U:64U),signature_bytes=scratch.allocate(std::max<std::size_t>(signature.size(),1));if(!info || !signature_bytes)throw CertificateError{8};
        memory.write(GuestAddress{info},std::vector<std::uint8_t>(x86_?32U:64U));if(!signature.empty())memory.write(GuestAddress{signature_bytes},signature);
        memory.store(GuestAddress{info},32,content.size());memory.store(GuestAddress{info+unit},bits,tbs);
        for(unsigned field=0;field<3;++field)memory.store(GuestAddress{info+2*unit+field*unit},field==1?32U:bits,memory.load(GuestAddress{arg(5)+field*unit},field==1?32U:bits));
        memory.store(GuestAddress{info+5*unit},32,signature.size());memory.store(GuestAddress{info+6*unit},bits,signature_bytes);
        encode(1,info,arg(7),arg(8));result=1;
    }else{
        // (hCryptProv, dwCertEncodingType, pbEncoded, cbEncoded, pbComputedHash, pcbComputedHash)
        if(!arg(5))throw CertificateError{87};const auto der=read_der(u32(1),arg(2),u32(3));
        const unsigned char* cursor=der.data();const unsigned char* const end=der.data()+der.size();long length=0;int tag=0,type=0;
        const auto object=[&](const unsigned char*& at,const unsigned char* limit){const auto status=ASN1_get_object(&at,&length,&tag,&type,limit-at);if((status&0x80) || type!=V_ASN1_UNIVERSAL || length>limit-at)throw CertificateError{0x8009310bU};return status;};
        if(!(object(cursor,end)&V_ASN1_CONSTRUCTED) || tag!=V_ASN1_SEQUENCE || cursor+length!=end)throw CertificateError{0x8009310bU};
        const auto* tbs_begin=cursor;(void)object(cursor,end);cursor+=length;const std::span<const std::uint8_t> tbs(tbs_begin,static_cast<std::size_t>(cursor-tbs_begin));
        std::unique_ptr<X509_ALGOR,decltype(&X509_ALGOR_free)> algorithm(d2i_X509_ALGOR(nullptr,&cursor,end-cursor),X509_ALGOR_free);if(!algorithm){ERR_clear_error();throw CertificateError{0x8009310bU};}
        char name[128]{};if(OBJ_obj2txt(name,sizeof name,algorithm->algorithm,1)<=0){ERR_clear_error();throw CertificateError{bad_algorithm};}
        const auto* md=algorithm_of(name).first;if(arg(0))check_provider(arg(0),md);const auto hash=message_digest(md,tbs);const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(5)},32));
        memory.store(GuestAddress{arg(5)},32,hash.size());if(arg(4)){if(capacity<hash.size())throw CertificateError{234};memory.write(GuestAddress{arg(4)},hash);}
        result=1;
    }
    arg.return_to(state,result);return std::nullopt;
}
