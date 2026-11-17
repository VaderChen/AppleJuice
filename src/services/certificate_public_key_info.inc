// CERT_PUBLIC_KEY_INFO 的匯出與匯入（由 windows_certificates.cpp 的 invoke_certificate 引入，沿用 CSP、NCrypt 與 CNG 已驗證的金鑰 DER）：
// - CryptExportPublicKeyInfo(Ex)：CSP（依 dwKeySpec 取使用者金鑰）或 NCrypt 金鑰的公鑰，依 SubjectPublicKeyInfo 填入演算法 OID、參數 DER 與公鑰位元字串；
//   結構與資料放在同一個輸出緩衝區，pInfo 為 NULL 時只回傳大小，不足時為 ERROR_MORE_DATA。Ex 版可指定公鑰 OID。
// - CryptImportPublicKeyInfo(Ex)：匯入 CSP 成為公鑰 handle（RSA；aiKeyAlg 為 0 時用 CALG_RSA_KEYX）。
// - CryptImportPublicKeyInfoEx2：匯入成 BCrypt 公鑰 handle；ECC 依旗標選 ECDSA（預設）或 ECDH（CRYPT_OID_INFO_PUBKEY_ENCRYPT_KEY_FLAG）。
if(symbol.starts_with("CryptExportPublicKeyInfo") || symbol.starts_with("CryptImportPublicKeyInfo")){
    if(symbol.starts_with("CryptExportPublicKeyInfo")){
        // (hKey, dwKeySpec, dwCertEncodingType, [pszPublicKeyObjId, dwFlags, pvAuxInfo,] pInfo, pcbInfo)
        const bool extended=symbol=="CryptExportPublicKeyInfoEx";const auto info_out=arg(extended?6:3),size_out=arg(extended?7:4);
        if((u32(2)&0xffffU)!=1)throw CertificateError{0x80070057};if(!size_out)throw CertificateError{87};if(extended && arg(5))throw CertificateError{87};
        std::vector<std::uint8_t> key_der;bool private_key=false;
        if(const auto ncrypt=runtime_.storage_crypto_keys.find(arg(0));ncrypt!=runtime_.storage_crypto_keys.end()){
            if(!ncrypt->second.finalized)throw CertificateError{0x8009000bU};key_der=ncrypt->second.der;private_key=ncrypt->second.private_key;
        }else{
            if(!runtime_.crypt_providers.contains(arg(0)))throw CertificateError{6};
            const auto spec=u32(1);if(spec!=1 && spec!=2)throw CertificateError{0x80090008U};
            const auto found=runtime_.legacy_crypto_user_keys.find({arg(0),spec});if(found==runtime_.legacy_crypto_user_keys.end())throw CertificateError{0x8009000dU};
            key_der=found->second.der;private_key=found->second.private_key;
        }
        cng::Key key(nullptr,EVP_PKEY_free);try{key=cng::decode(key_der,private_key);}catch(const cng::Error& error){throw CertificateError{error.code};}
        const auto spki=encoded(key.get(),i2d_PUBKEY);const auto* cursor=spki.data();
        std::unique_ptr<X509_PUBKEY,decltype(&X509_PUBKEY_free)> pub(d2i_X509_PUBKEY(nullptr,&cursor,static_cast<long>(spki.size())),X509_PUBKEY_free);if(!pub)throw CertificateError{0x80090005U};
        ASN1_OBJECT* object=nullptr;const unsigned char* bits_data=nullptr;int bits_size=0;X509_ALGOR* algorithm=nullptr;
        if(!X509_PUBKEY_get0_param(&object,&bits_data,&bits_size,&algorithm,pub.get()))throw CertificateError{0x80090005U};
        char oid_text[128]{};OBJ_obj2txt(oid_text,sizeof oid_text,object,1);std::string oid=oid_text;
        if(extended && arg(3)){const auto requested=read_ansi(memory,arg(3),128);if(!requested.empty())oid=requested;}
        std::vector<std::uint8_t> parameters;const ASN1_OBJECT* algorithm_object=nullptr;int parameter_type=0;const void* parameter_value=nullptr;
        X509_ALGOR_get0(&algorithm_object,&parameter_type,&parameter_value,algorithm);
        if(parameter_type==V_ASN1_NULL)parameters={5,0};
        else if(parameter_type==V_ASN1_OBJECT)parameters=encoded(static_cast<const ASN1_OBJECT*>(parameter_value),i2d_ASN1_OBJECT);
        else if(parameter_type==V_ASN1_SEQUENCE){const auto* sequence=static_cast<const ASN1_STRING*>(parameter_value);parameters.assign(ASN1_STRING_get0_data(sequence),ASN1_STRING_get0_data(sequence)+ASN1_STRING_length(sequence));}
        const std::uint32_t header=x86_?24U:48U;std::vector<std::uint8_t> bytes(header);
        const auto put=[&](std::size_t at,unsigned width,std::uint64_t value){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
        const auto append=[&](std::span<const std::uint8_t> data){const auto at=bytes.size();bytes.insert(bytes.end(),data.begin(),data.end());while(bytes.size()%4)bytes.push_back(0);return at;};
        std::vector<std::uint8_t> oid_bytes(oid.begin(),oid.end());oid_bytes.push_back(0);
        const auto oid_at=append(oid_bytes),parameters_at=parameters.empty()?0:append(parameters),key_at=append(std::span(bits_data,static_cast<std::size_t>(bits_size)));
        const auto needed=static_cast<std::uint32_t>(bytes.size());const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{size_out},32));
        memory.check(GuestAddress{size_out},4,Access::Write);memory.store(GuestAddress{size_out},32,needed);
        if(!info_out){result=1;}
        else if(capacity<needed)throw CertificateError{234};
        else{
            put(0,unit,info_out+oid_at);put(unit,4,parameters.size());put(2*unit,unit,parameters.empty()?0:info_out+parameters_at);
            put(3*unit,4,static_cast<std::uint32_t>(bits_size));put(4*unit,unit,info_out+key_at);put(5*unit,4,0);
            memory.write(GuestAddress{info_out},bytes);result=1;
        }
    }else if(symbol=="CryptImportPublicKeyInfoEx2"){
        // (dwCertEncodingType, pInfo, dwFlags, pvAuxInfo, phKey)
        if((u32(0)&0xffffU)!=1)throw CertificateError{0x80070057};if(arg(3) || !arg(4))throw CertificateError{87};
        auto key=public_key(arg(1));std::string name;try{name=cng::algorithm(key.get());}catch(const cng::Error& error){throw CertificateError{error.code};}
        if(name.starts_with("ECD")){const bool exchange=(u32(2)&0x40000000U)!=0;name=(exchange?std::string("ECDH"):std::string("ECDSA"))+name.substr(name.find('_'));}
        CryptoKey value;value.algorithm=CryptoAlgorithm{name,false,false};value.bits=static_cast<std::uint32_t>(EVP_PKEY_get_bits(key.get()));value.der=cng::der(key.get(),false);value.finalized=true;
        if(runtime_.crypto_keys.size()>=4096)throw CertificateError{8};memory.check(GuestAddress{arg(4)},unit,Access::Write);
        const auto handle=runtime_.next_crypto_handle++;runtime_.crypto_keys.emplace(handle,std::move(value));memory.store(GuestAddress{arg(4)},bits,handle);result=1;
    }else{
        // CryptImportPublicKeyInfo(hCryptProv, dwCertEncodingType, pInfo, phKey)／Ex(hCryptProv, dwCertEncodingType, pInfo, aiKeyAlg, dwFlags, pvAuxInfo, phKey)
        const bool extended=symbol=="CryptImportPublicKeyInfoEx";const auto output=arg(extended?6:3);
        if(!runtime_.crypt_providers.contains(arg(0)))throw CertificateError{6};if((u32(1)&0xffffU)!=1)throw CertificateError{0x80070057};
        if(!output || (extended && arg(5)))throw CertificateError{87};
        auto key=public_key(arg(2));if(!EVP_PKEY_is_a(key.get(),"RSA"))throw CertificateError{0x80090008U};
        const auto algorithm=extended && u32(3)?u32(3):0xa400U;if(algorithm!=0xa400U && algorithm!=0x2400U)throw CertificateError{0x80090008U};
        LegacyCryptKey value;value.provider=arg(0);value.algorithm=algorithm;value.bits=static_cast<std::uint32_t>(EVP_PKEY_get_bits(key.get()));value.der=encoded(key.get(),i2d_PUBKEY);
        if(runtime_.legacy_crypto_keys.size()>=4096)throw CertificateError{8};memory.check(GuestAddress{output},unit,Access::Write);
        const auto handle=runtime_.next_crypto_handle++;runtime_.legacy_crypto_keys.emplace(handle,std::move(value));memory.store(GuestAddress{output},bits,handle);result=1;
    }
    arg.return_to(state,result);return std::nullopt;
}
