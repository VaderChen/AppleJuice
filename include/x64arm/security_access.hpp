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
std::uint32_t integrity_access(const VirtualToken&,const SecurityDescriptor&,std::uint32_t,const AccessMapping&);
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
