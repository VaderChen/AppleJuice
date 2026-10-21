#pragma once
#include <cstdint>

namespace x64arm {
// 引擎的 Windows 相容性身份；不是宿主 macOS 或 Microsoft DLL 的版本。
inline constexpr std::uint32_t windows_version_major=10;
inline constexpr std::uint32_t windows_version_minor=0;
inline constexpr std::uint32_t windows_version_build=19045;
inline constexpr std::uint32_t windows_version_revision=0;
}
