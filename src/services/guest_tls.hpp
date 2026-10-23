#pragma once
#include "guest_tls_layout.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace x64arm::detail {
constexpr std::uint64_t tls_slot_offset(bool x86,std::size_t index) noexcept {
    const unsigned unit=x86?4U:8U;
    return index<X64ARM_TLS_INLINE_SLOTS?
        (x86?X64ARM_TLS_X86_INLINE_OFFSET:X64ARM_TLS_X64_INLINE_OFFSET)+index*unit:
        X64ARM_TLS_EXPANSION_OFFSET+(index-X64ARM_TLS_INLINE_SLOTS)*unit;
}
// Windows 明列低 TLS_MINIMUM_AVAILABLE 槽只做最小驗證；擴充槽沿用配置生命週期。
constexpr bool tls_writable_index(std::uint32_t index,const std::array<bool,X64ARM_TLS_SLOTS>& used) noexcept {
    return index<used.size() && (index<X64ARM_TLS_INLINE_SLOTS || used[index]);
}
}
