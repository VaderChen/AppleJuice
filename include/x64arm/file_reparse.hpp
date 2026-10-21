#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace x64arm {
struct FileReparse {
    std::string target,physical;
    std::uint64_t device{},id{};
    std::uint32_t kind{},error{4390},attributes{}; // 1=file symlink、2=directory symlink、3=volume mount；4390=非 reparse point。
};
namespace file_reparse {
FileReparse read(int directory,std::string_view name);
std::uint32_t set_attributes(int directory,std::string_view name,std::uint32_t attributes);
std::uint32_t mark(int directory,std::string_view name,std::uint32_t kind,std::string_view target,std::string_view physical);
// 正規化 Windows 連結目標；relative:/ 是共用檔案層內部的相對掛載命名空間。
std::string target_path(std::string_view link,std::string_view target,std::string_view current={},bool strict_root=false);
}
}
