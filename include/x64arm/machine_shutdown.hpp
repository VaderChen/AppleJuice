#pragma once
#include "x64arm/process_identity.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace x64arm {
enum class ShutdownPhase : std::uint64_t { Idle, Countdown, Committed, Aborted };
enum class ShutdownAction : std::uint32_t { Halt, PowerOff };
struct ShutdownRequest {
    ProcessIdentity origin;
    std::uint32_t seconds{},reason{};
    ShutdownAction action{ShutdownAction::PowerOff};
    std::u16string message;
};
struct ShutdownSnapshot {
    ShutdownPhase phase{};
    std::uint64_t generation{},deadline{};
    ShutdownRequest request;
};
// 同一 mapping session 共用的強制關機控制；時間使用單調軸，沒有宿主電源操作。
// 非強制結束協商、倒數視窗及重啟屬於上層能力，不能由這個控制器代為宣告完成。
class MachineShutdown {
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit MachineShutdown(std::unique_ptr<Storage>);
public:
    static constexpr std::uint32_t max_timeout=315360000;
    static constexpr std::size_t max_message=32767;
    ~MachineShutdown();
    static std::shared_ptr<MachineShutdown> open(const std::string& session,bool host_clock);
    std::uint32_t initiate(const ShutdownRequest&,std::uint64_t monotonic,bool override_grace=false,bool scheduled_error=false);
    std::uint32_t abort(std::uint64_t monotonic);
    bool poll(std::uint64_t monotonic);
    // acquire 讀取已發布狀態，不取得檔案鎖或採樣時鐘；不快取跨程序請求。
    ShutdownPhase phase()const noexcept;
    std::uint64_t deadline()const noexcept;
    ShutdownSnapshot snapshot()const;
};
}
