#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace x64arm {
struct WinTrustState {std::uint64_t provider{},signer{},certificate_array{};std::uint32_t certificate_count{};std::vector<std::uint64_t> allocations,certificates;bool operator==(const WinTrustState&)const=default;};
struct CatalogAdmin {std::string subsystem,algorithm{"SHA1"};bool operator==(const CatalogAdmin&)const=default;};
struct CatalogContext {std::uint64_t admin{};std::string path;bool operator==(const CatalogContext&)const=default;};
}
