#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <vector>
namespace x64arm {
struct DataProtectionError:std::runtime_error{
    std::uint32_t code;
    explicit DataProtectionError(std::uint32_t value):std::runtime_error("原生資料保護操作失敗"),code(value){}
};
// 持久金鑰只存在目前 sandbox 的 C: 與虛擬使用者／machine 範圍。
struct PrivateKeyStoreContext;
std::array<std::uint8_t,32> sandbox_data_protection_key(const PrivateKeyStoreContext&,std::string_view identity,bool machine,bool create);
// DPAPI 與 WLAN 設定檔共用同一格式、驗證及 sandbox 金鑰作用域。
std::vector<std::uint8_t> protect_sandbox_data(const PrivateKeyStoreContext&,std::span<const std::uint8_t>,std::u16string_view,bool machine,std::span<const std::uint8_t> entropy={});
struct UnprotectedData {std::vector<std::uint8_t> bytes,description;};
UnprotectedData unprotect_sandbox_data(const PrivateKeyStoreContext&,std::span<const std::uint8_t>,std::span<const std::uint8_t> entropy={});
std::string native_data_protection_boot();
// parent 是共用 NativeGui 的 NSView；不將主機物件曝露給客體。
std::uint32_t native_data_protection_prompt(void* parent,std::u16string_view text,bool protect,bool password,std::u16string& result);
}
