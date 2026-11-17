#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace x64arm {
using CertificateProperties=std::map<std::uint32_t,std::vector<std::uint8_t>>;
struct CertificateContext {
    std::vector<std::uint8_t> der;
    std::uint64_t store{}, identity{}, property_identity{};
    std::uint32_t references{1};
    std::uint64_t source_store{},source_identity{};
    std::shared_ptr<const std::vector<std::uint64_t>> enumeration{};
    std::size_t enumeration_cursor{};
    bool operator==(const CertificateContext& other) const {
        return der==other.der && store==other.store && identity==other.identity && property_identity==other.property_identity &&
            references==other.references && source_store==other.source_store && source_identity==other.source_identity && enumeration_cursor==other.enumeration_cursor &&
            ((!enumeration && !other.enumeration) || (enumeration && other.enumeration && *enumeration==*other.enumeration));
    }
};
struct CertificateSibling {
    std::uint64_t store{},identity{};
    std::uint32_t priority{};
    bool writable{};
    bool operator==(const CertificateSibling&) const = default;
};
struct CertificateStore {
    std::map<std::uint64_t,std::vector<std::uint8_t>> items;
    std::map<std::uint64_t,std::uint64_t> property_identities;
    std::map<std::uint64_t,std::vector<std::uint8_t>> crls;
    std::map<std::uint64_t,std::uint64_t> crl_property_identities;
    std::uint64_t next_identity{1};
    std::uint32_t references{1};
    std::uint32_t engine_references{};
    bool readonly{},enum_archived{};
    bool collection{},root_provider{},auto_resync{};
    std::uint32_t collection_references{};
    std::uint64_t next_sibling_identity{1};
    std::vector<CertificateSibling> siblings;
    std::map<std::pair<std::uint64_t,std::uint64_t>,std::uint64_t> aliases;
    // 系統存放區（ROOT 以外）：沙箱登錄中的機碼路徑，以及最近一次與登錄一致的內容（相對機碼 → Blob）。
    std::u16string system_path;
    std::map<std::u16string,std::vector<std::uint8_t>> system_saved;
    // CertSetStoreProperty 的存放區屬性（系統存放區開啟時帶 CERT_STORE_LOCALIZED_NAME_PROP_ID）；系統存放區的位置（CERT_SYSTEM_STORE_*_LOCATION）。
    std::map<std::uint32_t,std::vector<std::uint8_t>> properties;
    std::uint32_t system_location{};
    bool operator==(const CertificateStore&) const = default;
};
struct CertificateChain {
    std::uint64_t find_store{},find_identity{};
    std::vector<std::uint64_t> contexts;
    std::uint32_t references{1};
    bool operator==(const CertificateChain&) const = default;
};
struct CertificateEngine {
    std::uint64_t root{},other{};
    bool exclusive{};
    std::vector<std::uint64_t> additional,retained;
    bool operator==(const CertificateEngine&) const = default;
};
// CryptMsgOpenToEncode 的簽署者：開啟時取得的金鑰 DER 與 SignerInfo 所需的識別、雜湊與屬性。
struct CryptMessageSigner {
    std::vector<std::uint8_t> key,issuer,serial;bool private_key{};std::string hash_oid;
    std::vector<std::pair<std::string,std::vector<std::vector<std::uint8_t>>>> authenticated,unauthenticated;
    bool operator==(const CryptMessageSigner&) const = default;
};
struct CryptMessage {
    std::vector<std::uint8_t> encoded;
    std::uint32_t type{},references{1};
    bool finalized{};
    // 編碼用的訊息：累積的內容、分離簽章、簽署者、雜湊演算法、附帶的憑證與 CRL；完成時產生 encoded。
    bool encoder{},detached{};std::vector<std::uint8_t> content;std::vector<CryptMessageSigner> signers;std::string hash_oid;
    std::vector<std::vector<std::uint8_t>> certificates,crls;
    bool operator==(const CryptMessage&) const = default;
};
struct CryptOidFunction {
    std::string oid;
    std::uint64_t address{},module{};
    std::uint32_t encoding{};
    // 依登錄載入的 DLL（CryptFreeOIDFunctionAddress 時 FreeLibrary）；default_index 為預設 DLL 清單中的位置（逐一取用時接續）。
    bool loaded{};std::uint32_t default_index{UINT32_MAX};
    bool operator==(const CryptOidFunction&) const = default;
};
struct CryptOidFunctionSet {
    std::string name;
    std::vector<CryptOidFunction> functions;
    std::string original; // 建立時的函式名稱（保留大小寫，供 GetProcAddress 與登錄機碼）
    bool operator==(const CryptOidFunctionSet&) const = default;
};
// CryptFindOIDInfo 的項目：內建表與 CryptRegisterOIDInfo 登錄的項目。
struct CryptOidInfoEntry {
    std::string oid;std::u16string name;std::uint32_t group{},value{};std::vector<std::uint32_t> extra;std::u16string cng,cng_extra;
    bool operator==(const CryptOidInfoEntry&) const = default;
};
struct CertificateFind {
    std::uint64_t store{},identity{},pending_context{},callback{},argument{},chain_index{},element_index{};
    std::uint32_t flags{},key_spec{},acquire_flags{};
    std::string usage;
    std::vector<std::vector<std::uint8_t>> issuers;
    bool operator==(const CertificateFind&) const = default;
};
struct CertificateState {
    std::map<std::uint64_t,CertificateFind> finds;
    std::map<std::string,std::uint64_t> decoded_oids;
    std::uint64_t next_property_identity{1};
    std::map<std::uint64_t,CertificateProperties> properties;
    std::map<std::uint64_t,CertificateStore> stores;
    std::map<std::uint64_t,CertificateContext> contexts;
    std::map<std::uint64_t,CertificateContext> crl_contexts;
    std::map<std::uint64_t,CertificateChain> chains;
    std::map<std::uint64_t,CertificateEngine> engines;
    std::map<std::uint64_t,CryptMessage> messages;
    std::map<std::uint64_t,CryptOidFunctionSet> oid_sets;
    std::map<std::uint64_t,CryptOidFunction> oid_addresses;
    bool operator==(const CertificateState&) const = default;
};
}
