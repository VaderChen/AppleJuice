#pragma once
#include "x64arm/windows_abi.hpp"
#include <array>
#include <vector>
#include <stdexcept>

namespace x64arm::detail {
// Nt／Zw 名稱不保證是核心系統呼叫；GetTickCount 是使用者模式的時鐘函式。
inline constexpr bool nt_system_service(std::string_view symbol){
    return (symbol.starts_with("Nt") || symbol.starts_with("Zw")) &&
        symbol.substr(2)!="GetTickCount" && symbol.substr(2)!="CurrentTeb";
}
inline std::uint16_t nt_entry_stack_bytes(const WindowsServiceDescriptor& service){
    const auto slots=service.arguments+static_cast<unsigned>(std::popcount(service.wide_arguments))+
        3U*static_cast<unsigned>(std::popcount(service.aggregate_arguments));
    if(slots>UINT16_MAX/4)throw std::invalid_argument("NT 服務堆疊參數超出返回指令容量");
    return static_cast<std::uint16_t>(service.caller_cleanup?0:slots*4);
}
// 產生匯出與偵測客體攔截必須使用同一版入口，避免正常入口被誤認為已改寫。
// x64 與 Windows 10（1511 起）的 ntdll 相同：mov r10,rcx; mov eax,id; 依 KUSER_SHARED_DATA.SystemCall（0x7ffe0308）
// 第 0 位元選擇 syscall 或 int 2e；32 位元組一槽，結尾以多位元組 nop 補齊。x86 為 WoW64 的 16 位元組版面。
inline constexpr unsigned nt_service_entry_size(bool x86){return x86?16U:32U;}
inline std::vector<std::uint8_t> nt_service_entry(bool x86,std::uint32_t id,
        std::uint16_t stack_bytes,std::uint64_t x86_transition){
    std::vector<std::uint8_t> bytes=x86?
        std::vector<std::uint8_t>{0xb8,0,0,0,0,0xba,0,0,0,0,0xff,0xd2,0xc2,0,0,0x90}:
        std::vector<std::uint8_t>{0x4c,0x8b,0xd1,0xb8,0,0,0,0,0xf6,0x04,0x25,0x08,0x03,0xfe,0x7f,0x01,
                                  0x75,0x03,0x0f,0x05,0xc3,0xcd,0x2e,0xc3,0x0f,0x1f,0x84,0x00,0x00,0x00,0x00,0x00};
    const auto put=[&](unsigned at,unsigned count,std::uint64_t value){for(unsigned n=0;n<count;++n)bytes[at+n]=static_cast<std::uint8_t>(value>>(8*n));};
    put(x86?1:4,4,id);
    if(x86){put(6,4,x86_transition);put(13,2,stack_bytes);}
    return bytes;
}
}
