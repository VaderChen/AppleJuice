#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace x64arm {
using PeFileBytes = std::shared_ptr<const std::vector<std::uint8_t>>;
struct PeFileCacheStats {
    std::uint64_t hits{}, misses{}, waits{}, read_calls{}, read_bytes{}, evictions{};
    std::size_t retained_bytes{}, entries{};
};
// EXE／DLL 共用不可變來源快照。每次命中仍核對實際檔案身分與時間戳；
// 快取不代表存取授權，客體呼叫者須先完成 sandbox 路徑及權杖檢查。
class PeFileCache {
public:
    explicit PeFileCache(std::size_t capacity = 256U * 1024U * 1024U, std::size_t entries = 128);
    ~PeFileCache();
    PeFileCache(const PeFileCache&) = delete;
    PeFileCache& operator=(const PeFileCache&) = delete;
    PeFileBytes read(const std::filesystem::path&, std::size_t maximum);
#if !defined(_WIN32)
    // 借用已授權的可讀描述元；pread 不改變位置，不持有或關閉描述元。
    PeFileBytes read(int descriptor, std::size_t maximum);
#endif
    PeFileCacheStats stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
PeFileCache& pe_file_cache();
// PE overlay 不映射至客體；來源檔案上限與客體記憶體容量分離。
PeFileBytes read_pe_snapshot(const std::filesystem::path&, std::size_t maximum = std::size_t{2} * 1024 * 1024 * 1024);
}
