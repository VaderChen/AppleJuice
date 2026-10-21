#pragma once
#include <vector>
#include "x64arm/windows_abi.hpp"
#include <array>
#include <string>

namespace x64arm {
// 客體聚合鍵盤的能力；Win32、Raw Input 與 DirectInput 共用同一個裝置描述。
struct GuiKeyboardCapabilities {std::uint32_t type,subtype,function_keys,dinput_subtype;};
inline constexpr GuiKeyboardCapabilities windows_keyboard_capabilities{4,0,12,4};
// 目前提供的 HKL 為 0409；掃描碼、文字與原生實體位置必須使用同一個配置。
struct GuiKeyboardTranslation {
    std::uint32_t alt_number{};bool alt_digits{},ansi{};
    bool operator==(const GuiKeyboardTranslation&) const=default;
};
struct NativeInputMethod {
    std::uint32_t layout{};bool current{};std::u16string identifier{},filename{},description{};
};
// 配置目錄共用不可變快照；目前來源即時查詢，只有檔名 API 才解析 bundle。
std::vector<NativeInputMethod> native_input_methods(bool include_filenames=false);
bool native_input_layout_supported(std::uint32_t);
std::span<const WindowsServiceDescriptor> input_service_descriptors();
std::uint32_t windows_map_virtual_key(std::uint32_t,std::uint32_t);
std::u16string windows_key_name(std::uint32_t);
std::u16string windows_key_text(std::uint32_t,std::uint32_t,const std::array<std::uint8_t,256>&,
    std::uint32_t,GuiKeyboardTranslation&);
// native=true 表示文字確實經 macOS 的 U.S. Unicode keyboard layout 轉換。
std::u16string native_us_key_text(std::uint32_t,bool,bool,bool* native=nullptr);
}
