#include "x64arm/windows_keyboard.hpp"
namespace x64arm {
std::vector<NativeInputMethod> native_input_methods(bool){return {};}
bool native_input_layout_supported(std::uint32_t){return false;}
std::u16string native_us_key_text(std::uint32_t scan,bool shift,bool caps,bool* native){
    if(native)*native=false;const auto key=windows_map_virtual_key(scan,1);
    if(key>='A' && key<='Z')return {static_cast<char16_t>(shift!=caps?key:key+32)};
    if(key>='0' && key<='9'){constexpr auto shifted=u")!@#$%^&*(";return {static_cast<char16_t>(shift?shifted[key-'0']:key)};}
    const auto normal=windows_map_virtual_key(key,2);if(!normal)return {};
    constexpr std::u16string_view plain=u";=,-./`[\\]'",upper=u":+<_>?~{|}\"";
    const auto at=plain.find(static_cast<char16_t>(normal));return {shift && at!=plain.npos?upper[at]:static_cast<char16_t>(normal)};
}
}
