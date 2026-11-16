#pragma once
#include "x64arm/registry.hpp"
#include "x64arm/http.hpp"
#include <array>
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
// 快取群組（含成員 URL）、自訂容器、標頭計數與設定，與項目索引分開保存，不改變項目編碼。
struct Group {std::uint64_t id{};std::uint32_t flags{},type{},quota{};std::array<std::uint32_t,4> owner{};std::u16string name;std::vector<std::u16string> urls;bool operator==(const Group&)const=default;};
struct Container {std::u16string name,prefix,path;std::uint32_t limit{},type{},options{};bool operator==(const Container&)const=default;};
struct Metadata {std::uint64_t next_group{1};std::vector<Group> groups;std::array<std::uint32_t,32> header{};std::vector<Container> containers;std::uint32_t quota{256000},sync_mode{4};bool operator==(const Metadata&)const=default;};
inline const std::u16string metadata_index=std::u16string(1,u'\1')+u"\\software\\x64arm\\wininet-cache-metadata";
Metadata load_metadata(const RegistryState&);
void store_metadata(RegistryState&,const Metadata&);
// WinINet Cookie 的持久狀態：同一機碼下三個值（持久 Cookie、各站台決定、各區域隱私範本），避免占用登錄值數量上限。
inline const std::u16string cookie_index=std::u16string(1,u'\1')+u"\\software\\x64arm\\wininet-cookies";
struct PrivacyPreference {std::uint32_t template_id{3};std::u16string preference;bool operator==(const PrivacyPreference&)const=default;};
std::vector<HttpCookie> load_cookies(const RegistryState&);
void store_cookies(RegistryState&,const std::vector<HttpCookie>&);
std::map<std::string,std::uint32_t> load_cookie_decisions(const RegistryState&);
void store_cookie_decisions(RegistryState&,const std::map<std::string,std::uint32_t>&);
// 以 zone<<1|type（0 第一方、1 第三方）為鍵。
std::map<std::uint32_t,PrivacyPreference> load_privacy(const RegistryState&);
void store_privacy(RegistryState&,const std::map<std::uint32_t,PrivacyPreference>&);
}
