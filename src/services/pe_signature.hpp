#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace x64arm {
enum class PePublisher : std::uint8_t { Untrusted, Microsoft, Store, Whql };
struct PeSignature { PePublisher publisher{};std::string reason; };
// 使用明確的信任根驗證檔案雜湊、PKCS#7、用途與憑證鏈；不以名稱取代信任驗證。
PeSignature authenticate_pe(std::span<const std::uint8_t> bytes,std::string_view roots);
struct TrustCertificate {std::vector<std::uint8_t> der;bool trusted_root{},self_signed{};};
struct TrustAttribute {std::string oid;std::vector<std::vector<std::uint8_t>> values;};
struct TrustVerification {std::vector<std::uint8_t> issuer,serial,signature;std::string hash_oid,encryption_oid;std::vector<TrustAttribute> authenticated_attributes,unauthenticated_attributes;std::uint32_t status{0x800b0100U};std::string reason;std::vector<TrustCertificate> certificates;std::uint64_t verify_time{};};
// roots 為空時僅使用內建 Microsoft 公開根；Windows 呼叫端另傳入 Sandbox 根，不使用宿主信任來源。
TrustVerification verify_authenticode(std::span<const std::uint8_t>,std::string_view roots={});
TrustVerification verify_catalog(std::span<const std::uint8_t>,std::span<const std::uint8_t> member_hash,std::string_view roots={});
bool catalog_contains(std::span<const std::uint8_t>,std::span<const std::uint8_t> member_hash);
std::vector<std::uint8_t> catalog_file_hash(std::span<const std::uint8_t>,std::string_view algorithm);
PeSignature authenticate_microsoft_pe(std::span<const std::uint8_t> bytes);
}
