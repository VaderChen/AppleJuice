#pragma once
#include "x64arm/tls_policy.hpp"
#include <cstdint>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

namespace x64arm {
// OpenSSL 驗證回呼的共用部分：Schannel（windows_tls.cpp）與宿主 HTTP 傳輸（libcurl，同一份靜態 OpenSSL）都經這裡。
// 取出錯誤碼與撤銷例外需要的鏈資訊，交給 tls_certificate_problem_ignored 判斷；放行時把錯誤改為 X509_V_OK，
// 讓交握後的 SSL_get_verify_result 也一致。exceptions 為 tls_policy 的統一例外位元。
inline int tls_verify_with_exceptions(int valid,X509_STORE_CTX* store,std::uint32_t exceptions) {
    if(valid)return 1;
    TlsCertificateProblem problem{X509_STORE_CTX_get_error(store)};
    if(problem.code==X509_V_ERR_UNABLE_TO_GET_CRL) {
        auto* certificate=X509_STORE_CTX_get_current_cert(store);
        const auto* chain=X509_STORE_CTX_get0_chain(store);
        problem.crl_location=certificate && X509_get_ext_by_NID(certificate,NID_crl_distribution_points,-1)>=0;
        problem.self_signed_root=certificate && chain && X509_STORE_CTX_get_error_depth(store)==sk_X509_num(chain)-1 &&
            X509_check_issued(certificate,certificate)==X509_V_OK;
    }
    if(!tls_certificate_problem_ignored(problem,exceptions))return 0;
    X509_STORE_CTX_set_error(store,X509_V_OK);return 1;
}
}
