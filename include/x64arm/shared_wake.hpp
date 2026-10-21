#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace x64arm {
// 喚醒只要求排程器重新檢查物件；不代替事件、管道或逾時的實際判定。
class SharedWake {
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit SharedWake(std::unique_ptr<Storage>);
public:
    ~SharedWake();
    static std::shared_ptr<SharedWake> open(const std::string& session);
    std::uint64_t snapshot() const noexcept;
    void notify() noexcept;
    // expected 必須在輪詢所有等待條件之前取得，避免檢查與休眠之間漏掉通知。
    // 不支援的宿主回傳 false，呼叫端沿用原有有界輪詢。
    bool wait_for(std::uint64_t expected,std::uint64_t nanoseconds) noexcept;
};
}
