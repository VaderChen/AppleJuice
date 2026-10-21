#pragma once
#include <cstdint>
#include "x64arm/certificate_roots_path.hpp"
#include "x64arm/file_reparse.hpp"
#include "x64arm/file_volumes.hpp"
#include "x64arm/network_files.hpp"
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace x64arm {
struct VirtualToken;
namespace security { struct NativeFileSecurity;struct FileTokenContext; }
struct FileMetadata {
    std::uint64_t size{}, created{}, accessed{}, modified{}, changed{}, id{}, device{};
    std::uint32_t attributes{}, links{1};
    std::uint64_t allocation_size{};
    std::uint32_t reparse_tag{};
    std::uint64_t valid_data_size{};
    bool operator==(const FileMetadata&) const = default;
};
struct FileInformation {
    std::string key;
    FileMetadata metadata;
    std::uint32_t error{};
};
struct FileIoResult { std::uint64_t bytes{}; std::uint32_t error{}; };
struct HostFileState;
struct HostDeleteTarget;
struct HostFileRegistry;
class SharedFileState;class SharedFileLease;
struct FileVolumeSpace;
struct HostSecurityTarget {
    int directory{-1};std::string leaf;FileMetadata identity;
    ~HostSecurityTarget();
};
struct HostFile {
    explicit HostFile(int descriptor) : descriptor_(descriptor) {}
    explicit HostFile(std::shared_ptr<HostSecurityTarget> target) : security_target_(std::move(target)) {}
    ~HostFile();
    HostFile(const HostFile&) = delete;
    HostFile& operator=(const HostFile&) = delete;
    std::uint32_t validate_io(std::uint64_t offset,std::uint64_t length,bool writing) const;
    FileIoResult read(std::span<std::uint8_t> bytes, std::uint64_t offset) const;
    FileIoResult write(std::span<const std::uint8_t> bytes, std::uint64_t offset) const;
    std::uint32_t truncate(std::uint64_t size) const;
    std::uint32_t flush() const;
    std::uint32_t lock_range(std::uint64_t offset,std::uint64_t length,bool exclusive,bool unlock);
    std::uint32_t set_times(std::uint64_t created, std::uint64_t accessed, std::uint64_t modified) const;
    std::uint32_t set_basic(std::uint64_t created,std::uint64_t accessed,std::uint64_t modified,std::uint64_t changed,std::uint32_t attributes) const;
    std::uint32_t allocate(std::uint64_t size) const;
    std::uint32_t set_valid_data(std::uint64_t size) const;
    FileInformation information() const;
    // 借用描述元，只查詢即時資訊；不接管、不關閉，也不改變檔案位置。
    static FileInformation descriptor_information(int descriptor);
    FileVolumeSpace volume_space() const;
    int duplicate_descriptor() const;
    int transfer_descriptor() const noexcept { return descriptor_; }
    bool transferable() const;
    bool security_writable() const noexcept { return !readonly_; }
    const std::shared_ptr<HostSecurityTarget>& security_target() const noexcept{return security_target_;}
    bool delete_pending() const;
    std::uint64_t transfer_identity() const;
    std::shared_ptr<void> retain_file() const;
private:
    friend class ReadOnlyFiles;
    int descriptor_{-1};
    std::shared_ptr<HostSecurityTarget> security_target_;
    std::shared_ptr<HostFileState> state_;
    std::shared_ptr<HostDeleteTarget> delete_target_;
    std::uint32_t access_{}, sharing_{7};
    bool readonly_{},delete_on_close_{};
    char volume_{};
    struct RangeLock {std::uint64_t offset{},length{};bool exclusive{};};
    std::vector<RangeLock> locks_;
    std::shared_ptr<SharedFileLease> shared_lease_;
    std::shared_ptr<void> network_lease_;
    std::uint32_t lock_conflict(std::uint64_t offset,std::uint64_t length,bool writing) const;
    std::uint32_t mutate_size(const std::function<std::uint32_t()>&) const;
    std::uint32_t copy_template_metadata(int destination,bool directory,std::uint32_t attributes) const;
};
struct OpenHostFile {
    std::string key;
    std::shared_ptr<HostFile> file;
    FileMetadata metadata;
    std::uint32_t error{};
    bool existed{};
};
struct DirectoryEntry {
    std::string name;
    FileMetadata metadata;
    bool operator==(const DirectoryEntry&) const = default;
};
struct DirectorySnapshot {
    std::vector<DirectoryEntry> entries;
    std::uint32_t error{};
};
struct FileSnapshot {
    std::string key;
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    std::uint32_t error{};
    bool access_denied{}; // 區分實際 ACL／traversal 拒絕與未提供的客體搜尋根。
    std::filesystem::path source{}; // 已授權描述元對應的宿主來源，僅供載入器使用。
};
struct FileVolumeSpace {
    std::uint64_t total{}, free{}, available{};
    std::uint32_t allocation_unit{}, sector_size{}, error{};
};
class ReadOnlyFiles {
public:
    explicit ReadOnlyFiles(const std::filesystem::path& root = {}, const std::filesystem::path& c_drive = {}, bool writable = false, const std::filesystem::path& install_root = {}, const std::map<char,std::filesystem::path>& drives = {});
    FileSnapshot open(std::string path);
    FileSnapshot open_library(std::string path, const std::filesystem::path& application = {}, std::string_view application_guest_directory = {});
    OpenHostFile open_reader(std::string path, const std::filesystem::path& application = {}, std::string_view application_guest_directory = {}, std::uint32_t sharing = 7);
    std::string map_executable(const std::filesystem::path& path);
    // Windows DOS 名稱空間先折疊 .／..，之後仍由映射與 NOFOLLOW 限制宿主存取。
    void set_dos_path_normalization(bool enabled) noexcept { dos_path_normalization_=enabled; }
    void mount_readonly(std::string guest_directory,const std::filesystem::path& host_directory);
    std::pair<std::filesystem::path,std::uint32_t> executable_path(std::string path, const std::filesystem::path& application, std::string_view application_guest_directory);
    FileInformation information(std::string path);
    void set_shared_session(std::string);
    const std::string& shared_session()const noexcept{return shared_session_;}
    std::uint32_t publish_shared_file(HostFile&);
    FileInformation target_information(std::string path);
    FileVolumeSpace volume_space(std::string path);
    std::vector<FileVolume> volumes() const;
    FileVolumeState volume_state() const;
    const std::shared_ptr<NetworkFiles>& network_files()const noexcept{return network_files_;}
    const std::string& network_scope()const noexcept{return network_scope_;}
    bool network_path(std::string path)const;
    std::filesystem::path network_mount_directory(std::string name);
    FileVolumeLocation volume_location(std::string path,bool follow=false) const;
    FileVolumePaths volume_mount_paths(char volume,bool relative=false) const;
    std::uint32_t set_volume_mount(std::string path,char target);
    std::uint32_t delete_volume_mount(std::string path);
    OpenHostFile open_directory_observer(std::string path);
    DirectorySnapshot list(std::string path);
    // 只為 include(name) 為真的項目查詢中繼資料；結果等同完整列舉後依名稱過濾，部分結果不寫入快取。
    DirectorySnapshot list(std::string path,const std::function<bool(std::string_view)>& include);
    bool writable() const noexcept { return writable_; }
    void set_certificate_roots_file(std::string_view path) { certificate_roots_file_=certificate_roots_relative_path(path); }
    const std::string& certificate_roots_file() const noexcept { return certificate_roots_file_; }
    void set_security_context(const security::FileTokenContext&);
    void set_security_context(const VirtualToken&,const std::vector<std::uint8_t>& primary_user,const std::vector<std::uint8_t>& primary_group);
    std::uint32_t set_current_directory(std::string path);
    OpenHostFile open_host(std::string path, std::uint32_t access, std::uint32_t disposition, std::uint32_t flags, std::uint32_t sharing = 7,
                          const std::function<std::vector<std::uint8_t>()>& creation = {},const std::function<OpenHostFile()>& file_template = {});
    std::uint32_t create_directory(std::string path,const std::function<std::vector<std::uint8_t>()>& creation = {},std::string template_path = {});
    std::uint32_t set_attributes(std::string path, std::uint32_t attributes);
    std::uint32_t remove(std::string path, bool directory);
    std::uint32_t copy(std::string from, std::string to, bool fail_if_exists);
    std::uint32_t hard_link(std::string existing, std::string target);
    std::uint32_t symbolic_link(std::string link,std::string target,bool directory);
    FileReparse reparse(std::string path) const;
    std::uint32_t replace(std::string original,std::string replacement,std::string backup,std::uint32_t flags);
    std::uint32_t rename(std::string from, std::string to, bool replace, bool copy_allowed = false, bool write_through = false,const HostFile* owner=nullptr);
    FileInformation final_path(const HostFile&,std::string fallback) const;
    OpenHostFile pin(std::string path) const;
    // image_metadata 僅供安全描述子操作；明示可寫掛載中的映像不提供內容描述元。
    OpenHostFile pin_security(std::string path,bool image_metadata=false) const;
    // 安全描述子繼承使用：呼叫端已驗證 WRITE_DAC；保留宿主權限與映射來源限制。
    DirectorySnapshot list_security(std::string path) const;
    security::NativeFileSecurity security_information(std::string path, bool with_acl) const;
    std::uint32_t security_access(std::string path,std::uint32_t desired) const;
    OpenHostFile inherit_host(int descriptor,std::uint32_t access,std::uint32_t sharing,bool readonly,std::uint64_t object=0);
    OpenHostFile reopen(const HostFile&,std::string path,std::uint32_t access,std::uint32_t sharing,std::uint32_t flags);
    std::uint32_t set_disposition(HostFile&,std::string path,bool deleting);
private:
    struct Cache {
        std::map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> files;
        std::map<std::string, FileMetadata> metadata;
        std::map<std::string, DirectorySnapshot> directories;
        std::size_t bytes{};
        std::optional<FileVolumeState> volumes;
    };
    struct Resolved { std::string key; int descriptor{-1}; std::uint32_t error{}; bool readonly_file{}; std::filesystem::path host_path{}; std::string security_leaf{}; bool access_denied{}; char volume{}; std::string volume_path{},mount_path{}; std::shared_ptr<void> network_lease{}; };
    Resolved resolve(std::string path, bool directory = false, std::string* leaf = nullptr, bool metadata_only = false,bool follow_leaf = false,unsigned depth = 0,bool strict_root = false) const;
    Resolved resolve_mapped(std::string path, bool directory, std::string* leaf, bool metadata_only = false,unsigned depth = 0,bool follow_leaf = false,bool strict_root = false) const;
    // 已完成 metadata 解析後的屬性查詢；接管 resolved.descriptor。
    FileInformation information_resolved(Resolved resolved);
    std::uint32_t symbolic_link_reparse(std::string target,std::string link,bool directory);
    std::pair<std::string,std::uint32_t> follow_reparse(std::string path,bool strict_root=false) const;
    std::pair<std::filesystem::path,std::uint32_t> link_host_path(std::string path) const;
    bool readonly_descriptor(int descriptor) const;
    bool readonly_identity(std::uint64_t device,std::uint64_t inode) const;
    bool writable_image_source(std::string path,const std::filesystem::path& host) const;
    std::shared_ptr<HostFileState> file_state(const FileMetadata& metadata, bool create = false);
    std::uint32_t attach_shared_file(HostFile&,std::uint64_t object=0,bool existing=false);
    std::string shared_session_;
    std::uint32_t sharing_error(const FileMetadata& metadata, std::uint32_t access, std::uint32_t sharing,const HostFile* owner=nullptr);
    std::uint32_t authorize(int,std::uint32_t) const;
    std::uint32_t authorize_at(int,std::string_view,std::uint32_t) const;
    std::uint32_t authorize_delete(int,std::string_view) const;
    std::uint32_t inherit_file_label(int,int,bool directory) const;
    std::vector<std::uint8_t> creation_security(int,bool,std::span<const std::uint8_t>) const;
    std::uint32_t initialize_file_security(int,std::span<const std::uint8_t>) const;
    std::uint32_t initialize_file_security(const HostFile&,std::span<const std::uint8_t>) const;
    std::shared_ptr<const security::FileTokenContext> security_context_;
    std::shared_ptr<HostFileRegistry> host_files_;
    std::shared_ptr<FileVolumes> volumes_;
    std::shared_ptr<NetworkFiles> network_files_{std::make_shared<NetworkFiles>()};
    std::string network_scope_{"default"};
    char relative_volume_{};
    std::filesystem::path root_, c_drive_, install_root_;
    std::map<char,std::filesystem::path> drives_;
    std::string current_directory_;
    std::string certificate_roots_file_; // 空字串為預設關閉，不讀寫 ROOT.pem。
    std::map<std::string,std::filesystem::path> readonly_files_;
    std::map<std::string,std::filesystem::path> readonly_mounts_;
    std::map<std::string,std::filesystem::path> application_sources_;
    bool writable_{};
    bool dos_path_normalization_{};
    // 比對的兩份服務共享不可變輸入快照，file cursor 另由各服務保存。
    mutable std::shared_ptr<Cache> cache_{std::make_shared<Cache>()};
};
}
