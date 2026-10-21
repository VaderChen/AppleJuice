#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace x64arm::startup_profile {
using Clock=std::chrono::steady_clock;
bool enabled();
std::uint64_t unix_us();
void mark(std::string_view name) noexcept;
void flush() noexcept;
struct Recorder;
// 獨立的低頻程序建立時間線；不啟用 Scope 計時，也不關閉 resident 快速路徑。
class ProcessLaunch {
    Recorder* output_{};Clock::time_point start_{};
    std::uint64_t id_{};std::uint32_t peer_{},status_{UINT32_MAX};
public:
    ProcessLaunch() noexcept;
    ~ProcessLaunch();
    void event(std::string_view name) noexcept;
    void peer(std::uint32_t pid) noexcept {peer_=pid;}
    void result(std::uint32_t status) noexcept {status_=status;}
    ProcessLaunch(const ProcessLaunch&)=delete;ProcessLaunch& operator=(const ProcessLaunch&)=delete;
};
class Scope {
    static thread_local Scope* current_;
    Scope* parent_{};Recorder* output_{};Clock::time_point start_{};
    Clock::duration children_{};std::uint64_t wall_{};std::string group_,name_;bool event_{};
public:
    Scope(std::string_view group,std::string_view name,bool event=false) noexcept;
    ~Scope();
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
}
