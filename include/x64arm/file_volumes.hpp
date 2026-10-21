#pragma once
#include <cstdint>
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

// volume identity 與 drive alias 分離；複本共享鎖與原生根目錄能力。
// 只有明示的根目錄可作為 target，持久中繼資料不會新增宿主存取權。
class FileVolumes {
public:
    explicit FileVolumes(const std::map<char,std::filesystem::path>& roots = {});
    std::vector<FileVolume> volumes() const;
    FileVolumeState snapshot() const;
    FileVolumeOpen open_root(char volume,bool metadata_only=false) const;
    FileVolumeReparse reparse(int directory) const;
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
