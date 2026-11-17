#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace x64arm {
struct WinTrustState {std::uint64_t provider{},signer{},certificate_array{};std::uint32_t certificate_count{};std::vector<std::uint64_t> allocations,certificates;bool operator==(const WinTrustState&)const=default;};
struct CatalogAdmin {std::string subsystem,algorithm{"SHA1"};bool operator==(const CatalogAdmin&)const=default;};
struct CatalogContext {std::uint64_t admin{};std::string path;bool operator==(const CatalogContext&)const=default;};
// CryptCATOpen 開啟的目錄（windows_wintrust_catalog.inc）：成員與屬性以宿主資料保存；客體端 CRYPTCATMEMBER／CRYPTCATATTRIBUTE 首次回傳時配置於程序堆積，
// 位址保留到 CryptCATClose（同一成員重複回傳相同指標），allocations 為全部配置。
struct CatalogAttributeEntry {std::u16string tag;std::uint32_t flags{};std::vector<std::uint8_t> value;std::uint64_t guest{};bool operator==(const CatalogAttributeEntry&)const=default;};
struct CatalogMemberEntry {std::u16string tag,file,subject;std::uint32_t version{};std::vector<std::uint8_t> indirect;std::vector<CatalogAttributeEntry> attributes;std::uint64_t guest{};
    bool operator==(const CatalogMemberEntry&)const=default;};
struct CatalogDocument {std::string path;std::uint32_t version{1};std::vector<std::uint8_t> identifier;std::vector<CatalogMemberEntry> members;std::vector<CatalogAttributeEntry> attributes;
    std::vector<std::uint64_t> allocations;bool operator==(const CatalogDocument&)const=default;};
}
