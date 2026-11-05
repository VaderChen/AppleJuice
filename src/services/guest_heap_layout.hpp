#pragma once
#include <algorithm>
#include <cstdint>
#include "guest_heap_cache_layout.h"

namespace x64arm::detail::guest_heap_layout {
// 與客體配置常式及宿主補充器共用；worker 只消耗已存在的 chunk。
inline constexpr std::uint64_t guest_heap_chunk=0x40000,guest_heap_header=64,
    guest_heap_magic=0x50414548343658abULL,guest_heap_small_limit=4096,
    guest_heap_limit=65536,guest_heap_reservation=UINT64_C(0x100000000),
    guest_heap_aligned_header=X64ARM_GUEST_HEAP_ALIGNED_HEADER;
inline constexpr unsigned guest_heap_heads=0x40;
// x64 每執行緒級距快取（TEB 的 CRT 私有頁）：涵蓋 1 KiB payload 與對齊標頭，串列頭 +0x4600+class*8、
// 數量 +0x4810+class（每級距至多 32）。只有該執行緒的客體常式讀寫；宿主在執行緒結束時歸還全域串列。
inline constexpr std::uint64_t guest_heap_cache_offset=X64ARM_GUEST_HEAP_CACHE_HEAD_OFFSET,
    guest_heap_cache_count_offset=X64ARM_GUEST_HEAP_CACHE_COUNT_OFFSET;
inline constexpr unsigned guest_heap_cache_classes=X64ARM_GUEST_HEAP_CACHE_CLASSES,
    guest_heap_cache_limit=X64ARM_GUEST_HEAP_CACHE_LIMIT;
constexpr std::uint64_t guest_heap_class(std::uint64_t size) {
    return size<=guest_heap_small_limit?std::max(UINT64_C(1),(size+15)/16):
        256+(size-guest_heap_small_limit+255)/256;
}
constexpr std::uint64_t guest_heap_class_size(std::uint64_t cls) {
    return cls<=256?cls*16:guest_heap_small_limit+(cls-256)*256;
}
constexpr bool valid_block(std::uint64_t address,std::uint64_t cls,std::uint64_t block) {
    const auto chunk=address&~(guest_heap_chunk-1);
    return cls && cls<=guest_heap_class(guest_heap_limit+guest_heap_aligned_header) &&
        block==guest_heap_class_size(cls) && address>=chunk+guest_heap_header &&
        address<=chunk+guest_heap_chunk-block && (address-chunk-guest_heap_header)%block==0;
}
}
