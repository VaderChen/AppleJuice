#pragma once
#include "x64arm/state.hpp"
#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace x64arm {
inline constexpr unsigned native_region_instruction_limit=64;
// 原生回傳 ABI：低 32 bits 是已完成量，bit 32 是可串接的側出口，
// 高 31 bits 是恢復狀態編號加一。零保留給舊線性區塊的 count-index 格式。
namespace native_result {
inline constexpr std::uint32_t completed(std::uint64_t value) noexcept { return static_cast<std::uint32_t>(value); }
inline constexpr bool side_exit(std::uint64_t value) noexcept { return (value&(UINT64_C(1)<<32))!=0; }
inline constexpr std::uint32_t recovery_token(std::uint64_t value) noexcept { return static_cast<std::uint32_t>(value>>33); }
inline constexpr std::uint64_t recovery(std::uint32_t count,std::uint32_t state) noexcept {
    return (static_cast<std::uint64_t>(state+1)<<33)|count;
}
}
// 配置、正常提交與故障恢復共用相同槽順序。X4／X5 僅供不消耗它們的
// 整數區間；冷記憶體 helper 依宿主 ABI 保存，不能當作 callee-saved。
inline constexpr std::array<unsigned,4> arm64_region_gprs{14,15,4,5};
// 只標記尚未提交目的狀態的存取／guard；不把任意 host PC 當成精確邊界。
bool arm64_guest_access(std::uint32_t word) noexcept;
bool arm64_recovery_guard(std::uint32_t word) noexcept;
// 故障／側出口依配置槽順序提交；目的只限相異的客體 GPR 槽。
std::size_t arm64_recovery_spills_end(std::span<const std::uint32_t> words,std::size_t at) noexcept;
// 故障出口可先由保存的 X6–X8 補齊旗標；僅接受完整共用模板。
std::size_t arm64_recovery_flags_end(std::span<const std::uint32_t> words,std::size_t at) noexcept;
struct Arm64RecoverySite {
    std::uint32_t word_offset{},state{};
    bool operator==(const Arm64RecoverySite&) const=default;
};
// 發布器與側表共同驗證同一回傳模板；不接受任意 MOVK 或跳過提交的出口。
std::optional<std::uint64_t> arm64_recovery_result(std::span<const std::uint32_t> words,std::size_t at,std::size_t end) noexcept;
// 可直接連結的條件出口樁：start 為提交配置槽後的第一字（MOVZ X1，物化 RIP 與旗標之前），
// branch 為樁尾「B 共用出口」。位置隨作用域轉換與發布布局重映射；發布器修補前仍逐字核對樣式。
struct Arm64LinkSite {
    std::uint32_t start{},branch{};
    std::uint64_t rip{};
    std::uint32_t completed{};
    bool operator==(const Arm64LinkSite&) const=default;
};
struct Arm64Recovery {
    // 新格式只收錄實際需要恢復的狀態；編號、完成量與客體 RIP 互相獨立。
    // 舊 SMAP 保留 count-index 語義，由 explicit_ids 明確區分。
    bool explicit_ids{};
    bool direct_memory{}; // 高速 MMU 只保留 CPU／控制流出口，沒有記憶體故障快照。
    std::uint32_t loop_step{},loop_limit{};
    std::vector<GuestAddress> rips;
    std::vector<std::uint32_t> exits;
    std::vector<std::uint32_t> completed;
    std::vector<Arm64RecoverySite> sites;
    std::vector<Arm64LinkSite> links;
    std::uint32_t count(std::size_t id) const noexcept { return explicit_ids?completed[id]:static_cast<std::uint32_t>(id); }
    std::uint32_t execution_limit(std::uint32_t linear) const noexcept { return loop_limit?loop_limit:linear; }
    void restore(GuestState& state,std::uint64_t encoded) const;
    const Arm64RecoverySite* find(std::size_t word) const noexcept;
    bool valid(std::span<const std::uint32_t> words) const noexcept;
};
struct LlvmFaultState;
struct JitRecoveryLocation {
    GuestAddress rip;
    std::uintptr_t bailout_pc{};
    std::uint32_t completed{};
    std::uint32_t state{};
    const LlvmFaultState* llvm{};
};
}
