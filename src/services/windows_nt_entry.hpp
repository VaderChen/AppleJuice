#pragma once
#include "x64arm/windows_abi.hpp"
#include <array>
#include <stdexcept>

namespace x64arm::detail {
inline std::uint16_t nt_entry_stack_bytes(const WindowsServiceDescriptor& service){
    const auto slots=service.arguments+static_cast<unsigned>(std::popcount(service.wide_arguments))+
        3U*static_cast<unsigned>(std::popcount(service.aggregate_arguments));
    if(slots>UINT16_MAX/4)throw std::invalid_argument("NT 服務堆疊參數超出返回指令容量");
    return static_cast<std::uint16_t>(service.caller_cleanup?0:slots*4);
}
// 產生匯出與偵測客體攔截必須使用同一版入口，避免正常入口被誤認為已改寫。
inline std::array<std::uint8_t,16> nt_service_entry(bool x86,std::uint32_t id,
        std::uint16_t stack_bytes,std::uint64_t x86_transition){
    std::array<std::uint8_t,16> bytes=x86?
        std::array<std::uint8_t,16>{0xb8,0,0,0,0,0xba,0,0,0,0,0xff,0xd2,0xc2,0,0,0x90}:
        std::array<std::uint8_t,16>{0x4c,0x8b,0xd1,0xb8,0,0,0,0,0x0f,0x05,0xc3,0x66,0x66,0x90,0x66,0x90};
    const auto put=[&](unsigned at,unsigned count,std::uint64_t value){for(unsigned n=0;n<count;++n)bytes[at+n]=static_cast<std::uint8_t>(value>>(8*n));};
    put(x86?1:4,4,id);
    if(x86){put(6,4,x86_transition);put(13,2,stack_bytes);}
    return bytes;
}
}
