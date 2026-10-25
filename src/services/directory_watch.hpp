#pragma once
#include "x64arm/readonly_files.hpp"
#include <optional>

namespace x64arm {
// 監看已通過虛擬檔案系統路徑檢查的 fd；不接受宿主絕對路徑。
class DirectoryWatch {
public:
    static std::shared_ptr<DirectoryWatch> create(const HostFile&, bool subtree,
        std::uint32_t filter, std::uint32_t capacity, std::uint32_t& error, unsigned information_class = 1);
    virtual ~DirectoryWatch() = default;
    virtual void poll() = 0;
    virtual std::optional<FileIoResult> take(std::span<std::uint8_t>) = 0;
    virtual std::size_t pending_bytes() const = 0;
    virtual bool matches(bool subtree, std::uint32_t filter, unsigned information_class = 1) const = 0;
};
}
