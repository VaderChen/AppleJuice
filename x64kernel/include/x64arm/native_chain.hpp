#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "x64arm/native_dispatch_config.h"
#include "x64arm/address_cache.hpp"

namespace x64arm {
// 沒有服務回呼時，原生鏈與 LLVM 共用有界退休額度；仍受呼叫者剩餘預算限制。
// 有服務時保留服務提供的鏈長與時間片，不用固定小額度切碎它。
inline constexpr std::uint64_t native_dispatch_budget=65536;
// 分層接納仍按批次取樣；待接納入口保留原額度，避免放大批次延後熱區編譯。
inline constexpr std::uint64_t native_tier_sample_budget=4096;
static_assert(native_tier_sample_budget<=native_dispatch_budget);
struct GuestState;
class GuestMemory;
inline constexpr unsigned native_dispatch_bits=X64ARM_NATIVE_DISPATCH_BITS;
inline constexpr std::size_t native_dispatch_capacity=std::size_t{1}<<native_dispatch_bits;
// 原生串接入口表（組語 IBTC／串接器以同一位寬查找）。
inline constexpr unsigned native_chain_dispatch_bits=X64ARM_CHAIN_DISPATCH_BITS;
inline constexpr std::size_t native_chain_dispatch_capacity=std::size_t{1}<<native_chain_dispatch_bits;
constexpr std::size_t native_dispatch_index(std::uint64_t rip) noexcept {
    // 與常駐 AOT、服務返回及組語 IBTC 使用相同的完整 64-bit RIP CRC32C。
    return address_cache::index(rip,native_dispatch_capacity-1);
}
// 僅宿主持有；不寫入 SMAP，也不放進客體位址空間。更新／撤銷須等所有 worker 返回。
struct NativeChainNode {
    std::uint64_t rip{};
    void (*body)(){};
    std::array<NativeChainNode*,2> successors{};
    // 指令數與已驗證類型一次載入：bit 0 浮點、bit 1 stack GPR、bit 2 頁面證明、bit 3 冷入口觀測、bit 4 LLVM C ABI、bit 5 上八個 GPR、bit 6 完整保存傳入的 FPCR／FPSR、bit 7 借用鏈浮點環境的 LLVM 本體（同時設定 bit 0）。
    std::uint32_t count{},flags{};
    std::uint64_t fragment{};
    void* owner{};
    // 只供宿主快取回收使用；不改變客體計時或序列化格式。
    // 平行執行以 ARM64 對齊 LDR／STR 或 C++ atomic_ref 作 relaxed 原子存取；同批次時間戳相同便略過寫入。
    // 一般非原子讀取須等所有 worker 返回。
    std::uint64_t last_use{};
    std::uint64_t (*llvm_invoke)(GuestState*,GuestMemory*,std::uint64_t){};
};
struct NativeChainEvent { NativeChainNode* node;std::uint64_t encoded,next_rip; };
struct NativeChainContext {
    // profiling bit 0 全部事件；bit 1 只記錄仍需熱度／首次使用觀測的入口。
    std::uint64_t budget,profiling;
    std::uint64_t completed,blocks,fragment_instructions,fragment_exits;
    NativeChainNode* last;
    std::uint64_t encoded;
    NativeChainNode* const* lookup;
    std::uint64_t use_stamp{};
    std::uint64_t event_count{};
    std::uint64_t event_limit{128};
    std::array<NativeChainEvent,128> events;
    // 每批／每個 worker 自有計數，原生熱路徑不競爭共用統計。
    std::uint64_t llvm_instructions{},llvm_batches{},llvm_guard_exits{};
    // 返回後為片段內直接分派次數；執行期間暫存串接器返回次數，避免命中路徑額外計數寫入。
    std::uint64_t inline_links{};
};
extern "C" void x64arm_native_chain(GuestState*,GuestMemory*,NativeChainNode*,NativeChainContext*);
// 單次執行與串接共用同一份已發布 body；此入口保存宿主 ABI 並接回 BR X22 出口。
extern "C" std::uint64_t x64arm_native_single(GuestState*,GuestMemory*,void (*body)(),unsigned scopes=0);
extern "C" void x64arm_native_llvm();
}
