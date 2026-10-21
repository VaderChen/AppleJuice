#pragma once
#include <cstdint>
#include <limits>

namespace x64arm {
// 呼叫端須持有共用儲存鎖，並提供同一單調時鐘的奈秒數。
// 純狀態轉移不等待、不執行 I/O；持有者代次避免逾時後的延遲釋放誤傷新持有者。
struct PolicyLease {
    static constexpr std::uint64_t duration_ns=600'000'000'000ULL;
    std::uint64_t generation{},process{},thread{},deadline{};
    std::uint32_t depth{};
    struct Acquisition {std::uint64_t generation{};std::uint32_t error{};};
    bool expired(std::uint64_t now)const noexcept{return depth && now>=deadline;}
    void retire()noexcept{process=thread=deadline=0;depth=0;}
    Acquisition acquire(std::uint64_t owner_process,std::uint64_t owner_thread,std::uint64_t now)noexcept{
        if(!owner_process || !owner_thread)return {0,87};
        if(expired(now))retire();
        if(depth){
            if(process!=owner_process || thread!=owner_thread)return {0,170};
            if(depth==std::numeric_limits<std::uint32_t>::max())return {0,8};
            ++depth;return {generation,0}; // 遞迴進入不延長最初的十分鐘期限。
        }
        if(generation==std::numeric_limits<std::uint64_t>::max() || now>std::numeric_limits<std::uint64_t>::max()-duration_ns)return {0,8};
        ++generation;process=owner_process;thread=owner_thread;deadline=now+duration_ns;depth=1;
        return {generation,0};
    }
    std::uint32_t release(std::uint64_t owner_process,std::uint64_t owner_thread,std::uint64_t ticket,std::uint64_t now)noexcept{
        if(expired(now))retire();
        if(!depth || ticket!=generation || process!=owner_process || thread!=owner_thread)return 288;
        if(!--depth)retire();return 0;
    }
    bool abandon(std::uint64_t owner_process,std::uint64_t ticket)noexcept{
        if(!depth || process!=owner_process || generation!=ticket)return false;
        retire();return true;
    }
};
}
