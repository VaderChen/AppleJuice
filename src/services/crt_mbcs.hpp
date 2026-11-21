#pragma once
#include "x64arm/memory.hpp"
#include "crt_mbc_contract.h"
#include "x64arm/windows_runtime.hpp"
#include <cstdint>

namespace x64arm {
void initialize_crt_multibyte(GuestMemory&,std::uint64_t process_base,std::uint32_t codepage);

// _locale_t 的 mbcinfo 指標（locinfo 之後一格）。locale 物件與 MBCS 字串函式共用同一份客體 ABI 狀態：
// +4 碼頁、+8 是否為多位元組碼頁（UTF-8 依 Windows 視為單位元組）、+29 起 256 位元組 _mbctype（4＝前導、8＝後隨）、+285 起小寫對照。
inline std::uint64_t crt_mbc_address(const GuestMemory& memory,const CrtLocaleObject& object,bool x86){
    return memory.load(GuestAddress{object.storage+(x86?4U:8U)},x86?32:64);
}
}
