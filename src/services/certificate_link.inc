// CertAddCertificateLinkToStore（由 windows_certificates.cpp 的 invoke_certificate 引入）：在存放區加入指向原憑證的連結，
// 連結與原 context 共用同一組屬性（從任一方設定屬性雙方都看得到），刪除連結不影響原憑證。
// 處置方式與 CertAddCertificateContextToStore 相同（簽發者與序號相同視為同一張）；集合存放區不可加入連結（E_INVALIDARG），唯讀存放區為 E_ACCESSDENIED。
if(symbol=="CertAddCertificateLinkToStore"){
    // (hCertStore, pCertContext, dwAddDisposition, ppStoreContext)
    const auto output=arg(3);if(output)memory.store(GuestAddress{output},bits,0);
    const auto store=certs.stores.find(arg(0));if(store==certs.stores.end() || !store->second.references || store->second.collection)throw CertificateError{0x80070057};
    if(store->second.readonly)throw CertificateError{0x80070005};const auto disposition=u32(2);if(!disposition || disposition>7)throw CertificateError{0x80070057};
    const auto source=certs.contexts.find(arg(1));if(source==certs.contexts.end())throw CertificateError{87};
    const auto der=source->second.der;const auto shared=source->second.property_identity;auto& destination=store->second;
    const auto incoming=decode_certificate(der);std::uint64_t existing=0;
    if(disposition!=4)for(const auto& [identity,value]:destination.items){const auto old=decode_certificate(value);if(X509_issuer_and_serial_cmp(old.get(),incoming.get())==0){existing=identity;break;}}
    if(existing){
        if(disposition==1)throw CertificateError{0x80092005};
        if(disposition==2){if(output)memory.store(GuestAddress{output},bits,create_certificate(memory,destination.items.at(existing),arg(0),existing));result=1;arg.return_to(state,result);return std::nullopt;}
        const auto old=decode_certificate(destination.items.at(existing));
        if(disposition>=6 && certificate_time(X509_get0_notBefore(incoming.get()))<=certificate_time(X509_get0_notBefore(old.get())))throw CertificateError{0x80092005};
        // 繼承屬性的處置把原有項目的屬性補進共用屬性（不覆蓋連結來源已有的值）。
        if(disposition==5 || disposition==7){auto& values=certs.properties.at(shared);for(const auto& [id,value]:certs.properties.at(destination.property_identities.at(existing)))if(id!=5)values.try_emplace(id,value);}
        destination.items.erase(existing);destination.property_identities.erase(existing);
    }
    if(destination.items.size()>=4096 || destination.next_identity==UINT64_MAX)throw CertificateError{8};
    const auto identity=destination.next_identity++;destination.items.emplace(identity,der);destination.property_identities.emplace(identity,shared);
    if(output){std::uint64_t context=0;try{context=create_certificate(memory,der,arg(0),identity);}catch(...){destination.items.erase(identity);destination.property_identities.erase(identity);throw;}memory.store(GuestAddress{output},bits,context);}
    prune_certificate_properties();result=1;
    // 系統存放區與 CertAdd* 相同寫回沙箱登錄。
    if(const auto error=persist_system_certificate_stores()){last_error_=error;result=0;}
    arg.return_to(state,result);return std::nullopt;
}
