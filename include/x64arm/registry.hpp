#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace x64arm {
enum class RegistryView {Native64,Wow32};
RegistryView registry_view(std::uint32_t flags,bool x86);
std::u16string registry_view_path(std::u16string path,RegistryView view);
void initialize_registry_views(struct RegistryState&);
struct RegistryValue {
    std::uint32_t type{};
    std::vector<std::uint8_t> bytes;
    std::u16string display_name{};
    bool operator==(const RegistryValue&) const = default;
};
struct RegistryHandle {
    std::u16string path;
    std::uint32_t access{};
    RegistryView view{RegistryView::Native64};
    std::u16string logical_path{};
    bool operator==(const RegistryHandle&) const = default;
};
inline const std::u16string& registry_logical_path(const RegistryHandle& handle){return handle.logical_path.empty()?handle.path:handle.logical_path;}
struct RegistryHiveMount {
    std::u16string source;
    std::uint64_t device{},id{};
    std::uint32_t minor{3};
    bool operator==(const RegistryHiveMount&) const = default;
};
struct RegistryState {
    std::map<std::u16string,std::map<std::u16string,RegistryValue>> keys;
    std::map<std::uint64_t,RegistryHandle> handles;
    std::map<std::u16string,std::u16string> display_names;
    std::map<std::u16string,std::uint64_t> modified_times;
    std::map<std::u16string,std::vector<std::uint8_t>> security_descriptors;
    std::map<std::u16string,std::u16string> classes;
    std::set<std::u16string> volatile_keys;
    // 由硬體 provider 產生的程序內查詢資料，不寫入持久 registry。
    std::set<std::u16string> generated_keys;
    std::map<std::u16string,RegistryHiveMount> hives;
    std::uint64_t next_handle{0xe0000}, bytes{}, values{};
    bool operator==(const RegistryState&) const = default;
};
std::vector<std::u16string> registry_subkeys(const RegistryState&,const RegistryHandle&);
}
