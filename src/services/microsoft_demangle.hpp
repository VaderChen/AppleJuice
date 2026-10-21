#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace x64arm {
inline constexpr std::size_t microsoft_name_limit = 4096;
// 無法識別的編碼保留原名稱；資源上限則回傳 nullopt，由 Windows 入口回報失敗。
std::optional<std::string> demangle_microsoft_name(std::string_view, std::uint16_t, bool type_only);
}
