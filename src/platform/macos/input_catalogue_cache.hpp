#pragma once
#include "x64arm/windows_keyboard.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

namespace x64arm::detail {
// 通知只撤銷版本，不在通知回呼中列舉 TIS 或取得鎖。
// 一秒期限補足未泵送 distributed notification 的呼叫者及未啟用來源安裝變更。
class InputCatalogueCache {
public:
    using Clock=std::chrono::steady_clock;
    using Snapshot=std::shared_ptr<const std::vector<NativeInputMethod>>;
    void invalidate(){generation_.fetch_add(1,std::memory_order_release);}
    template<class Load> Snapshot get(Load&& load,Clock::time_point now){
        std::lock_guard lock(mutex_);
        const auto generation=generation_.load(std::memory_order_acquire);
        if(!snapshot_ || version_!=generation || now>=expires_){
            auto next=std::make_shared<const std::vector<NativeInputMethod>>(load());
            snapshot_=std::move(next);version_=generation;expires_=now+std::chrono::seconds(1);
        }
        return snapshot_;
    }
private:
    std::atomic<std::uint64_t> generation_{};
    std::mutex mutex_;
    Snapshot snapshot_;
    std::uint64_t version_{};
    Clock::time_point expires_{};
};
}
