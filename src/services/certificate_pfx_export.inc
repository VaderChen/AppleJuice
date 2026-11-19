if(symbol=="PFXExportCertStore" || symbol=="PFXExportCertStoreEx"){
    // (hStore, pPFX, szPassword, [pvPara,] dwFlags)：CRYPT_DATA_BLOB 的 pbData 為 NULL 時只回報大小，不足時 ERROR_MORE_DATA。
    // EXPORT_PRIVATE_KEYS 時沿用 CryptAcquireCertificatePrivateKey 的流程取得私鑰（CSP 或 CNG）：不可匯出的金鑰略過，設 REPORT_NOT_ABLE_TO_EXPORT_PRIVATE_KEY
    // 時改為失敗（NTE_BAD_KEY_STATE）；沒有私鑰且設 REPORT_NO_PRIVATE_KEY 時以取得私鑰的錯誤失敗。金鑰包含容器、CSP 名稱與 LocalKeySet，PFXImportCertStore 可原樣還原。
    // PKCS12_INCLUDE_EXTENDED_PROPERTIES 輸出可序列化的憑證屬性；PKCS12_DISABLE_ENCRYPT_CERTIFICATES 時憑證不加密；PKCS12_EXPORT_PBES2_PARAMS 改用 PBES2 AES-256／SHA-256。
    const bool ex=symbol=="PFXExportCertStoreEx";const auto flags=u32(ex?4:3);
    constexpr std::uint32_t allowed=1U|2U|4U|0x10U|0x40U|0x80U|0x100U|0x200U|0x1000U;
    if(!arg(1) || (flags&~allowed) || (flags&0x300U)==0x300U)throw CertificateError{87};
    if(ex && arg(3)){
        // PKCS12_PBES2_EXPORT_PARAMS：dwSize、hNcryptDescriptor、pwszPbes2Alg；只支援 AES256-SHA256，不支援保護描述元。
        if(!(flags&0x80U))throw CertificateError{87};const auto size=memory.load(GuestAddress{arg(3)},32);if(size<3U*unit)throw CertificateError{87};
        if(memory.load(GuestAddress{arg(3)+unit},bits))throw CertificateError{50};
        const auto algorithm=memory.load(GuestAddress{arg(3)+2*unit},bits);if(algorithm && read_utf16(memory,GuestAddress{algorithm},64)!=u"AES256-SHA256")throw CertificateError{0x80090008};
    }
    if(!certs.stores.contains(arg(0)))throw CertificateError{87};
    struct Password {std::string value;~Password(){OPENSSL_cleanse(value.data(),value.size());}} password;
    try{if(arg(2))password.value=utf16_to_utf8(read_utf16(memory,GuestAddress{arg(2)},32768));}catch(const std::invalid_argument&){throw CertificateError{87};}
    PfxBundle bundle;
    for(const auto& entry:store_entries(certs,arg(0),0,false)){
        PfxCertificate item;item.der=*entry.der;if(const auto found=certs.properties.find(entry.properties);found!=certs.properties.end())item.properties=found->second;
        if(flags&4U){
            const auto context=create_certificate(memory,item.der,arg(0),entry.identity,0,0,false);
            std::uint64_t provider=0;std::uint32_t spec=0;bool caller_free=false;std::optional<std::uint32_t> missing;
            try{acquire_certificate_key(context,0x10040U,provider,spec,caller_free);}catch(const CertificateError& error){missing=error.code;}
            const auto metadata=certs.properties.find(certs.contexts.at(context).property_identity);
            std::optional<KeyProviderInfo> info;if(metadata!=certs.properties.end())if(const auto found=metadata->second.find(2);found!=metadata->second.end())info=key_info(found->second);
            (void)free_certificate(state,memory,context,false);
            if(missing){if(flags&1U)throw CertificateError{*missing};}
            else{
                PfxKey key;bool exportable=false;
                try{
                    if(spec==UINT32_MAX){
                        const auto& value=runtime_.storage_crypto_keys.at(provider);exportable=(value.export_policy&3U)!=0;
                        key.der=value.der;key.algorithm=value.algorithm;key.bits=value.bits;key.machine=value.machine;key.name=value.name;key.spec=0;key.provider="Microsoft Software Key Storage Provider";
                    }else{
                        const auto& value=runtime_.legacy_crypto_user_keys.at({provider,spec});exportable=value.exportable;key.der=value.der;key.bits=value.bits;key.algorithm="RSA";key.spec=spec;
                    }
                    if(info){if(!info->container.empty())key.name=info->container;if(!info->name.empty())key.provider=info->name;key.machine=(info->flags&32U)!=0;
                        if(spec!=UINT32_MAX && info->name.empty())key.provider=info->type==24?"Microsoft Enhanced RSA and AES Cryptographic Provider":"Microsoft Enhanced Cryptographic Provider v1.0";}
                }catch(...){if(caller_free){if(spec==UINT32_MAX)(void)release_storage_crypto_key(runtime_,provider);else (void)release_crypto_provider(provider);}throw;}
                if(caller_free){if(spec==UINT32_MAX)(void)release_storage_crypto_key(runtime_,provider);else (void)release_crypto_provider(provider);}
                if(!exportable){if(flags&2U)throw CertificateError{0x8009000bU};}
                else{item.key=bundle.keys.size();bundle.keys.push_back(std::move(key));}
            }
        }
        bundle.certificates.push_back(std::move(item));
    }
    PfxWriteOptions options;options.encrypt_certificates=!(flags&0x100U);options.pbes2=(flags&0x80U)!=0;options.extended_properties=(flags&0x10U)!=0;
    const auto bytes=write_pfx(bundle,arg(2)?password.value.c_str():nullptr,options);
    const auto capacity=memory.load(GuestAddress{arg(1)},32),data=memory.load(GuestAddress{arg(1)+unit},bits);memory.store(GuestAddress{arg(1)},32,bytes.size());
    if(data){if(capacity<bytes.size())throw CertificateError{234};memory.check(GuestAddress{data},bytes.size(),Access::Write);memory.write(GuestAddress{data},bytes);}
    result=1;
}else
