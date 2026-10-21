#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
namespace x64arm {
struct DataProtectionError:std::runtime_error{
    std::uint32_t code;
    explicit DataProtectionError(std::uint32_t value):std::runtime_error("原生資料保護操作失敗"),code(value){}
};
// 持久金鑰只存在目前 sandbox 的 C: 與虛擬使用者／machine 範圍。
struct PrivateKeyStoreContext;
std::array<std::uint8_t,32> sandbox_data_protection_key(const PrivateKeyStoreContext&,std::string_view identity,bool machine,bool create);
std::string native_data_protection_boot();
// parent 是共用 NativeGui 的 NSView；不將主機物件曝露給客體。
std::uint32_t native_data_protection_prompt(void* parent,std::u16string_view text,bool protect,bool password,std::u16string& result);
}
