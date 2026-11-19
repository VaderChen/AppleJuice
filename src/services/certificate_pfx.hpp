#pragma once
#include "x64arm/certificates.hpp"
#include <optional>
#include <span>

namespace x64arm::certificate_detail {
struct PfxKey {
    std::vector<std::uint8_t> der;
    std::string algorithm,provider,name;
    std::uint32_t bits{},spec{1},usage{0xffffff};
    bool machine{};
    PfxKey()=default;
    PfxKey(PfxKey&&)=default;
    PfxKey& operator=(PfxKey&&)=default;
    ~PfxKey();
};
struct PfxCertificate {
    std::vector<std::uint8_t> der;
    CertificateProperties properties;
    std::optional<std::size_t> key;
};
struct PfxBundle {std::vector<PfxCertificate> certificates;std::vector<PfxKey> keys;};
bool is_pfx(std::span<const std::uint8_t>);
// 先完整驗證所有 safe bag 與金鑰，再交給 Windows 層建立 store／provider。
PfxBundle read_pfx(std::span<const std::uint8_t>,const char* password,bool extended_properties,bool password_only=false);
// 匯出：金鑰一律加密（3DES 或 PBES2 AES-256），憑證預設加密、可改為明文；金鑰包含容器名稱（friendlyName）、CSP 名稱、LocalKeySet 與金鑰用途，
// 憑證包含 friendlyName 與 extended 時的 1.3.6.1.4.1.311.17.3.<屬性> 擴充屬性；與 read_pfx 往返一致。
struct PfxWriteOptions {bool encrypt_certificates{true},pbes2{},extended_properties{};};
std::vector<std::uint8_t> write_pfx(const PfxBundle&,const char* password,const PfxWriteOptions&);
}
