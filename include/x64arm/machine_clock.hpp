#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace x64arm {
struct MachineTime {
    std::uint64_t filetime{},monotonic{},revision{};
    std::uint32_t increment{10000},adjustment{10000};bool disabled{true};
};
// 時間與速率屬於同一個 sandbox 開機命名空間；單調時鐘不受 SetSystemTime 影響。
class MachineClock {
    struct Storage;std::unique_ptr<Storage> storage_;
    explicit MachineClock(std::unique_ptr<Storage>);
public:
    ~MachineClock();
    static std::shared_ptr<MachineClock> open(const std::string& session,bool host,std::uint64_t monotonic,std::uint64_t filetime);
    std::uint64_t synchronize(std::uint64_t monotonic)const noexcept;
    MachineTime snapshot(std::uint64_t monotonic)const noexcept;
    void set_time(std::uint64_t monotonic,std::uint64_t filetime);
    void set_adjustment(std::uint64_t monotonic,std::uint32_t adjustment,bool disabled);
};
}
