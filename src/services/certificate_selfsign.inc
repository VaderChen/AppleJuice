// 自簽憑證沿用 CSP／CNG 已完成的金鑰、憑證 context 與屬性管理。
if(service.symbol=="CertCreateSelfSignCertificate"){
    const WindowsCall call(state,memory,service);std::uint64_t result=0,owned_provider=0;bool failed=false;std::optional<StorageCryptoKey> stored_key;
    try{
        const unsigned unit=x86_?4:8;const auto width=static_cast<std::uint8_t>(unit*8);
        const auto get=[&](std::uint64_t at){return memory.load(GuestAddress{at},32);};
        const auto pointer=[&](std::uint64_t at){return memory.load(GuestAddress{at},width);};
        const auto blob=[&](std::uint64_t at){const auto size=get(at),data=pointer(at+unit);if(size>output_limit || (size && !data))throw CertificateError{87};std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));if(size)memory.read(GuestAddress{data},bytes);return bytes;};
        if(!call(1) || (call(2)&~UINT64_C(3)))throw CertificateError{87};
        const auto subject_bytes=blob(call(1));const auto* cursor=subject_bytes.data();
        std::unique_ptr<X509_NAME,decltype(&X509_NAME_free)> subject(d2i_X509_NAME(nullptr,&cursor,static_cast<long>(subject_bytes.size())),X509_NAME_free);
        if(!subject || cursor!=subject_bytes.data()+subject_bytes.size())throw CertificateError{0x8009310b};
        KeyProviderInfo info;if(call(3))info=read_key_info(memory,call(3),x86_);
        if(!info.parameters.empty())throw CertificateError{50};
        std::uint64_t handle=call(0);std::vector<std::uint8_t> key_der;
        if(!handle){
            if(call(3) && !info.type){
                if(!info.name.empty() && info.name!="Microsoft Software Key Storage Provider")throw CertificateError{0x80090013};
                if(info.flags&~0x60U)throw CertificateError{0x80090009};
                if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生金鑰儲存與金鑰產生");
                stored_key.emplace();const auto status=load_storage_crypto_key(private_key_context(),info.container,(info.flags&32U)!=0,0,*stored_key);
                if(status)throw CertificateError{status};
            }else{
            if(!call(3)){std::array<std::uint8_t,16> id{};if(!random_bytes(id))throw CertificateError{8};info.container="MacWin-SelfSign-";for(auto byte:id){info.container+="0123456789abcdef"[byte>>4];info.container+="0123456789abcdef"[byte&15];}info.type=1;info.spec=2;}
            handle=acquire_crypto_provider(info.container,info.name,info.type,info.flags&0x60U);
            if(!handle && last_error_==0x80090016U)handle=acquire_crypto_provider(info.container,info.name,info.type,(info.flags&0x60U)|8U);
            if(!handle)throw CertificateError{last_error_};owned_provider=handle;
            }
        }
        const auto cng=runtime_.storage_crypto_keys.find(handle);
        if(stored_key || cng!=runtime_.storage_crypto_keys.end()){
            const auto& key=stored_key?*stored_key:cng->second;if(!key.finalized || !key.private_key)throw CertificateError{0x8009000b};if(!(key.usage&2U))throw CertificateError{0x80090010};key_der=key.der;
            if(!call(3)){info.container=key.name;info.name="Microsoft Software Key Storage Provider";info.spec=UINT32_MAX;info.flags=key.machine?32U:0U;}
        }else{
            const auto provider=runtime_.crypt_providers.find(handle);if(provider==runtime_.crypt_providers.end())throw CertificateError{6};
            if(!call(3)){info.container=provider->second.container;info.name=provider->second.name;info.type=provider->second.type;info.flags=provider->second.flags&32U;info.spec=2;}
            if(info.spec!=1 && info.spec!=2)throw CertificateError{87};auto found=runtime_.legacy_crypto_user_keys.find({handle,info.spec});
            if(found==runtime_.legacy_crypto_user_keys.end()){
                const auto generated=legacy_crypto::generate_rsa(runtime_,2048,[&](std::span<std::uint8_t> bytes){return random_bytes(bytes);});if(!generated)throw CertificateError{0x80090005};
                LegacyCryptKey key;key.provider=handle;key.algorithm=info.spec==2?0x2400U:0xa400U;key.bits=2048;key.private_key=true;key.der=*generated;
                if(!store_crypto_user_key(handle,info.spec,key))throw CertificateError{last_error_};found=runtime_.legacy_crypto_user_keys.find({handle,info.spec});
            }key_der=found->second.der;
        }
        cursor=key_der.data();std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key(d2i_AutoPrivateKey(nullptr,&cursor,static_cast<long>(key_der.size())),EVP_PKEY_free);
        if(!key || cursor!=key_der.data()+key_der.size())throw CertificateError{0x80090003};
        std::string oid="1.2.840.113549.1.1.5";if(call(4)){oid=read_ansi(memory,pointer(call(4)),1024);const auto params=blob(call(4)+unit);if(!params.empty() && params!=std::vector<std::uint8_t>{5,0})throw CertificateError{50};}
        int digest_id=0,key_id=0;if(!OBJ_find_sigid_algs(OBJ_txt2nid(oid.c_str()),&digest_id,&key_id))throw CertificateError{0x80090008};
        const auto* digest=EVP_get_digestbynid(digest_id);if(!digest || (key_id==NID_rsaEncryption?!EVP_PKEY_is_a(key.get(),"RSA"):key_id==NID_X9_62_id_ecPublicKey?!EVP_PKEY_is_a(key.get(),"EC"):true))throw CertificateError{0x80090008};
        std::unique_ptr<X509,decltype(&X509_free)> cert(X509_new(),X509_free);if(!cert)throw CertificateError{8};
        std::array<std::uint8_t,16> serial{};if(!random_bytes(serial))throw CertificateError{8};serial[0]&=0x7f;serial[0]|=1;
        std::unique_ptr<BIGNUM,decltype(&BN_free)> number(BN_bin2bn(serial.data(),static_cast<int>(serial.size()),nullptr),BN_free);
        if(!number || !BN_to_ASN1_INTEGER(number.get(),X509_get_serialNumber(cert.get())) || !X509_set_version(cert.get(),2) || !X509_set_subject_name(cert.get(),subject.get()) || !X509_set_issuer_name(cert.get(),subject.get()) || !X509_set_pubkey(cert.get(),key.get()))throw CertificateError{0x80090005};
        const auto now=std::chrono::sys_seconds{std::chrono::seconds{static_cast<std::int64_t>((runtime_.system_filetime())/10000000)-11644473600LL}};
        const auto today=std::chrono::floor<std::chrono::days>(now);const std::chrono::year_month_day date{today};const std::chrono::hh_mm_ss time{now-today};
        std::array<unsigned,8> start{static_cast<unsigned>(static_cast<int>(date.year())),static_cast<unsigned>(date.month()),0,static_cast<unsigned>(date.day()),static_cast<unsigned>(time.hours().count()),static_cast<unsigned>(time.minutes().count()),static_cast<unsigned>(time.seconds().count()),0};
        const auto read_time=[&](std::uint64_t at){std::array<unsigned,8> value{};for(unsigned i=0;i<8;++i)value[i]=static_cast<unsigned>(memory.load(GuestAddress{at+2*i},16));return value;};
        if(call(5))start=read_time(call(5));auto end=start;++end[0];if(call(6))end=read_time(call(6));
        const auto set_time=[&](ASN1_TIME* out,const std::array<unsigned,8>& value){if(value[0]<1601 || value[0]>9999 || value[7]>999)throw CertificateError{87};char text[32]{};std::snprintf(text,sizeof text,"%04u%02u%02u%02u%02u%02uZ",value[0],value[1],value[3],value[4],value[5],value[6]);if(!ASN1_TIME_set_string_X509(out,text))throw CertificateError{87};};
        set_time(X509_getm_notBefore(cert.get()),start);set_time(X509_getm_notAfter(cert.get()),end);if(ASN1_TIME_compare(X509_get0_notBefore(cert.get()),X509_get0_notAfter(cert.get()))>0)throw CertificateError{87};
        if(call(7)){const auto count=get(call(7)),entries=pointer(call(7)+unit);if(count>4096 || (count && !entries))throw CertificateError{87};
            for(std::uint64_t i=0;i<count;++i){const auto at=entries+i*4*unit;const auto name=read_ansi(memory,pointer(at),1024);const auto bytes=blob(at+2*unit);
                std::unique_ptr<ASN1_OBJECT,decltype(&ASN1_OBJECT_free)> object(OBJ_txt2obj(name.c_str(),1),ASN1_OBJECT_free);std::unique_ptr<ASN1_OCTET_STRING,decltype(&ASN1_OCTET_STRING_free)> data(ASN1_OCTET_STRING_new(),ASN1_OCTET_STRING_free);
                if(!object || !data || !ASN1_OCTET_STRING_set(data.get(),bytes.data(),static_cast<int>(bytes.size())))throw CertificateError{87};std::unique_ptr<X509_EXTENSION,decltype(&X509_EXTENSION_free)> ext(X509_EXTENSION_create_by_OBJ(nullptr,object.get(),get(at+unit)!=0,data.get()),X509_EXTENSION_free);if(!ext || !X509_add_ext(cert.get(),ext.get(),-1))throw CertificateError{87};
            }
        }
        if(X509_sign(cert.get(),key.get(),digest)<=0)throw CertificateError{0x80090005};
        if(call(2)&1U){const ASN1_BIT_STRING* signature=nullptr;X509_get0_signature(&signature,nullptr,cert.get());if(!ASN1_BIT_STRING_set(const_cast<ASN1_BIT_STRING*>(signature),nullptr,0))throw CertificateError{0x80090005};}
        result=create_certificate(memory,encoded(cert.get(),i2d_X509));
        if(!(call(2)&2U))runtime_.certificates.properties.at(runtime_.certificates.contexts.at(result).property_identity).insert_or_assign(2,key_info(info));
    }catch(const CertificateError& error){failed=true;last_error_=error.code;}catch(const MemoryFault&){failed=true;last_error_=87;}catch(const std::bad_alloc&){failed=true;last_error_=8;}
    if(failed && result){const auto error=last_error_;(void)free_certificate(state,memory,result);result=0;last_error_=error;}
    if(owned_provider){const auto error=last_error_;(void)release_crypto_provider(owned_provider);last_error_=error;}ERR_clear_error();call.return_to(state,result);return std::nullopt;
}
