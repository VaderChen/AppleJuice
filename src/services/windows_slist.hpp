#pragma once
#include "x64arm/host_services.hpp"
#include "x64arm/memory.hpp"

namespace x64arm::slist {
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
inline void update(GuestMemory& memory, std::uint64_t header, bool x86, std::uint64_t next, std::uint16_t count) {
    check(memory, header, x86, Access::Write);
    if (x86) {
        const auto sequence = memory.load(GuestAddress{header + 6}, 16);
        memory.store(GuestAddress{header}, 32, next); memory.store(GuestAddress{header + 4}, 16, count);
        memory.store(GuestAddress{header + 6}, 16, sequence + 1);
    } else {
        const auto stamp = memory.load(GuestAddress{header}, 64), flags = memory.load(GuestAddress{header + 8}, 64) & 15U;
        memory.store(GuestAddress{header + 8}, 64, next | flags);
        memory.store(GuestAddress{header}, 64, ((stamp + 65536) & ~UINT64_C(65535)) | count);
    }
}
inline std::uint64_t flush(GuestMemory& memory, std::uint64_t header, bool x86) {
    const auto previous = first(memory, header, x86); update(memory, header, x86, 0, 0); return previous;
}
inline std::uint64_t push(GuestMemory& memory, std::uint64_t header, bool x86, std::uint64_t entry) {
    check(memory, header, x86, Access::Write);
    if (!entry || entry % (x86 ? 4 : 16)) throw HostCallFault("SLIST_ENTRY 對齊錯誤");
    const auto previous = first(memory, header, x86); const auto count = depth(memory, header, x86);
    memory.store(GuestAddress{entry}, x86 ? 32 : 64, previous);
    update(memory, header, x86, entry, static_cast<std::uint16_t>(count + 1)); return previous;
}
inline std::uint64_t pop(GuestMemory& memory, std::uint64_t header, bool x86) {
    const auto previous = first(memory, header, x86); const auto count = depth(memory, header, x86);
    const auto next = previous ? memory.load(GuestAddress{previous}, x86 ? 32 : 64) : 0;
    if (next % (x86 ? 4 : 16)) throw HostCallFault("SLIST_ENTRY 對齊錯誤");
    update(memory, header, x86, next, previous ? static_cast<std::uint16_t>(count - 1) : count); return previous;
}
}
