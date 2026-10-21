#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace x64arm {
// 客體金鑰物件保留可驗證的 DER；原生 EVP 指標不跨越 API 邊界。
struct StorageCryptoKey {
    std::string algorithm,name;
    std::vector<std::uint8_t> der;
    std::map<std::string,std::vector<std::uint8_t>> properties;
    std::uint32_t bits{},export_policy{},usage{0xffffff},legacy_spec{};
    bool machine{},overwrite{},finalized{},private_key{true},archived{};
    std::string user_scope; // 開啟時的虛擬使用者；切換 impersonation 不改變持久容器位置。
    bool operator==(const StorageCryptoKey&) const = default;
};
}
