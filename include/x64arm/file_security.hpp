#pragma once
#include "x64arm/security_descriptor.hpp"
#include <array>
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

// 索引仍持有的物件與 vector capacity；不含呼叫端副本、淘汰後仍在複製中的參照、
// allocator／map／deque 開銷，也不代表程序 RSS。固定大小統計不保存逐筆存取歷史。
struct NativeFileSecurityCacheStats {
    std::uint64_t entries{}, entry_limit{}, object_bytes{}, dynamic_bytes{}, peak_indexed_bytes{};
    std::uint64_t hits{}, misses{}, insertions{}, duplicate_insertions{}, evictions{}, invalidations{};
    std::uint64_t acl_entries{}, largest_dynamic_bytes{};
    // 物件、動態 capacity 與控制區額度；含已移出索引但仍被複製的物件，另有 4096 筆索引上限。
    // 不含 malloc rounding、map／deque 與呼叫端回傳副本，不是整程序記憶體上限。
    std::uint64_t byte_limit{},reserved_bytes{},peak_reserved_bytes{},indexed_charged_bytes{};
    std::uint64_t budget_rejections{},allocation_rejections{};
    // 每筆動態容量：0、1～256、257～4096、4097～65536、65537 以上。
    std::array<std::uint64_t,5> size_buckets{};
};
NativeFileSecurityCacheStats native_file_security_cache_stats();

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
