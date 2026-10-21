#pragma once
#include "x64arm/windows_runtime.hpp"

namespace x64arm {
// PEB 與排程器共用同一把載入鎖。只發布客體位址及公開 TID。
inline void publish_loader_lock(GuestMemory& memory,std::uint64_t peb,bool x86,
                                std::uint64_t owner,std::uint32_t depth) {
    const auto address=peb+0x800;const auto bits=static_cast<std::uint8_t>(x86?32:64);
    memory.store(GuestAddress{peb+(x86?0xa0:0x110)},bits,address);
    memory.store(GuestAddress{address+(x86?4:8)},32,depth?depth-1U:UINT32_MAX);
    memory.store(GuestAddress{address+(x86?8:12)},32,depth);
    memory.store(GuestAddress{address+(x86?12:16)},bits,depth?owner:0);
}
}
