#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm {
struct NativeServiceStatus {
    std::u16string label;
    bool system{};
    std::uint32_t pid{};
    std::int32_t exit_status{};
    std::u16string command{},account{};
    bool configured{},automatic{};
};
struct NativeServiceCatalog {
    std::vector<NativeServiceStatus> jobs;
    std::array<bool,2> domains{}; // user、system；不能把不可讀取的 domain 當空資料庫。
};
std::optional<NativeServiceCatalog> native_service_catalog();
std::optional<NativeServiceStatus> native_service_status(std::u16string_view,bool system);
std::array<std::uint32_t,9> service_status_words(const NativeServiceStatus&);
// 原生唯讀服務與客體 SCM 服務合併後的目錄項目；EnumServicesStatus、EnumDependentServices、GetServiceDisplayName／KeyName 共用。
struct ServiceCatalogEntry {std::u16string name,display,group;std::array<std::uint32_t,9> words{};std::vector<std::u16string> dependencies;};
}
