if(symbol=="PFXIsPFXBlob" || symbol=="PFXImportCertStore" || symbol=="PFXVerifyPassword"){
    const auto input=blob(arg(0));
    if(symbol=="PFXIsPFXBlob")result=is_pfx(input)?1:0;
    else {
        const auto flags=u32(2);const bool verify=symbol=="PFXVerifyPassword";
        constexpr std::uint32_t allowed=1U|2U|0x10U|0x20U|0x40U|0x100U|0x200U|0x1000U|0x4000U|0x8000U|0x10000000U;
        if(flags&~(verify?0U:allowed) || (flags&0x1020U)==0x1020U)throw CertificateError{87};
        if(flags&2U)throw CertificateError{flags&0x40U?0x80090022U:50U};
        if(!verify && certs.stores.size()>=128)throw CertificateError{8};
        struct Password {std::string value;~Password(){OPENSSL_cleanse(value.data(),value.size());}} password;
        try{if(arg(1))password.value=utf16_to_utf8(read_utf16(memory,GuestAddress{arg(1)},32768));}
        catch(const std::invalid_argument&){throw CertificateError{87};}
        auto bundle=read_pfx(input,arg(1)?password.value.c_str():nullptr,(flags&0x10U)!=0,verify);
        if(verify)result=1;
        else {
            const bool temporary=(flags&(0x8000U|0x10000000U))!=0;
            const auto key_machine=[&](const PfxKey& key){return !(flags&0x1000U) && ((flags&0x20U) || key.machine);};
            if(!temporary && !bundle.keys.empty() && !runtime_.network.external_io)throw HostCallFault("compare 不允許原生 PFX 私鑰儲存");
            struct Created {std::uint32_t type;std::string name;bool machine;};
            std::vector<Created> created;created.reserve(bundle.keys.size());
            std::vector<std::pair<std::uint64_t,std::uint32_t>> owned;owned.reserve(bundle.certificates.size());
            std::vector<std::uint64_t> properties;properties.reserve(bundle.certificates.size());
            std::vector<KeyProviderInfo> providers(bundle.keys.size());
            const auto is_cng=[&](const PfxKey& key){return key.algorithm!="RSA" || (flags&0x200U) || key.provider=="Microsoft Software Key Storage Provider" || ((flags&0x100U) && key.provider.empty());};
            const auto storage_key=[&](const PfxKey& key){StorageCryptoKey value;value.algorithm=key.algorithm;value.der=key.der;value.bits=key.bits;value.private_key=true;value.finalized=true;value.machine=key_machine(key);value.export_policy=flags&1U?3U:0U;value.usage=key.usage;return value;};
            const auto legacy_key=[&](const PfxKey& key,std::uint64_t provider){LegacyCryptKey value;value.provider=provider;value.algorithm=key.spec==1?0xa400:0x2400;value.bits=key.bits;value.der=key.der;value.private_key=true;value.exportable=(flags&1U)!=0;if(value.exportable)value.permissions|=4;return value;};
            const auto provider_type=[](const PfxKey& key){return key.provider=="Microsoft Enhanced RSA and AES Cryptographic Provider"?24U:1U;};
            try{
                // 所有憑證、MAC 與私鑰已通過驗證，才建立新容器；失敗只回收本次建立者。
                if(!temporary)for(std::size_t i=0;i<bundle.keys.size();++i){
                    const auto& key=bundle.keys[i];auto& info=providers[i];const bool cng_key=is_cng(key),machine=key_machine(key);
                    info.type=cng_key?0U:provider_type(key);info.spec=cng_key?UINT32_MAX:key.spec;info.flags=machine?32U:0U;
                    info.name=cng_key?"Microsoft Software Key Storage Provider":info.type==24?"Microsoft Enhanced RSA and AES Cryptographic Provider":"Microsoft Enhanced Cryptographic Provider v1.0";
                    info.container=key.name;
                    if(info.container.empty()){
                        std::array<std::uint8_t,16> random{};if(!random_bytes(random))throw CertificateError{8};info.container="MacWin-PFX-";
                        constexpr char hex[]="0123456789abcdef";for(const auto byte:random){info.container+=hex[byte>>4];info.container+=hex[byte&15];}
                    }
                    Created record{cng_key?storage_crypto_type:info.type,info.container,machine};
                    if(cng_key){
                        auto value=storage_key(key);value.name=info.container;auto status=save_storage_crypto_key(private_key_context(),value,true);const bool existing=status==0x8009000f;
                        if(existing && (flags&0x4000U))status=save_storage_crypto_key(private_key_context(),value,false);
                        OPENSSL_cleanse(value.der.data(),value.der.size());if(status)throw CertificateError{status};if(!existing)created.push_back(std::move(record));
                    }
                    else {
                        auto provider=acquire_crypto_provider(info.container,info.name,info.type,8U|info.flags|(flags&0x40U));const bool existing=!provider && last_error_==0x8009000f;
                        if(existing && (flags&0x4000U))provider=acquire_crypto_provider(info.container,info.name,info.type,info.flags|(flags&0x40U));
                        if(!provider)throw CertificateError{last_error_};if(!existing)created.push_back(std::move(record));
                        try{auto value=legacy_key(key,provider);const bool saved=store_crypto_user_key(provider,key.spec,value);OPENSSL_cleanse(value.der.data(),value.der.size());if(!saved)throw CertificateError{last_error_};}
                        catch(...){(void)release_crypto_provider(provider);throw;}
                        (void)release_crypto_provider(provider);
                    }
                }
                CertificateStore store;
                for(auto& cert:bundle.certificates){
                    auto values=std::move(cert.properties);
                    if(cert.key){
                        const auto& key=bundle.keys[*cert.key];
                        if(!temporary)values.emplace(2,key_info(providers[*cert.key]));
                        else if(is_cng(key)){
                            if(runtime_.storage_crypto_keys.size()>=4096)throw CertificateError{8};
                            const auto handle=runtime_.next_crypto_handle++;owned.emplace_back(handle,UINT32_MAX);
                            runtime_.storage_crypto_keys.emplace(handle,storage_key(key));values.emplace(5,key_context(handle,UINT32_MAX,true));
                        }else {
                            const auto handle=acquire_crypto_provider({}, {},provider_type(key),0xf0000000U|(flags&0x40U)|(key_machine(key)?32U:0U));if(!handle)throw CertificateError{last_error_};
                            owned.emplace_back(handle,key.spec);auto value=legacy_key(key,handle);const bool saved=store_crypto_user_key(handle,key.spec,value);OPENSSL_cleanse(value.der.data(),value.der.size());if(!saved)throw CertificateError{last_error_};
                            values.emplace(5,key_context(handle,key.spec,true));
                        }
                    }
                    const auto identity=store.next_identity++,property=certs.next_property_identity++;
                    properties.push_back(property);certs.properties.emplace(property,std::move(values));
                    store.items.emplace(identity,std::move(cert.der));store.property_identities.emplace(identity,property);
                }
                const auto handle=runtime_.allocate_wait_handle();certs.stores.emplace(handle,std::move(store));result=handle;
            }catch(...){
                for(const auto property:properties)certs.properties.erase(property);
                for(const auto& [handle,spec]:owned){if(spec==UINT32_MAX)(void)release_storage_crypto_key(runtime_,handle);else (void)release_crypto_provider(handle);}
                for(const auto& item:created)(void)private_key_store_delete(private_key_context(),item.type,item.name,item.machine);
                throw;
            }
        }
    }
}else
