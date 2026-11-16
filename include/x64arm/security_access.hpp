#pragma once
#include "x64arm/security_descriptor.hpp"
#include "x64arm/windows_tokens.hpp"

namespace x64arm::security {
using AccessMapping=std::array<std::uint32_t,4>;
inline constexpr AccessMapping process_access_mapping{0x20410U,0x20bebU,0x120000U,0x1fffffU};
inline constexpr AccessMapping thread_access_mapping{0x20048U,0x203b7U,0x120000U,0x1fffffU};
std::uint32_t mapped_access(std::uint32_t,const AccessMapping&);
std::uint32_t creation_access(std::uint32_t,const AccessMapping&);
bool token_sid_matches(const VirtualToken&,const Bytes&,bool deny,bool restricting=false);
std::uint32_t token_acl_access(const VirtualToken&,const SecurityDescriptor&,std::uint32_t,const AccessMapping&);
// AccessCheckByType 的 OBJECT_TYPE_LIST 節點；第一個為 level 0 的物件本身，子節點依序排在父節點之後。
struct ObjectTypeNode {
    std::uint16_t level{};
    Guid type{};
};
// self_sid 取代 ACE 中的 PRINCIPAL_SELF（S-1-5-10）；types 為空時只檢查物件本身。
std::uint32_t token_acl_access(const VirtualToken&,const SecurityDescriptor&,std::uint32_t,const AccessMapping&,
                              std::span<const ObjectTypeNode> types,std::span<const std::uint8_t> self_sid);
struct AccessCheckResult {
    std::uint32_t granted{},error{};
    std::vector<TokenPrivilege> privileges;
};
AccessCheckResult access_check(const VirtualToken&,const SecurityDescriptor&,std::uint32_t,const AccessMapping&,
                               std::span<const ObjectTypeNode> types = {},std::span<const std::uint8_t> self_sid = {});
std::uint32_t integrity_access(const VirtualToken&,const SecurityDescriptor&,std::uint32_t,const AccessMapping&);
// 私有物件安全（CreatePrivateObjectSecurity 家族）。flags 為 SEF_* 自動繼承旗標；token 可為空，
// 只在需要預設擁有者／群組／DACL、驗證擁有者或 SACL 權限時使用，缺少時回 ERROR_NO_TOKEN。
struct NewObjectRequest {
    const SecurityDescriptor* parent{};
    const SecurityDescriptor* creator{};
    std::span<const Guid> object_types;
    bool container{};
    std::uint32_t flags{};
    AccessMapping mapping{};
    const VirtualToken* token{};
};
SecurityDescriptor new_object_descriptor(const NewObjectRequest&);
SecurityDescriptor modify_object_descriptor(SecurityDescriptor current,const SecurityDescriptor& modification,std::uint32_t information,
                                            std::uint32_t flags,const AccessMapping&,const VirtualToken*);
SecurityDescriptor auto_inherit_descriptor(SecurityDescriptor current,const SecurityDescriptor* parent,std::span<const Guid> object_types,
                                           bool container,const AccessMapping&);
std::uint32_t duplicate_object_access(const VirtualToken&,const SecurityDescriptor&,std::uint32_t source,std::uint32_t desired,const AccessMapping&);
std::uint32_t object_access(const VirtualToken&,const SecurityDescriptor&,std::uint32_t,const AccessMapping&);
struct FileTokenContext {
    VirtualToken token;
    Bytes primary_user,primary_group;
    bool operator==(const FileTokenContext&) const = default;
};
std::uint32_t file_token_access(const FileTokenContext&,const SecurityDescriptor&,std::uint32_t);
std::uint32_t file_integrity_access(const FileTokenContext&,const SecurityDescriptor&,std::uint32_t);
Bytes integrity_label(std::uint32_t level,std::uint8_t flags=0,std::uint32_t policy=1);
}
