#include "x64arm/process_identity.hpp"
#include <cerrno>
#include <climits>
#include <cstddef>
#include <map>
#include <mutex>
#include <utility>
#if defined(__APPLE__)
#include <fcntl.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <unistd.h>
#elif defined(__linux__)
#include <charconv>
#include <fstream>
#include <sstream>
#include <string>
#endif

namespace x64arm::process_identity {
namespace {
[[maybe_unused]] std::uint32_t query_error() {
    return errno == EPERM || errno == EACCES ? 5U : errno == ESRCH || errno == ENOENT ? 87U : 31U;
}
#if defined(__linux__)
Status stat_identity(std::uint32_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    if (!file)
        return {query_error(), {pid, 0}};
    std::string line;
    if (!std::getline(file, line))
        return {31, {pid, 0}};
    // comm 可以包含空白及右括號；欄位 3 從最後一個右括號之後開始。
    const auto end = line.rfind(')');
    if (end == std::string::npos)
        return {31, {pid, 0}};
    std::istringstream fields(line.substr(end + 1));
    std::string field;
    Status result{0, {pid, 0}};
    for (unsigned index = 3; index <= 22; ++index) {
        if (!(fields >> field))
            return {31, {pid, 0}};
        if (index == 3)
            result.exited = field == "Z" || field == "X";
        if (index == 22) {
            const auto parsed = std::from_chars(field.data(), field.data() + field.size(), result.identity.birth);
            if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() || !result.identity.birth)
                return {31, {pid, 0}};
        }
    }
    return result;
}
#endif
} // namespace

Status query(std::uint32_t pid) {
    if (!pid || pid > INT_MAX)
        return {87, {pid, 0}};
#if defined(__APPLE__)
    int mib[]{CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
    kinfo_proc info{};
    std::size_t size = sizeof(info);
    int status;
    do {
        size = sizeof(info);
        status = ::sysctl(mib, 4, &info, &size, nullptr, 0);
    } while (status && errno == EINTR);
    if (status)
        return {query_error(), {pid, 0}};
    if (!size)
        return {87, {pid, 0}};
    if (size != sizeof(info))
        return {31, {pid, 0}};
    const auto started = info.kp_proc.p_starttime;
    if (started.tv_sec < 0 || started.tv_usec < 0 || started.tv_usec >= 1000000)
        return {31, {pid, 0}};
    const auto birth =
        static_cast<std::uint64_t>(started.tv_sec) * 1000000 + static_cast<std::uint64_t>(started.tv_usec);
    if (!birth)
        return {31, {pid, 0}};
    const bool exited = info.kp_proc.p_stat == SZOMB;
    return {0, {pid, birth}, !exited && (info.kp_proc.p_flag & P_TRACED) != 0, exited};
#elif defined(__linux__)
    auto result = stat_identity(pid);
    if (result.error)
        return result;
    std::ifstream file("/proc/" + std::to_string(pid) + "/status");
    if (!file)
        return {query_error(), {pid, 0}};
    std::string key;
    while (file >> key) {
        if (key == "TracerPid:") {
            std::uint32_t tracer{};
            if (!(file >> tracer))
                return {31, {pid, 0}};
            // 兩個 proc 檔之間 PID 若被重用，不把新程序的 debugger 附在舊身分上。
            const auto checked = stat_identity(pid);
            if (checked.error)
                return checked;
            if (checked.identity != result.identity)
                return {87, {pid, 0}};
            result.exited = checked.exited;
            result.traced = !result.exited && tracer != 0;
            return result;
        }
        std::string ignored;
        std::getline(file, ignored);
    }
    return {31, {pid, 0}};
#else
    return {50, {pid, 0}};
#endif
}

struct Lifetime::Impl {
    ProcessIdentity expected;
    std::mutex mutex;
    bool initialized{}, ended{};
#if defined(__APPLE__)
    int descriptor{-1};
    void close_watch() {
        if (descriptor >= 0)
            ::close(descriptor);
        descriptor = -1;
    }
    void arm() {
        const auto before = query(expected.pid);
        ended = before.gone(expected);
        if (!before.live(expected))
            return;
        descriptor = ::kqueue();
        if (descriptor < 0)
            return;
        if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC)) {
            close_watch();
            return;
        }
        struct kevent change{}, receipt{};
        EV_SET(&change, expected.pid, EVFILT_PROC, EV_ADD | EV_ONESHOT | EV_RECEIPT, NOTE_EXIT, 0, nullptr);
        const timespec timeout{};
        int count;
        do {
            count = ::kevent(descriptor, &change, 1, &receipt, 1, &timeout);
        } while (count < 0 && errno == EINTR);
        if (count != 1 || receipt.ident != expected.pid || receipt.filter != EVFILT_PROC ||
            !(receipt.flags & EV_ERROR) || receipt.data) {
            close_watch();
            return;
        }
        // EV_RECEIPT 不消耗待取出的退出事件。註冊前後都核對出生身分，
        // 防止註冊競態監看到重用 PID 的新程序；暫時查詢失敗則回到即時查詢。
        const auto after = query(expected.pid);
        ended = after.gone(expected);
        if (!after.live(expected))
            close_watch();
    }
    ~Impl() { close_watch(); }
#endif
    explicit Impl(ProcessIdentity identity) : expected(identity) {}

    bool alive() {
        std::lock_guard lock(mutex);
        if (!expected || ended)
            return false;
        if (!initialized) {
            initialized = true;
#if defined(__APPLE__)
            arm();
#endif
        }
        if (ended)
            return false;
#if defined(__APPLE__)
        if (descriptor >= 0) {
            struct kevent event{};
            const timespec timeout{};
            int count;
            do {
                count = ::kevent(descriptor, nullptr, 0, &event, 1, &timeout);
            } while (count < 0 && errno == EINTR);
            if (!count)
                return true;
            if (count == 1 && event.ident == expected.pid && event.filter == EVFILT_PROC &&
                !(event.flags & EV_ERROR) && (event.fflags & NOTE_EXIT))
                ended = true;
            // 錯誤或非預期事件不能當成存活證據，也不能永久快取成退出。
            close_watch();
            if (ended)
                return false;
        }
#endif
        const auto status = query(expected.pid);
        ended = status.gone(expected);
        return status.live(expected);
    }
};

Lifetime::Lifetime(ProcessIdentity expected) : impl_(std::make_unique<Impl>(expected)) {}
Lifetime::~Lifetime() = default;
bool Lifetime::alive() { return impl_->alive(); }

std::shared_ptr<Lifetime> observe(ProcessIdentity expected) {
    if (!expected)
        return {};
    static std::mutex mutex;
    static std::map<std::pair<std::uint32_t, std::uint64_t>, std::weak_ptr<Lifetime>> observations;
    std::lock_guard lock(mutex);
    for (auto it = observations.begin(); it != observations.end();) {
        if (it->second.expired())
            it = observations.erase(it);
        else
            ++it;
    }
    auto &entry = observations[{expected.pid, expected.birth}];
    if (auto current = entry.lock())
        return current;
    auto current = std::make_shared<Lifetime>(expected);
    entry = current;
    return current;
}
} // namespace x64arm::process_identity
