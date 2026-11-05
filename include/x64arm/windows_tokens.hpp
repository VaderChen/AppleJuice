#pragma once
#include <cstdint>
#include <array>
#include <vector>
#include <span>
#include <optional>
#include <memory>
#include <string>
#include "x64arm/native_authentication.hpp"

namespace x64arm {
struct TokenSid {
    std::vector<std::uint8_t> sid;
    std::uint32_t attributes{};
    bool operator==(const TokenSid&) const = default;
};
enum class ApplicationModel : std::uint32_t { Unspecified, ClassicDesktop };
struct TokenPrivilege {
    std::uint64_t luid{};
    std::uint32_t attributes{};
    bool operator==(const TokenPrivilege&) const = default;
};
struct TokenNetworkSecret {
    std::shared_ptr<authentication::Secret> secret;
    TokenNetworkSecret& operator=(std::shared_ptr<authentication::Secret> value){secret=std::move(value);return *this;}
    explicit operator bool()const noexcept{return bool(secret);}
    authentication::Secret* operator->()const noexcept{return secret.get();}
    bool operator==(const TokenNetworkSecret& other)const{return bool(secret)==bool(other.secret) && (!secret || secret->value==other.secret->value);}
};
// 客體權杖為獨立物件；DuplicateHandle 共用，DuplicateToken 複製。
// 物件 ID 不依賴宿主指標，使直譯、JIT 與狀態快照共用相同語義。
struct VirtualToken {
    TokenSid user;
    std::vector<TokenSid> groups, restricted;
    std::vector<TokenPrivilege> privileges;
    std::uint64_t modified_id{};
    std::vector<std::uint8_t> owner, primary_group, default_dacl;
    std::vector<std::uint8_t> object_descriptor;
    std::uint32_t type{1}, level{2}, integrity{0x2000}, session{1};
    ApplicationModel application_model{ApplicationModel::Unspecified};
    bool restriction_check{}, write_restricted{}, sandbox_inert{}, virtualization_enabled{}, child_process_restricted{};
    std::uint64_t authentication_id{1},source_id{1},expires{INT64_MAX};
    std::array<std::uint8_t,8> source{'X','6','4','A','R','M',' ',' '};
    // NewCredentials 只改變對外認證資料；本機 SID、群組及權限維持原 token。
    std::u16string network_user,network_domain;
    TokenNetworkSecret network_password;
    bool has_privilege(std::uint64_t luid,bool require_enabled=false) const noexcept {
        for(const auto& privilege:privileges)if(privilege.luid==luid)return !require_enabled || (privilege.attributes&2U);
        return false;
    }
    bool operator==(const VirtualToken&) const = default;
};
std::vector<std::uint8_t> encode_virtual_token(const VirtualToken&);
std::optional<VirtualToken> decode_virtual_token(std::span<const std::uint8_t>);
}
