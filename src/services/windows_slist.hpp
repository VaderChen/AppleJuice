#pragma once
#include "x64arm/host_services.hpp"
#include "x64arm/memory.hpp"

#include <array>

namespace x64arm::slist {
// SLIST_HEADER 格式與客體常式（guest_thread_x64.s／guest_thread_x86.s）相同：
// - x64（16 位元組）：低 64 位元 Depth:16｜Sequence:48，高 64 位元 16 位元組對齊的第一項指標｜低 4 位元保留旗標。
// - x86（8 位元組）：Next:32｜Depth:16｜Sequence:16。
// 服務路徑可能在其他 vCPU 正以 LOCK CMPXCHG16B／CMPXCHG8B 更新同一標頭時執行（協調者本地服務不全停），
// 所以每次更新都以整個標頭的比較交換完成（序號加一防 ABA），與 Windows 及客體常式的原子契約相同。
inline unsigned size(bool x86) { return x86 ? 8U : 16U; }
inline void check(GuestMemory& memory, std::uint64_t header, bool x86, Access access) {
    if (!header || header % size(x86)) throw HostCallFault("SLIST_HEADER 對齊錯誤");
    memory.check(GuestAddress{header}, size(x86), access);
}
inline std::uint64_t first(GuestMemory& memory, std::uint64_t header, bool x86) {
    check(memory, header, x86, Access::Read);
    return x86 ? memory.load(GuestAddress{header}, 32) : memory.load(GuestAddress{header + 8}, 64) & ~UINT64_C(15);
}
inline std::uint16_t depth(GuestMemory& memory, std::uint64_t header, bool x86) {
    check(memory, header, x86, Access::Read);
    return static_cast<std::uint16_t>(memory.load(GuestAddress{header + (x86 ? 4 : 0)}, 16));
}
namespace detail {
// 標頭快照：x64 為 {低 64, 高 64}；x86 只用 [0]（整個 8 位元組）。
using Header = std::array<std::uint64_t, 2>;
inline Header read(GuestMemory& memory, std::uint64_t header, bool x86) {
    if (x86) return {memory.load(GuestAddress{header}, 64), 0};
    return {memory.load(GuestAddress{header}, 64), memory.load(GuestAddress{header + 8}, 64)};
}
inline std::uint64_t head(const Header& value, bool x86) { return x86 ? value[0] & UINT64_C(0xffffffff) : value[1] & ~UINT64_C(15); }
inline std::uint16_t count(const Header& value, bool x86) { return static_cast<std::uint16_t>(x86 ? value[0] >> 32 : value[0]); }
// 新標頭：第一項 next、深度 depth_now，序號加一，x64 保留旗標位元。
inline Header next_header(const Header& value, bool x86, std::uint64_t next, std::uint16_t depth_now) {
    if (x86) {
        const auto sequence = static_cast<std::uint16_t>((value[0] >> 48) + 1);
        return {(next & UINT64_C(0xffffffff)) | (std::uint64_t{depth_now} << 32) | (std::uint64_t{sequence} << 48), 0};
    }
    return {((value[0] + 65536) & ~UINT64_C(65535)) | depth_now, next | (value[1] & 15U)};
}
// 比較交換整個標頭；成功回 true，失敗時把觀察到的值寫回 value。
inline bool exchange(GuestMemory& memory, std::uint64_t header, bool x86, Header& value, const Header& desired) {
    if (x86) {
        const auto observed = memory.atomic_rmw(GuestAddress{header}, 64, GuestMemory::AtomicRmw::CompareExchange, desired[0], value[0]);
        if (observed == value[0]) return true;
        value = {observed, 0};
        return false;
    }
    const auto observed = memory.compare_exchange128(GuestAddress{header}, value, desired);
    if (observed == value) return true;
    value = observed;
    return false;
}
}
inline std::uint64_t flush(GuestMemory& memory, std::uint64_t header, bool x86) {
    check(memory, header, x86, Access::Write);
    auto value = detail::read(memory, header, x86);
    for (;;) {
        const auto previous = detail::head(value, x86);
        if (!previous) return 0;
        if (detail::exchange(memory, header, x86, value, detail::next_header(value, x86, 0, 0))) return previous;
    }
}
// InterlockedPushListSListEx：把已串好的 first…last 一次推入，last 接上原本的第一項。
inline std::uint64_t push_list(GuestMemory& memory, std::uint64_t header, bool x86, std::uint64_t entry, std::uint64_t last, std::uint32_t added) {
    check(memory, header, x86, Access::Write);
    if (!entry || !last || entry % (x86 ? 4 : 16) || last % (x86 ? 4 : 16)) throw HostCallFault("SLIST_ENTRY 對齊錯誤");
    auto value = detail::read(memory, header, x86);
    for (;;) {
        const auto previous = detail::head(value, x86);
        // 每次重試都改寫 last->Next 成目前的第一項（與客體常式相同）。
        memory.store(GuestAddress{last}, x86 ? 32 : 64, previous);
        const auto depth_now = static_cast<std::uint16_t>(detail::count(value, x86) + added);
        if (detail::exchange(memory, header, x86, value, detail::next_header(value, x86, entry, depth_now))) return previous;
    }
}
inline std::uint64_t push(GuestMemory& memory, std::uint64_t header, bool x86, std::uint64_t entry) {
    return push_list(memory, header, x86, entry, entry, 1);
}
inline std::uint64_t pop(GuestMemory& memory, std::uint64_t header, bool x86) {
    check(memory, header, x86, Access::Write);
    auto value = detail::read(memory, header, x86);
    for (;;) {
        const auto previous = detail::head(value, x86);
        if (!previous) return 0;
        // 讀 previous->Next 與比較交換之間，其他執行緒可能已取走並釋放該項；讀取故障時重讀標頭再試
        // （Windows 以 RtlpInterlockedPopEntrySList 的例外處理達成同一語義）。標頭若沒變就是真的故障。
        std::uint64_t next = 0;
        try {
            next = memory.load(GuestAddress{previous}, x86 ? 32 : 64);
        } catch (const MemoryFault&) {
            const auto current = detail::read(memory, header, x86);
            if (current == value) throw;
            value = current;
            continue;
        }
        if (next % (x86 ? 4 : 16)) {
            // 可能是併發改寫中的舊內容；標頭未變才是串列本身損毀。
            const auto current = detail::read(memory, header, x86);
            if (current == value) throw HostCallFault("SLIST_ENTRY 對齊錯誤");
            value = current;
            continue;
        }
        const auto desired = detail::next_header(value, x86, next, static_cast<std::uint16_t>(detail::count(value, x86) - 1));
        if (detail::exchange(memory, header, x86, value, desired)) return previous;
    }
}
}
