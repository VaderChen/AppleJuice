#pragma once
#include "x64arm/windows_abi.hpp"
#include "x64arm/windows_guid.hpp"
#include <string>
#include <optional>

namespace x64arm {
// 與 FormatMessage 共用；找不到代碼時回傳空字串，RoOriginateError 自行採用 E_FAIL。
std::u16string windows_error_message(std::uint32_t);
std::string_view windows_crt_error_message(std::uint32_t);
// 使用 Windows errc 數值；不使用宿主 errno，且與 CRT _dosmaperr 契約分開。
std::optional<std::uint32_t> windows_generic_error_condition(std::uint32_t) noexcept;
std::string_view windows_generic_error_message(std::uint32_t) noexcept;
namespace winrt_error_detail {
inline constexpr WindowsGuid restricted_iid{0x92,0x70,0xba,0x82,0x88,0x4c,0x7d,0x42,0xa7,0xbc,0x16,0xdd,0x93,0xfe,0xb6,0x7e};
inline constexpr WindowsGuid error_iid{0x20,0xb1,0xf2,0x1c,0x7d,0x54,0x1b,0x10,0x8e,0x65,8,0,0x2b,0x2b,0xd1,0x19};
inline std::uint64_t method(const GuestMemory& memory,std::uint64_t object,unsigned slot){
    const std::uint8_t bits=memory.execution_mode()==ExecutionMode::X86?32:64;
    const auto table=memory.load(GuestAddress{object},bits),offset=std::uint64_t{slot}*(bits/8);
    if(!table || table>UINT64_MAX-offset)throw HostCallFault("錯誤物件 IUnknown vtable 不合法");
    const auto target=memory.load(GuestAddress{table+offset},bits);
    if(!target)throw HostCallFault("錯誤物件 IUnknown 方法為空");return target;
}
}
}
