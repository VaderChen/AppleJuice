#pragma once
#include <cstdint>
#include <optional>

namespace x64arm {
// 所有 HTTPS／TLS 用戶端共用的政策：WinHTTP／WinINet／urlmon（經 WinINet）、WebSocket、PAC、
// CRL 下載走宿主 HTTP 傳輸（x64arm/http.hpp），Schannel 走 windows_tls.cpp；兩者的
// 協定版本範圍、憑證例外與信任來源（sandbox_certificate_roots）都由這裡決定，
// 各傳輸只負責把結果套到自己的 TLS 函式庫，不得各自重寫對應規則。

// WINHTTP_FLAG_SECURE_PROTOCOL_* 與 Schannel 的 SP_PROT_*_CLIENT 使用相同位元。
inline constexpr std::uint32_t tls_protocol_tls10=0x80,tls_protocol_tls11=0x200,tls_protocol_tls12=0x800,tls_protocol_tls13=0x2000;
inline constexpr std::uint32_t tls_protocol_default=tls_protocol_tls12|tls_protocol_tls13;
struct TlsProtocolRange {
    // TLS 線上版本號（0x0301＝TLS 1.0 … 0x0304＝TLS 1.3），與 OpenSSL 的 TLS*_VERSION 相同。
    std::uint16_t minimum{},maximum{};
    // 範圍內未啟用的版本（例如只開 1.0 與 1.2 時的 1.1），支援逐版關閉的函式庫須另外排除。
    std::uint32_t disabled{};
};
// mask 為 0 時使用 fallback；沒有任何可用的 TLS 版本（例如只要求 SSL 3.0）回傳 nullopt。
constexpr std::optional<TlsProtocolRange> tls_protocol_range(std::uint32_t mask,std::uint32_t fallback=tls_protocol_default) {
    if(!mask)mask=fallback;
    constexpr std::uint32_t bits[]{tls_protocol_tls10,tls_protocol_tls11,tls_protocol_tls12,tls_protocol_tls13};
    TlsProtocolRange range;
    for(unsigned i=0;i<4;++i)if(mask&bits[i]){const auto version=static_cast<std::uint16_t>(0x0301+i);if(!range.minimum)range.minimum=version;range.maximum=version;}
    if(!range.minimum)return std::nullopt;
    for(unsigned i=0;i<4;++i){const auto version=0x0301+i;if(version>range.minimum && version<range.maximum && !(mask&bits[i]))range.disabled|=bits[i];}
    return range;
}

// 統一的憑證例外位元；WinHTTP／WinINet 的 SECURITY_FLAG_IGNORE_* 與 Schannel 的 SCH_CRED_* 都先轉成這組。
enum TlsCertificateException : std::uint32_t {
    tls_ignore_date=0x1,                  // 憑證未生效／已過期
    tls_ignore_unknown_ca=0x2,            // 鏈無法連到信任根
    tls_ignore_name=0x4,                  // 主機名稱／IP 不符
    tls_ignore_usage=0x8,                 // 用途不符
    tls_ignore_revocation=0x10,           // 所有撤銷檢查錯誤
    tls_ignore_revocation_offline=0x20,   // 取不到 CRL
    tls_ignore_revocation_no_location=0x40, // 憑證沒有 CRL 位置
    tls_ignore_revocation_root=0x80,      // 根憑證不做撤銷檢查
};
// SECURITY_FLAG_IGNORE_REVOCATION 0x80、UNKNOWN_CA 0x100、WRONG_USAGE 0x200、CERT_CN_INVALID 0x1000、CERT_DATE_INVALID 0x2000。
constexpr std::uint32_t winhttp_certificate_exceptions(std::uint32_t security_flags) {
    return ((security_flags&0x80U)?tls_ignore_revocation:0U)|((security_flags&0x100U)?tls_ignore_unknown_ca:0U)|
        ((security_flags&0x200U)?tls_ignore_usage:0U)|((security_flags&0x1000U)?tls_ignore_name:0U)|((security_flags&0x2000U)?tls_ignore_date:0U);
}
// SCH_CRED_NO_SERVERNAME_CHECK 0x4、REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT 0x400、IGNORE_NO_REVOCATION_CHECK 0x800、IGNORE_REVOCATION_OFFLINE 0x1000。
constexpr std::uint32_t schannel_certificate_exceptions(std::uint32_t credential_flags) {
    return ((credential_flags&0x4U)?tls_ignore_name:0U)|((credential_flags&0x400U)?tls_ignore_revocation_root:0U)|
        ((credential_flags&0x800U)?tls_ignore_revocation_no_location:0U)|((credential_flags&0x1000U)?tls_ignore_revocation_offline:0U);
}
struct TlsCertificateProblem {
    int code{};                  // X509_V_ERR_*（OpenSSL 與 LibreSSL 的數值相同）
    bool self_signed_root{};     // 錯誤發生在鏈尾的自簽根憑證
    bool crl_location{true};     // 憑證帶有 CRL 發布點
};
// 驗證回呼遇到錯誤時是否依例外放行。簽章錯誤、已撤銷等其他錯誤一律不放行，
// 不能因另一種例外而被連帶忽略。
constexpr bool tls_certificate_problem_ignored(const TlsCertificateProblem& problem,std::uint32_t exceptions) {
    switch(problem.code) {
    case 9:case 10:return exceptions&tls_ignore_date;                                   // CERT_NOT_YET_VALID／CERT_HAS_EXPIRED
    case 2:case 18:case 19:case 20:case 21:return exceptions&tls_ignore_unknown_ca;     // 簽發者不明、自簽、無法驗證末端
    case 26:return exceptions&tls_ignore_usage;                                         // INVALID_PURPOSE
    case 62:case 64:return exceptions&tls_ignore_name;                                  // HOSTNAME／IP_ADDRESS_MISMATCH
    case 3:                                                                             // UNABLE_TO_GET_CRL
        return (exceptions&(tls_ignore_revocation|tls_ignore_revocation_offline)) ||
            ((exceptions&tls_ignore_revocation_no_location) && !problem.crl_location) ||
            ((exceptions&tls_ignore_revocation_root) && problem.self_signed_root);
    case 11:case 12:case 15:case 16:case 33:return exceptions&tls_ignore_revocation;    // CRL 日期錯誤、取不到 CRL 簽發者
    default:return false;
    }
}
}
