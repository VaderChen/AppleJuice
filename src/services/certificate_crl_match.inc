// CRL 與發行者憑證的比對（由 windows_certificates.cpp 的匿名命名空間引入）：CertFindCRLInStore、CertGetCRLFromStore、
// CertIsValidCRLForCertificate 與撤銷檢查（CertVerifySubjectCertificateContext、CertGetIssuerCertificateFromStore）共用。
using AuthorityKeyId=std::unique_ptr<AUTHORITY_KEYID,decltype(&AUTHORITY_KEYID_free)>;
bool crl_is_delta(X509_CRL* crl){return X509_CRL_get_ext_by_NID(crl,NID_delta_crl,-1)>=0;}
// 名稱以編碼位元組比較（與 CertCompareCertificateName 相同）。
bool same_certificate_name(X509_NAME* left,X509_NAME* right){return encoded(left,i2d_X509_NAME)==encoded(right,i2d_X509_NAME);}
// CRL_FIND_ISSUED_BY 的旗標：AKI（1）、簽章（2）、只要 delta（4）、只要 base（8）；同時指定 4 與 8 時兩者皆可。
bool crl_issued_by(X509_CRL* crl,X509* issuer,const std::vector<std::uint8_t>& issuer_der,std::uint32_t flags){
    if(!same_certificate_name(X509_CRL_get_issuer(crl),X509_get_subject_name(issuer)))return false;
    const bool delta=crl_is_delta(crl);if((flags&12U)==4U && !delta)return false;if((flags&12U)==8U && delta)return false;
    if((flags&2U) && X509_CRL_verify(crl,X509_get0_pubkey(issuer))!=1){ERR_clear_error();return false;}
    if(flags&1U){
        // 沒有授權金鑰識別碼延伸的 CRL 與任何發行者相符；有發行者名稱與序號時比對兩者，否則比對金鑰識別碼。
        AuthorityKeyId key(static_cast<AUTHORITY_KEYID*>(X509_CRL_get_ext_d2i(crl,NID_authority_key_identifier,nullptr,nullptr)),AUTHORITY_KEYID_free);ERR_clear_error();
        if(key){
            if(key->issuer && key->serial){bool named=false;
                for(int i=0;i<sk_GENERAL_NAME_num(key->issuer) && !named;++i){const auto* name=sk_GENERAL_NAME_value(key->issuer,i);if(name->type==GEN_DIRNAME)named=same_certificate_name(name->d.directoryName,X509_get_subject_name(issuer));}
                if(!named || ASN1_INTEGER_cmp(key->serial,X509_get0_serialNumber(issuer))!=0)return false;
            }else if(key->keyid){const auto identifier=certificate_computed_property(CertificateContext{issuer_der},20);
                if(!identifier || identifier->size()!=static_cast<std::size_t>(ASN1_STRING_length(key->keyid)) || !std::equal(identifier->begin(),identifier->end(),ASN1_STRING_get0_data(key->keyid)))return false;
            }else return false;
        }
    }
    return true;
}
// CRL_FIND_EXISTING：發行者名稱相同，且同為 base 或同為 delta CRL。
bool crl_matches_existing(X509_CRL* crl,X509_CRL* existing){return same_certificate_name(X509_CRL_get_issuer(crl),X509_CRL_get_issuer(existing)) && crl_is_delta(crl)==crl_is_delta(existing);}
// CRL 目前有效：thisUpdate ≤ now，沒有 nextUpdate 或 now ≤ nextUpdate。
bool crl_time_valid(X509_CRL* crl,std::uint64_t now){
    const auto* next=X509_CRL_get0_nextUpdate(crl);return certificate_time(X509_CRL_get0_lastUpdate(crl))<=now && (!next || now<=certificate_time(next));
}
// 散布點名稱比對：完整名稱有任一 GENERAL_NAME 相同，或相對名稱的編碼相同。
bool distribution_point_names_match(const DIST_POINT_NAME* left,const DIST_POINT_NAME* right){
    if(!left || !right || left->type!=right->type)return false;
    if(left->type==0){
        for(int i=0;i<sk_GENERAL_NAME_num(left->name.fullname);++i)for(int j=0;j<sk_GENERAL_NAME_num(right->name.fullname);++j)
            if(GENERAL_NAME_cmp(sk_GENERAL_NAME_value(left->name.fullname,i),sk_GENERAL_NAME_value(right->name.fullname,j))==0)return true;
        return false;
    }
    const auto set=[](STACK_OF(X509_NAME_ENTRY)* entries){std::vector<std::vector<std::uint8_t>> out;for(int i=0;i<sk_X509_NAME_ENTRY_num(entries);++i){const auto* entry=sk_X509_NAME_ENTRY_value(entries,i);
        const auto* value=X509_NAME_ENTRY_get_data(entry);auto bytes=encoded(X509_NAME_ENTRY_get_object(entry),i2d_ASN1_OBJECT);bytes.push_back(static_cast<std::uint8_t>(ASN1_STRING_type(value)));
        const auto* data=ASN1_STRING_get0_data(value);bytes.insert(bytes.end(),data,data+ASN1_STRING_length(value));out.push_back(std::move(bytes));}
        std::sort(out.begin(),out.end());return out;};
    return set(left->name.relativename)==set(right->name.relativename);
}
