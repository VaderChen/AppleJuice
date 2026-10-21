#pragma once
#include <filesystem>
#include <iosfwd>

namespace x64arm {
// 純解析，不初始化 WindowsServices、不映射客體、不執行 DLL 初始化器。
void audit_pe(const std::filesystem::path& path, std::ostream& output);
}
