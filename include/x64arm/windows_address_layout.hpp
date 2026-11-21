#pragma once
#include "x64arm/memory.hpp"

namespace x64arm::windows_address_layout {
// 內建映像與可讀 gateway 的配置不隨載入順序改變。主映像區和 gateway 保留既有位址，
// 擴充區承接超出主映像區的模組，映像內容仍只在載入時配置。
inline constexpr std::uint64_t module_stride=65536;
inline constexpr std::size_t primary_module_slots=256;
inline constexpr std::size_t module_slots=1024;
inline constexpr std::size_t private_code_slots=32;
inline constexpr std::size_t builtin_module_slots=module_slots-private_code_slots;
inline constexpr std::uint64_t overflow_capacity=(module_slots-primary_module_slots)*module_stride;
inline constexpr std::uint64_t gateway_capacity=1U<<20;
inline constexpr std::uint64_t reserved_capacity=primary_module_slots*module_stride+3*gateway_capacity;
struct Layout {std::uint64_t images,unresolved,services,callbacks,overflow;};
constexpr Layout for_mode(ExecutionMode mode) noexcept {
    const auto base=mode==ExecutionMode::X86?UINT64_C(0x7e000000):GuestMemory::native_mmu_window-(32U<<20);
    const auto gateways=base+primary_module_slots*module_stride;
    // PE32 擴充區位於 TEB／PEB 與主堆疊提示位址之間，保留除錯頁與 D3D11 命令串流的原有空間。
    const auto overflow=mode==ExecutionMode::X86?UINT64_C(0x73000000):base-overflow_capacity;
    return {base,gateways,gateways+gateway_capacity,gateways+2*gateway_capacity,overflow};
}
constexpr std::uint64_t module_address(ExecutionMode mode,std::size_t first_slot) noexcept {
    const auto layout=for_mode(mode);
    return first_slot<primary_module_slots?layout.images+first_slot*module_stride:
        layout.overflow+(first_slot-primary_module_slots)*module_stride;
}
// 擴充映像區尾端保留給 resident AOT 的私有 PE，沿用 64 KiB 配置粒度。
constexpr std::uint64_t private_code_address(ExecutionMode mode,std::size_t slot) noexcept {
    return module_address(mode,builtin_module_slots+slot);
}
constexpr bool overlaps(ExecutionMode mode,std::uint64_t address,std::uint64_t size) noexcept {
    const auto layout=for_mode(mode);
    const auto intersects=[=](std::uint64_t base,std::uint64_t capacity){
        return size && address<base+capacity && (address>=base || size>base-address);
    };
    return intersects(layout.images,reserved_capacity) || intersects(layout.overflow,overflow_capacity);
}
inline constexpr auto x86=for_mode(ExecutionMode::X86),x64=for_mode(ExecutionMode::X64);
static_assert(x86.callbacks==0x7f200000);
static_assert(x64.callbacks+gateway_capacity<=GuestMemory::native_mmu_window);
static_assert(x86.overflow+overflow_capacity<=0x78000000);

// 主堆疊必須避開尚未建立的系統映射；服務層與載入器共用地址契約。
struct ProcessLayout {std::uint64_t stack,command_line,runtime,teb,peb,exceptions;};
constexpr ProcessLayout process_for_mode(ExecutionMode mode) noexcept {
    return mode==ExecutionMode::X86?
        ProcessLayout{0x78000000,0x70000000,0x70010000,0x70200000,0x70210000,0x70300000}:
        ProcessLayout{0x3000000000,0x3001000000,0x3002000000,0x3004000000,0x3004010000,0x3005000000};
}
inline constexpr auto process_x64=process_for_mode(ExecutionMode::X64);
inline constexpr std::uint64_t process_heap=0x7ffd0000,shared_user_data=0x7ffe0000;
constexpr std::uint64_t user_limit(ExecutionMode mode,bool large_address_aware) noexcept {
    if(mode==ExecutionMode::X86)return large_address_aware?UINT64_C(0xffff0000):UINT64_C(0x7fff0000);
    const auto mmu_limit=GuestMemory::native_mmu_window-0x10000;
    return mmu_limit<UINT64_C(0x7fffffff0000)?mmu_limit:UINT64_C(0x7fffffff0000);
}
constexpr std::uint64_t process_reserved_end(ExecutionMode mode,std::uint64_t first,std::uint64_t end) noexcept {
    const auto p=process_for_mode(mode);
    const auto layout=for_mode(mode);
    struct Range {std::uint64_t base,size;};
    // 命令列／TEB／PEB 預留一個配置粒度；runtime 與例外區沿用既有最大映射大小。
    const Range ranges[]{{p.command_line,65536},{p.runtime,1U<<20},{p.teb,65536},{p.peb,65536},
        {p.exceptions,16U*8192},{process_heap,GuestMemory::guest_page_size},
        {shared_user_data,GuestMemory::guest_page_size},{layout.images,reserved_capacity},{layout.overflow,overflow_capacity}};
    auto high=first;
    for(const auto range:ranges)
        if(first<range.base+range.size && range.base<end && high<range.base+range.size)high=range.base+range.size;
    return high;
}
}
