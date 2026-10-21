#pragma once

namespace x64arm {
// 與客體的 64 位元 Windows 設定一致；Win32 目錄 API 與 Shell CSIDL 共用。
inline constexpr char16_t windows_directory[]=u"C:\\Windows";
inline constexpr char16_t windows_system_directory[]=u"C:\\Windows\\System32";
inline constexpr char16_t windows_wow64_directory[]=u"C:\\Windows\\SysWOW64";
}
