#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace x64arm::process_io {
enum class Kind : unsigned { Read, Write, Other };
struct Counters {
    // Windows IO_COUNTERS：前三欄為操作次數，後三欄為實際傳輸 bytes。
    std::array<std::uint64_t,6> values{};
    bool operator==(const Counters&) const = default;
};
class Publisher {
public:
    Publisher(const std::string& session,std::uint32_t pid,std::uint64_t birth,const Counters&);
    ~Publisher();
    Publisher(const Publisher&)=delete;
    Publisher& operator=(const Publisher&)=delete;
    void publish(const Counters&) noexcept;
    std::uint32_t error() const noexcept;
    static std::uint32_t read(const std::string& session,std::uint32_t pid,std::uint64_t birth,Counters&);
private:
    struct State;
    std::unique_ptr<State> state_;
};
// 只在客體服務／I/O 完成輪詢期間啟用；建置、載入器與 IPC 自身不計入客體 I/O。
class Scope {
public:
    Scope(Counters&,Publisher* = nullptr) noexcept;
    ~Scope();
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
private:
    Counters* counters_; Counters* previous_; Publisher* publisher_;
    void* previous_batch_;
};
// 一個客體要求的分段搬移共用一次操作；每段的成功 bytes 仍立即累加。
class Batch {
public:
    explicit Batch(Kind) noexcept;
    ~Batch();
    Batch(const Batch&)=delete;
    Batch& operator=(const Batch&)=delete;
private:
    friend void record(Kind,std::uint64_t) noexcept;
    Kind kind_; Counters* counters_; Batch* previous_; bool recorded_{};
};
void record(Kind,std::uint64_t bytes=0) noexcept;
void record(const Counters&) noexcept;
void progress(Kind,std::uint64_t bytes,bool performed,bool& recorded,std::uint64_t& accounted) noexcept;
}
