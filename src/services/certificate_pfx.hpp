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
}
