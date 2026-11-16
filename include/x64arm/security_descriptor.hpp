#pragma once
#include "x64arm/memory.hpp"
#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace x64arm {
struct SecurityDescriptorError { std::uint32_t code; };
namespace security {
using Bytes = std::vector<std::uint8_t>;
using NameResolver = std::function<Bytes(std::string_view)>;

struct SecurityAcl {
    Bytes bytes;
    std::vector<std::size_t> offsets;
    std::size_t used{8};
};
struct SecurityDescriptor {
    std::uint16_t control{};
    std::uint8_t rm_control{};
    std::array<std::uint64_t,4> parts{}; // owner、group、SACL、DACL
    std::array<Bytes,4> data;
};
// 物件 ACE（ACCESS_ALLOWED／DENIED／SYSTEM_AUDIT／ALARM_OBJECT_ACE，類型 5～8）的 GUID 欄位；
// flags 的 bit 0 為 ObjectType、bit 1 為 InheritedObjectType 是否存在。
using Guid = std::array<std::uint8_t,16>;
struct AceObjects {
    std::uint32_t flags{};
    Guid type{}, inherited{};
    bool operator==(const AceObjects&) const = default;
};
// 一般 ACE（0～3、0x11、0x13、0x14）與物件 ACE 共用；objects 有值即物件 ACE 版面。
struct ExplicitAce {
    std::uint8_t type{}, flags{};
    std::uint32_t mask{};
    Bytes sid;
    std::optional<AceObjects> objects{};
};
using AceRecord = ExplicitAce;
// 借用已驗證 ACE 的 SID；有效期間由呼叫端的 ACL 儲存空間決定。
struct ExplicitAceView {
    std::uint8_t type{}, flags{};
    std::uint32_t mask{};
    std::span<const std::uint8_t> sid;
};
struct ExplicitEntry {
    std::uint32_t mode{};
    ExplicitAce ace;
};
constexpr bool object_ace_type(unsigned type) { return type>=5 && type<=8; }
// 繼承選項：mapping 不為空時對有效 ACE 套用 generic 對應；object_types 是新物件的型別 GUID；
// auto_inherit 決定是否標記 INHERITED_ACE（Windows 2000 自動繼承）。
struct InheritOptions {
    const std::array<std::uint32_t,4>* mapping{};
    std::span<const Guid> object_types;
    bool container{}, auto_inherit{true};
};
struct TrusteeLayout {
    unsigned unit, size, name_offset, entry_size, trustee_offset;
};
constexpr TrusteeLayout trustee_layout(bool x86) {
    return x86 ? TrusteeLayout{4,20,16,32,12} : TrusteeLayout{8,32,24,48,16};
}
struct KnownAccount {
    std::string_view name, domain;
    std::uint8_t authority, count;
    std::array<std::uint32_t,2> subauthorities;
    std::uint32_t type;
};

Bytes read_sid(const GuestMemory&, std::uint64_t);
SecurityAcl parse_acl(std::span<const std::uint8_t>);
SecurityAcl read_acl(const GuestMemory&, std::uint64_t);
// GENERIC_MAPPING（Read、Write、Execute、All）。
std::array<std::uint32_t,4> read_mapping(const GuestMemory&, std::uint64_t);
SecurityDescriptor read_descriptor(const GuestMemory&, std::uint64_t, bool x86, bool contents);
Bytes relative_descriptor(const SecurityDescriptor&);
SecurityDescriptor parse_relative_descriptor(std::span<const std::uint8_t>);
// 依 SECURITY_INFORMATION（OWNER／GROUP／DACL／SACL／LABEL）取出描述元的部分與相關控制位元。
SecurityDescriptor select_descriptor(const SecurityDescriptor&, std::uint32_t information);
SecurityDescriptor inherit_descriptor(SecurityDescriptor child,const SecurityDescriptor& parent,
                                      const Bytes& owner,const Bytes& group,bool container);
ExplicitAce basic_ace(std::span<const std::uint8_t>);
ExplicitAceView basic_ace_view(std::span<const std::uint8_t>);
Bytes encode_ace(const ExplicitAce&);
Bytes encode_acl(const std::vector<Bytes>&, unsigned revision = 2);
AceRecord read_ace(std::span<const std::uint8_t>);
// ACL 的各 ACE 原始位元組（已驗證長度）；空 span 視為沒有 ACE。
std::vector<Bytes> acl_entries(std::span<const std::uint8_t>);
// 含物件 ACE 時 ACL 版本須為 ACL_REVISION_DS（4）。
unsigned acl_revision(const std::vector<Bytes>&, unsigned minimum = 2);
// 父層 ACL 對新物件可繼承的 ACE；CREATOR OWNER／GROUP 以新物件擁有者／群組取代，必要時拆成有效與僅繼承兩筆。
std::vector<Bytes> inherited_aces(std::span<const std::uint8_t> parent, const Bytes& owner, const Bytes& group,
                                  const InheritOptions&);
// TRUSTEE_IS_SID／NAME／OBJECTS_AND_SID／OBJECTS_AND_NAME；objects 不為空時回傳物件 GUID（OBJECTS_AND_* 形式）。
Bytes read_trustee(const GuestMemory&, std::uint64_t, bool x86, bool wide, const NameResolver&,
                   std::optional<AceObjects>* objects = nullptr);
std::vector<ExplicitEntry> read_explicit_entries(const GuestMemory&, std::uint64_t, std::uint32_t,
                                                bool x86, bool wide, const NameResolver&);
Bytes merge_acl(std::span<const std::uint8_t>, const std::vector<ExplicitEntry>&);
bool same_name(std::string_view, std::string_view);
const KnownAccount* find_account(std::string_view);
const KnownAccount* find_account(std::span<const std::uint8_t>);
Bytes account_sid(const KnownAccount&);
struct AccountIdentity {
    Bytes sid;
    std::string_view name, domain;
    std::uint32_t type;
};
std::optional<AccountIdentity> resolve_account(std::string_view, std::span<const std::uint8_t> primary_sid);
std::optional<AccountIdentity> resolve_account(std::span<const std::uint8_t>, std::span<const std::uint8_t> primary_sid);
Bytes unix_sid(bool group, std::uint32_t id);
}
}
