#pragma once
#include "x64arm/registry.hpp"
#include <span>
#include <optional>
namespace x64arm::url_cache {
struct Lease {std::uint64_t id{},until{};std::uint32_t pid{};bool operator==(const Lease&)const=default;};
struct Entry {
    std::u16string url,file,redirect,extension;
    std::uint64_t size{},modified{},expires{},accessed{},synced{};
    std::uint32_t type{1},hits{},exemption{};
    bool header_wide{},deleting{};
    std::vector<std::uint8_t> headers;
    std::vector<Lease> leases;
    bool operator==(const Entry&)const=default;
};
std::uint64_t now();
std::u16string key(std::u16string_view);
inline const std::u16string index=std::u16string(1,u'\1')+u"\\software\\x64arm\\wininet-cache";
std::vector<std::uint8_t> encode(const Entry&);
Entry decode(std::span<const std::uint8_t>);
std::optional<Entry> get(const RegistryState&,std::u16string_view,bool redirects=false);
void put(RegistryState&,const Entry&);
void erase(RegistryState&,std::u16string_view);
std::vector<std::uint8_t> information(const Entry&,bool wide,bool x86,std::uint64_t base);
}
