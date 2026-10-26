#pragma once
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>

namespace x64arm {
// 各家族共用額度契約，生命週期與索引仍由各家族維護。設定只能降低內建上限。
inline std::uint64_t retained_capacity_setting(const char* name,std::uint64_t maximum) noexcept {
    const auto* text=std::getenv(name);if(!text || !*text)return maximum;
    std::uint64_t value{};const auto end=text+std::strlen(text);const auto parsed=std::from_chars(text,end,value);
    return parsed.ec==std::errc{} && parsed.ptr==end?std::min(value,maximum):maximum;
}
inline bool reserve_retained_capacity(std::uint64_t bytes,std::uint64_t limit,
        std::atomic<std::uint64_t>& used,std::atomic<std::uint64_t>& peak,
        std::atomic<std::uint64_t>& rejected) noexcept {
    auto current=used.load(std::memory_order_relaxed);
    do{if(bytes>limit || current>limit-bytes){rejected.fetch_add(1,std::memory_order_relaxed);return false;}}
    while(!used.compare_exchange_weak(current,current+bytes,std::memory_order_relaxed));
    auto previous=peak.load(std::memory_order_relaxed);
    while(current+bytes>previous && !peak.compare_exchange_weak(previous,current+bytes,std::memory_order_relaxed)){}
    return true;
}
struct RetainedCapacity {
    const std::uint64_t limit;
    std::atomic<std::uint64_t> used{},peak{},rejected{};
    explicit RetainedCapacity(std::uint64_t maximum):limit(maximum){}
    bool reserve(std::uint64_t bytes) noexcept {return reserve_retained_capacity(bytes,limit,used,peak,rejected);}
};
// 額度由資料物件持有，移除索引不會提早歸帳；state 與最後持有者一起存活。
class RetainedCapacityLease {
    std::shared_ptr<RetainedCapacity> state_;
    std::uint64_t bytes_{};
    RetainedCapacityLease(std::shared_ptr<RetainedCapacity> state,std::uint64_t bytes)
        :state_(std::move(state)),bytes_(bytes){}
public:
    static std::optional<RetainedCapacityLease> acquire(const std::shared_ptr<RetainedCapacity>& state,std::uint64_t bytes){
        if(!state->reserve(bytes))return {};
        return RetainedCapacityLease(state,bytes);
    }
    RetainedCapacityLease(RetainedCapacityLease&& other) noexcept
        :state_(std::move(other.state_)),bytes_(std::exchange(other.bytes_,0)){}
    RetainedCapacityLease(const RetainedCapacityLease&)=delete;
    RetainedCapacityLease& operator=(const RetainedCapacityLease&)=delete;
    ~RetainedCapacityLease(){if(state_)state_->used.fetch_sub(bytes_,std::memory_order_relaxed);}
    std::uint64_t bytes() const noexcept {return bytes_;}
    bool shrink(std::uint64_t bytes) noexcept {
        if(bytes>bytes_)return false;
        if(!state_)return bytes==0;
        state_->used.fetch_sub(bytes_-bytes,std::memory_order_relaxed);bytes_=bytes;return true;
    }
};
}
