#pragma once
#include "x64arm/security_descriptor.hpp"
#include <optional>

namespace x64arm {
struct HostFile;
namespace security {
// 單次檔案／安全操作內重用成功的宿主身分轉換；最外層退出即清除，不快取授權結果。
class NativeFileIdentityScope {
public:
    NativeFileIdentityScope() noexcept;
    ~NativeFileIdentityScope();
    NativeFileIdentityScope(const NativeFileIdentityScope&) = delete;
    NativeFileIdentityScope& operator=(const NativeFileIdentityScope&) = delete;
};
struct NativeFileSecurity {
    std::uint32_t uid{}, gid{}, mode{};
    bool directory{}, protected_acl{}, deferred_inherit{};
    std::vector<ExplicitAce> extended;
    std::optional<Bytes> mapped_dacl; // 已核對原生 ACE 的 Windows DACL 投影；包含 NULL DACL。
    Bytes mandatory_label;
    std::optional<Bytes> mapped_descriptor; // Windows 虛擬 owner／group／ACL，綁定同一份原生 ACL。
};

// 同一個已固定 inode 的描述符提供 UID／GID、mode 與原生 ACL。
NativeFileSecurity read_native_file_security(const HostFile&, bool with_acl);
// 清除依 (裝置, inode, ctime) 保存的原生安全性快取；一般情況由 ctime 自動失效，測試或身分資料庫變更時可手動呼叫。
void invalidate_native_file_security_cache();
NativeFileSecurity read_native_file_security_at(int,std::string_view,bool with_acl);
void write_native_file_label(const HostFile&,const Bytes&);
// user／group 是虛擬 token 的宿主身分別名；未知 SID 仍交由原生身分服務解析。
void write_native_file_security(const HostFile&, const SecurityDescriptor&, std::uint32_t information,
                                const Bytes& user = {}, const Bytes& group = {});
void write_virtual_file_security(const HostFile&,const SecurityDescriptor&,std::uint32_t information);
bool virtual_file_owner(const SecurityDescriptor&);
SecurityDescriptor file_descriptor(const NativeFileSecurity&, std::uint32_t information);
std::optional<Bytes> native_account_sid(std::string_view);
}
}
