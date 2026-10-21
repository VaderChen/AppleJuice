#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <system_error>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/syslimits.h>
#endif

namespace x64arm {
struct HostFileLimit {
    std::uint64_t inherited{}, current{}, maximum{};
    bool supported{};
};

// Finder／LaunchServices 可繼承僅 256 個 descriptor 的 soft limit。
// Windows handle、執行緒身分及共享物件都使用真實 descriptor；在啟動
// 客體與預載 worker 前統一取得宿主允許的容量，子程序也繼承相同設定。
inline HostFileLimit prepare_host_file_limit() {
#if !defined(_WIN32)
    rlimit limits{};
    if (::getrlimit(RLIMIT_NOFILE, &limits))
        throw std::system_error(errno, std::generic_category(), "無法取得宿主開啟檔案上限");
    HostFileLimit result{limits.rlim_cur, limits.rlim_cur, limits.rlim_max, true};
    auto target = limits.rlim_max;
#if defined(__APPLE__)
    // Darwin 不接受 RLIM_INFINITY 作為 soft limit；使用核心的實際上限，
    // 無法查詢時沿用 SDK 的有限 OPEN_MAX。只提高 soft limit，不改 hard limit。
    int maximum = OPEN_MAX;
    std::size_t size = sizeof(maximum);
    if (::sysctlbyname("kern.maxfilesperproc", &maximum, &size, nullptr, 0) ||
        size != sizeof(maximum) || maximum <= 0)
        maximum = OPEN_MAX;
    target = std::min(target, static_cast<rlim_t>(maximum));
#else
    if (target == RLIM_INFINITY) target = 65536;
#endif
    if (target > limits.rlim_cur) {
        limits.rlim_cur = target;
        if (::setrlimit(RLIMIT_NOFILE, &limits))
            throw std::system_error(errno, std::generic_category(), "無法調整宿主開啟檔案上限");
        result.current = target;
    }
    return result;
#else
    return {};
#endif
}
}
