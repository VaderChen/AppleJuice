#pragma once
#include "x64arm/memory.hpp"

namespace x64arm::windows_address_layout {
// 內建映像與可讀 gateway 共用配置；不隨載入順序改變，也不縮減客體可用位址上限。
// x64 放在直接 MMU 視窗頂端的 32 MiB 區域；PE32 保留原來的固定地址。
inline constexpr std::uint64_t module_stride=65536;
inline constexpr std::size_t module_slots=256;
inline constexpr std::uint64_t gateway_capacity=1U<<20;
inline constexpr std::uint64_t reserved_capacity=module_slots*module_stride+3*gateway_capacity;
struct Layout {std::uint64_t images,unresolved,services,callbacks;};
constexpr Layout for_mode(ExecutionMode mode) noexcept {
    const auto base=mode==ExecutionMode::X86?UINT64_C(0x7e000000):GuestMemory::native_mmu_window-(32U<<20);
    const auto gateways=base+module_slots*module_stride;
    return {base,gateways,gateways+gateway_capacity,gateways+2*gateway_capacity};
}
constexpr bool overlaps(ExecutionMode mode,std::uint64_t address,std::uint64_t size) noexcept {
    const auto base=for_mode(mode).images;
    return size && address<base+reserved_capacity && (address>=base || size>base-address);
}
inline constexpr auto x86=for_mode(ExecutionMode::X86),x64=for_mode(ExecutionMode::X64);
static_assert(x86.callbacks==0x7f200000);
static_assert(x64.callbacks+gateway_capacity<=GuestMemory::native_mmu_window);
}
