#pragma once
#include <sys/stat.h>
#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace x64arm {
struct FileVolume {
    char id{};
    std::filesystem::path host_path;
    std::uint64_t device{}, inode{};
};
struct FileVolumeMount {
    char source{}, target{};
    std::string path; // source volume 根目錄下的相對路徑，不保存宿主路徑。
    std::uint64_t device{}, inode{};
    bool operator==(const FileVolumeMount&) const = default;
};
struct FileVolumeState {
    std::map<char,char> drives;
    std::vector<FileVolumeMount> folders;
    std::uint32_t error{};
    bool operator==(const FileVolumeState&) const = default;
};
struct FileVolumeLocation {
    char volume{};
    std::string path, mount_path;
    std::uint32_t error{};
};
struct FileVolumePaths {
    std::vector<std::string> paths;
    std::uint32_t error{};
};
struct FileVolumeOpen {
    int descriptor{-1}; // 成功時由呼叫端關閉。
    std::uint32_t error{};
};
struct FileVolumeReparse {
    FileVolumeMount mount;
    std::uint32_t error{4390};
};
struct FileVolumeProperties {
    std::u16string label{u"X64ARM"};
    std::array<std::uint8_t,64> object_id{};
    std::uint32_t error{};
};

// volume identity 與 drive alias 分離；複本共享鎖與原生根目錄能力。
// 只有明示的根目錄可作為 target，持久中繼資料不會新增宿主存取權。
class FileVolumes {
public:
    explicit FileVolumes(const std::map<char,std::filesystem::path>& roots = {});
    std::vector<FileVolume> volumes() const;
    FileVolumeState snapshot() const;
    FileVolumeProperties properties(char volume) const;
    std::uint32_t set_label(char volume,std::u16string label,bool writable);
    std::uint32_t set_object_id(char volume,const std::array<std::uint8_t,64>& value,bool writable);
    FileVolumeOpen open_root(char volume,bool metadata_only=false) const;
    FileVolumeReparse reparse(int directory) const;
    // 以呼叫端剛取得的目錄 stat（不追蹤符號連結）查詢「已確認不是掛載點」快取；命中時不必開啟目錄再做 reparse。
    // 與 reparse 相同的鍵（裝置、inode、ctime）及命名空間版本；未命中回傳 false，呼叫端照原流程開啟檢查。
    bool plain_directory(const struct stat& entry) const;
    std::uint32_t set_drive(char drive,char volume,bool writable);
    std::uint32_t delete_drive(char drive,bool writable);
    std::uint32_t set_folder(char source,std::string path,char target,bool writable);
    std::uint32_t delete_folder(char source,std::string path,bool writable,const std::function<std::uint32_t()>& remove={});
    std::uint32_t relocate_folders(char source,std::string from,char target,std::string to,bool writable,const std::function<std::uint32_t()>& move,const std::function<std::uint32_t()>& undo);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
