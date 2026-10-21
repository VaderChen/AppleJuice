#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace x64arm {
// Win32／Win64 的 PROPERTYKEY 均為 GUID 16 bytes + DWORD PID。
using WindowsPropertyKey = std::array<std::uint8_t,20>;
struct WindowsPropertyKeyDefinition {std::u16string_view name;WindowsPropertyKey key;};
struct WindowsPropertyKeyAlias {std::u16string_view name,canonical_name;};
std::span<const WindowsPropertyKeyDefinition> windows_property_key_catalog();
std::span<const WindowsPropertyKeyAlias> windows_property_key_aliases();
std::optional<WindowsPropertyKey> windows_property_key_from_name(std::u16string_view);
std::u16string_view windows_property_key_name(const WindowsPropertyKey&);
}
