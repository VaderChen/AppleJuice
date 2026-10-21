#pragma once
#include "x64arm/windows_runtime.hpp"
#include "x64arm/private_key_store.hpp"
namespace x64arm {
// 共用 NCrypt 的儲存格式、私鑰完整性驗證及匯出政策。
inline constexpr std::uint32_t storage_crypto_type=0x4e435259;
std::uint32_t save_storage_crypto_key(const PrivateKeyStoreContext&,const StorageCryptoKey&,bool create);
std::uint32_t load_storage_crypto_key(const PrivateKeyStoreContext&,std::string_view name, bool machine,
                                      std::uint32_t legacy_spec, StorageCryptoKey& result);
bool release_storage_crypto_key(RuntimeState& runtime, std::uint64_t handle);
}
