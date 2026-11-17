// CRL 撤銷檢查（由 windows_certificates.cpp 在 invoke_certificate 之後引入）：在存放區中找發行者簽發且簽章驗證通過的 CRL，
// 找不到時回傳 nullopt，否則回傳主體憑證是否列在其中（任一份相符的 CRL 列出即視為已撤銷）。
std::optional<bool> WindowsServices::certificate_crl_revoked(std::uint64_t store,const std::vector<std::uint8_t>& issuer_der,const std::vector<std::uint8_t>& subject_der){
    auto& certs=runtime_.certificates;if(!store || !certs.stores.contains(store))return std::nullopt;
    const auto issuer=decode_certificate(issuer_der);const auto subject=decode_certificate(subject_der);std::optional<bool> found;
    for(const auto& item:store_entries(certs,store,0,true)){
        const auto crl=decode_crl(*item.der);if(!crl_issued_by(crl.get(),issuer.get(),issuer_der,2U))continue;
        X509_REVOKED* entry=nullptr;const bool revoked=X509_CRL_get0_by_cert(crl.get(),&entry,subject.get())==1;ERR_clear_error();
        if(revoked)return true;found=false;
    }
    return found;
}
