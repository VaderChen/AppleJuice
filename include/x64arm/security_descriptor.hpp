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
struct ExplicitAce {
    std::uint8_t type{}, flags{};
    std::uint32_t mask{};
    Bytes sid;
};
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
SecurityDescriptor read_descriptor(const GuestMemory&, std::uint64_t, bool x86, bool contents);
Bytes relative_descriptor(const SecurityDescriptor&);
SecurityDescriptor parse_relative_descriptor(std::span<const std::uint8_t>);
SecurityDescriptor inherit_descriptor(SecurityDescriptor child,const SecurityDescriptor& parent,
                                      const Bytes& owner,const Bytes& group,bool container);
ExplicitAce basic_ace(std::span<const std::uint8_t>);
ExplicitAceView basic_ace_view(std::span<const std::uint8_t>);
Bytes encode_ace(const ExplicitAce&);
Bytes encode_acl(const std::vector<Bytes>&, unsigned revision = 2);
Bytes read_trustee(const GuestMemory&, std::uint64_t, bool x86, bool wide, const NameResolver&);
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
