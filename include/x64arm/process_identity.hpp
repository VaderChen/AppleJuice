#pragma once
#include <cstdint>
#include <memory>

namespace x64arm {
// PID 只識別當前 slot；跨程序 handle 必須同時綁定不可變的出生時間。
struct ProcessIdentity {
    std::uint32_t pid{};
    std::uint64_t birth{};
    explicit operator bool() const noexcept { return pid && birth; }
    bool operator==(const ProcessIdentity &) const = default;
};

namespace process_identity {
struct Status {
    std::uint32_t error{};
    ProcessIdentity identity;
    bool traced{}, exited{};
    bool live(ProcessIdentity expected) const noexcept { return !error && !exited && expected && identity == expected; }
    // 已驗證的身分：不存在、殭屍程序或出生識別改變才算失效；權限／暫時查詢錯誤不算。
    bool gone(ProcessIdentity expected) const noexcept {
        return expected && (error==87 || exited || (!error && identity && identity!=expected));
    }
};
// 程序 handle、偵錯通道及共享資源共用相同平台來源及出生識別格式。
Status query(std::uint32_t pid);

// 長期持有的 handle 只觀察同一個出生身分的生命期，不快取 debugger／權限資訊。
// 有退出事件的平台每次即時取出通知；不支援或監看失敗時仍使用 query。
class Lifetime {
  public:
    explicit Lifetime(ProcessIdentity expected);
    ~Lifetime();
    bool alive();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// 相同 PID＋出生時間的程序／執行緒 handle 共用監看，最後一個持有者關閉時釋放。
std::shared_ptr<Lifetime> observe(ProcessIdentity expected);
} // namespace process_identity
} // namespace x64arm
