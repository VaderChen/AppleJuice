// crypt32 CRL 查詢、存放區屬性與內部輔助（由 windows_certificates.cpp 的 invoke_certificate 引入，比對沿用 certificate_crl_match.inc）：
// - CertFindCRLInStore：CRL_FIND_ANY／ISSUED_BY（AKI、簽章、delta／base 旗標）／EXISTING／ISSUED_FOR，釋放前一個 context，找不到為 CRYPT_E_NOT_FOUND。
// - CertGetCRLFromStore：依發行者（NULL 為任一）取得 CRL；CERT_STORE_SIGNATURE_FLAG／TIME_VALIDITY_FLAG 驗證通過時清除，BASE／DELTA 旗標選擇類型。
// - CertFindCertificateInCRL：在 CRL 項目中找憑證序號，傳回 context 的 CRL_INFO 內的 CRL_ENTRY 指標，沒有時為 NULL。
// - CertIsValidCRLForCertificate：CRL 有發行散布點延伸時，須與憑證的 CRL 散布點名稱相符，否則 CRYPT_E_NO_MATCH。
// - CertVerifySubjectCertificateContext：簽章、有效期間與撤銷（找不到 CRL 時加上 CERT_STORE_NO_CRL_FLAG），通過的檢查清除旗標。
// - CertGetStoreProperty／CertSetStoreProperty：存放區屬性；CERT_ACCESS_STATE_PROP_ID 依存放區種類計算且不可設定，系統存放區開啟時帶本地化名稱。
// - I_CryptFreeTls／I_CryptDetachTls（沿用 TlsFree、TlsGetValue／TlsSetValue）、I_CryptReadTrustedPublisherDWORDValueFromRegistry（TrustedPublisher\Safer 原則值）、
//   I_CryptGetDefaultCryptProv（程序內共用、不需釋放的 PROV_RSA_AES 驗證用 CSP；指定演算法時須為該 CSP 支援的演算法）。
if(symbol=="CertFindCRLInStore" || symbol=="CertGetCRLFromStore"){
    const bool get=symbol=="CertGetCRLFromStore";const auto previous_pointer=arg(get?2:5);
    std::uint32_t type=0,flags=0,checks=0;std::uint64_t issuer_pointer=0,subject_pointer=0;std::vector<std::uint8_t> existing_der;
    if(get){
        // (hCertStore, pIssuerContext, pPrevCrlContext, pdwFlags)
        if(!arg(3))throw CertificateError{87};checks=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));if(checks&~0x303U)throw CertificateError{0x80070057};
        issuer_pointer=arg(1);type=issuer_pointer?1U:0U;if(checks&0x100U)flags|=8U;if(checks&0x200U)flags|=4U;
    }else{
        // (hCertStore, dwCertEncodingType, dwFindFlags, dwFindType, pvFindPara, pPrevCrlContext)
        type=u32(3);flags=u32(2);const auto parameter=arg(4);
        if(type==1)issuer_pointer=parameter;
        else if(type==2){if(parameter){const auto found=certs.crl_contexts.find(parameter);if(found==certs.crl_contexts.end())throw CertificateError{87};existing_der=found->second.der;}}
        else if(type==3){if(!parameter)throw CertificateError{87};subject_pointer=memory.load(GuestAddress{parameter},bits);issuer_pointer=memory.load(GuestAddress{parameter+unit},bits);if(!subject_pointer || !issuer_pointer)throw CertificateError{87};}
        else if(type!=0)throw CertificateError{0x80070057};
    }
    std::uint64_t previous=0;bool valid_previous=true;
    if(previous_pointer){const auto old=certs.crl_contexts.find(previous_pointer);valid_previous=old!=certs.crl_contexts.end() && old->second.store==arg(0);if(valid_previous)previous=old->second.identity;(void)free_certificate(state,memory,previous_pointer,true);}
    const auto store=certs.stores.find(arg(0));if(!valid_previous || store==certs.stores.end() || !store->second.references)throw CertificateError{0x80070057};
    if(!previous)auto_resync_store(certs,arg(0),filesystem_);
    Certificate issuer(nullptr,X509_free),subject(nullptr,X509_free);std::vector<std::uint8_t> issuer_der;
    if(issuer_pointer){const auto found=certs.contexts.find(issuer_pointer);if(found==certs.contexts.end())throw CertificateError{87};issuer_der=found->second.der;issuer=decode_certificate(issuer_der);}
    if(subject_pointer){const auto found=certs.contexts.find(subject_pointer);if(found==certs.contexts.end())throw CertificateError{87};subject=decode_certificate(found->second.der);}
    RevocationList existing(nullptr,X509_CRL_free);if(!existing_der.empty())existing=decode_crl(existing_der);
    const auto kind=[&](X509_CRL* crl){const bool delta=crl_is_delta(crl);return (flags&12U)==4U?delta:(flags&12U)==8U?!delta:true;};
    bool after=!previous;
    for(const auto& item:store_entries(certs,arg(0),0,true)){
        if(!after){if(item.identity==previous){after=true;continue;}if(!store->second.collection && item.identity>previous)after=true;else continue;}
        if(!store->second.enum_archived && certs.properties.at(item.properties).contains(19))continue;
        const auto crl=decode_crl(*item.der);bool matches=true;
        if(type==1 || type==3)matches=issuer?crl_issued_by(crl.get(),issuer.get(),issuer_der,flags):kind(crl.get());
        else if(type==0)matches=!get || kind(crl.get());
        if(matches && subject)matches=same_certificate_name(X509_CRL_get_issuer(crl.get()),X509_get_issuer_name(subject.get()));
        if(matches && existing)matches=crl_matches_existing(crl.get(),existing.get());
        if(!matches)continue;
        result=create_certificate(memory,*item.der,arg(0),item.identity,item.store,item.source_identity,true);
        if(get){
            if((checks&2U) && crl_time_valid(crl.get(),runtime_.system_filetime()))checks&=~2U;
            if((checks&1U) && issuer && X509_CRL_verify(crl.get(),X509_get0_pubkey(issuer.get()))==1)checks&=~1U;ERR_clear_error();
            memory.store(GuestAddress{arg(3)},32,checks);
        }
        break;
    }
    if(!result)throw CertificateError{0x80092004};
    arg.return_to(state,result);return std::nullopt;
}
if(symbol=="CertFindCertificateInCRL"){
    // (pCert, pCrlContext, dwFlags, pvReserved, ppCrlEntry)：比較序號時去掉高位的 0 位元組。
    if(!arg(4))throw CertificateError{87};memory.store(GuestAddress{arg(4)},bits,0);
    if(!certs.contexts.contains(arg(0)) || !certs.crl_contexts.contains(arg(1)))throw CertificateError{87};
    const auto certificate_info=memory.load(GuestAddress{arg(0)+3*unit},bits),crl_info=memory.load(GuestAddress{arg(1)+3*unit},bits);
    const auto trimmed=[](std::vector<std::uint8_t> value){while(value.size()>1 && !value.back())value.pop_back();return value;};const auto serial=trimmed(blob(certificate_info+unit));
    const auto count=memory.load(GuestAddress{crl_info+6*unit+16},32),entries=memory.load(GuestAddress{crl_info+(x86_?44U:72U)},bits);const std::uint64_t size=x86_?24U:40U;
    if(count>65536 || (count && !entries))throw CertificateError{87};
    for(std::uint64_t n=0;n<count;++n)if(trimmed(blob(entries+n*size))==serial){memory.store(GuestAddress{arg(4)},bits,entries+n*size);break;}
    result=1;arg.return_to(state,result);return std::nullopt;
}
if(symbol=="CertIsValidCRLForCertificate"){
    // (pCert, pCrl, dwFlags, pvReserved)：pCert 為 NULL 時為 TRUE。
    if(arg(0)){
        const auto certificate=certs.contexts.find(arg(0));const auto crl_context=certs.crl_contexts.find(arg(1));if(certificate==certs.contexts.end() || crl_context==certs.crl_contexts.end())throw CertificateError{87};
        const auto crl=decode_crl(crl_context->second.der);
        std::unique_ptr<ISSUING_DIST_POINT,decltype(&ISSUING_DIST_POINT_free)> point(static_cast<ISSUING_DIST_POINT*>(X509_CRL_get_ext_d2i(crl.get(),NID_issuing_distribution_point,nullptr,nullptr)),ISSUING_DIST_POINT_free);ERR_clear_error();
        if(point){
            const auto x509=decode_certificate(certificate->second.der);
            std::unique_ptr<CRL_DIST_POINTS,decltype(&CRL_DIST_POINTS_free)> points(static_cast<CRL_DIST_POINTS*>(X509_get_ext_d2i(x509.get(),NID_crl_distribution_points,nullptr,nullptr)),CRL_DIST_POINTS_free);ERR_clear_error();
            // 憑證沒有 CRL 散布點時無法對應有發行散布點的 CRL；發行散布點沒有名稱時不限制名稱。
            bool matched=points && !point->distpoint;
            for(int i=0;points && !matched && i<sk_DIST_POINT_num(points.get());++i)matched=distribution_point_names_match(sk_DIST_POINT_value(points.get(),i)->distpoint,point->distpoint);
            if(!matched)throw CertificateError{0x80092009};
        }
    }
    result=1;arg.return_to(state,result);return std::nullopt;
}
if(symbol=="CertVerifySubjectCertificateContext"){
    // (pSubject, pIssuer, pdwFlags)：簽章與撤銷檢查需要發行者。
    if(!arg(2))throw CertificateError{87};auto checks=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)},32));if(checks&~7U)throw CertificateError{0x80070057};
    const auto subject_context=certs.contexts.find(arg(0));if(subject_context==certs.contexts.end())throw CertificateError{87};
    const auto issuer_context=arg(1)?certs.contexts.find(arg(1)):certs.contexts.end();if(arg(1) && issuer_context==certs.contexts.end())throw CertificateError{87};
    if((checks&5U) && !arg(1))throw CertificateError{0x80070057};
    const auto subject=decode_certificate(subject_context->second.der);
    if(checks&4U){const auto store=issuer_context->second.store?issuer_context->second.store:subject_context->second.store;
        const auto revoked=certificate_crl_revoked(store,issuer_context->second.der,subject_context->second.der);if(!revoked)checks|=0x10000U;else if(!*revoked)checks&=~4U;}
    if(checks&2U){const auto now=runtime_.system_filetime();if(now>=certificate_time(X509_get0_notBefore(subject.get())) && now<=certificate_time(X509_get0_notAfter(subject.get())))checks&=~2U;}
    if(checks&1U){const auto issuer=decode_certificate(issuer_context->second.der);if(X509_verify(subject.get(),X509_get0_pubkey(issuer.get()))==1)checks&=~1U;ERR_clear_error();}
    memory.store(GuestAddress{arg(2)},32,checks);result=1;arg.return_to(state,result);return std::nullopt;
}
if(symbol=="CertGetStoreProperty" || symbol=="CertSetStoreProperty"){
    // Get(hCertStore, dwPropId, pvData, pcbData)／Set(hCertStore, dwPropId, dwFlags, const CRYPT_DATA_BLOB* pvData)
    const auto store=certs.stores.find(arg(0));if(store==certs.stores.end() || !store->second.references)throw CertificateError{0x80070057};const auto id=u32(1);auto& properties=store->second.properties;
    if(symbol=="CertSetStoreProperty"){
        if(id==14)throw CertificateError{0x80070057};
        if(!arg(3))properties.erase(id);
        else{auto value=blob(arg(3));if(!properties.contains(id) && properties.size()>=1024)throw CertificateError{8};properties.insert_or_assign(id,std::move(value));}
    }else{
        if(!arg(3))throw CertificateError{87};std::vector<std::uint8_t> data;
        if(id==14){
            // CERT_ACCESS_STATE_PROP_ID：可寫回的持久存放區（1）、系統存放區（2）、本機電腦系統存放區（4）。
            const auto& value=store->second;const bool system=value.root_provider || !value.system_path.empty();std::uint32_t access=0;
            if(!value.system_path.empty() && !value.readonly)access|=1U;if(system)access|=2U;if(system && value.system_location==0x20000U)access|=4U;
            data={static_cast<std::uint8_t>(access),0,0,0};
        }else{const auto found=properties.find(id);if(found==properties.end())throw CertificateError{0x80092004};data=found->second;}
        const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));memory.store(GuestAddress{arg(3)},32,data.size());
        if(arg(2)){if(capacity<data.size())throw CertificateError{234};if(!data.empty())memory.write(GuestAddress{arg(2)},data);}
    }
    result=1;arg.return_to(state,result);return std::nullopt;
}
if(symbol=="I_CryptFreeTls"){
    // (dwTlsIndex, dwFlags)：第二個參數不使用。
    result=nested_service(state,memory,"kernel32.dll","TlsFree",{u32(0)})&UINT32_MAX;arg.return_to(state,result);return std::nullopt;
}
if(symbol=="I_CryptDetachTls"){
    // 取回目前執行緒的值並將槽清為 NULL。
    auto value=nested_service(state,memory,"kernel32.dll","TlsGetValue",{u32(0)});if(x86_)value&=UINT32_MAX;
    result=(nested_service(state,memory,"kernel32.dll","TlsSetValue",{u32(0),0})&UINT32_MAX)?value:0;arg.return_to(state,result);return std::nullopt;
}
if(symbol=="I_CryptReadTrustedPublisherDWORDValueFromRegistry"){
    // (pwszValueName, pdwValue)：HKLM\Software\Policies\Microsoft\SystemCertificates\TrustedPublisher\Safer 的 4 位元組值；沒有時 *pdwValue 為 0 並回傳 FALSE。
    if(!arg(1))throw CertificateError{87};memory.store(GuestAddress{arg(1)},32,0);if(!arg(0))throw CertificateError{87};
    auto name=read_utf16(memory,GuestAddress{arg(0)},16383);for(auto& c:name)c=unicode_case_unit(c,false);
    if(registry_file_.empty() || runtime_.network.external_io){
        RegistryTransaction transaction(runtime_.registry,registry_file_,&registry_store_cache_,true);
        if(!transaction.error){initialize_registry_views(runtime_.registry);std::u16string logical(1,2);logical+=u"\\software\\policies\\microsoft\\systemcertificates\\trustedpublisher\\safer";
            if(const auto key=runtime_.registry.keys.find(registry_view_path(logical,registry_view(0,x86_)));key!=runtime_.registry.keys.end())
                if(const auto value=key->second.find(name);value!=key->second.end() && value->second.bytes.size()==4){memory.write(GuestAddress{arg(1)},value->second.bytes);result=1;}}
    }
    arg.return_to(state,result);return std::nullopt;
}
if(symbol=="I_CryptGetDefaultCryptProv"){
    // (ALG_ID)：0 為任一演算法；回傳的 handle 由 crypt32 持有，呼叫端不釋放。
    const auto algorithm=u32(0);if(algorithm){const auto* item=legacy_crypto::find(algorithm);if(!item || !legacy_crypto::supported(*item,24))throw CertificateError{0x80090008};}
    if(!crypt_default_provider_){
        GuestScratch scratch(*this,memory,state);const auto out=scratch.allocate(8);if(!out)throw CertificateError{8};memory.store(GuestAddress{out},bits,0);
        if(!(nested_service(state,memory,"advapi32.dll","CryptAcquireContextW",{out,0,0,24,0xF0000000U})&UINT32_MAX))throw CertificateError{last_error_?last_error_:0x80090020};
        crypt_default_provider_=memory.load(GuestAddress{out},bits);
    }
    result=crypt_default_provider_;arg.return_to(state,result);return std::nullopt;
}
