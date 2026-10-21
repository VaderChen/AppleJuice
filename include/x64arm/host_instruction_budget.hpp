#pragma once
#include <algorithm>
#include <cstdint>

namespace x64arm {
// 宿主時鐘模式以既有退休時間戳估計下次批次大小。API、yield、故障及
// 原有時間片到期檢查照常返回協調者；不在產生的客體碼插入計時指令。
struct HostInstructionBudget {
    std::uint64_t limit{32768}, sampled_at{};
    void observe(std::uint64_t count,std::uint64_t now) noexcept {
        const auto previous=sampled_at;sampled_at=now;
        // 短入口／API 間隔不代表滿批運算速度；時鐘重設也不作速度估計。
        if(!previous || now<previous || count<limit/2)return;
        // 一次回呼可能已包含多次原生續發；以實際退休數換算目前額度的成本，
        // 不能把整個時間片誤當成單批成本，令快速迴圈的觀察額度停在下限。
        const auto elapsed=static_cast<unsigned __int128>(now-previous)*limit;
        const auto retired=static_cast<unsigned __int128>(count);
        if(elapsed<retired*500)limit=std::min<std::uint64_t>(limit*2,524288);
        else if(elapsed>retired*2500)limit=std::max<std::uint64_t>(limit/2,4096);
    }
    bool operator==(const HostInstructionBudget&) const = default;
};
}
