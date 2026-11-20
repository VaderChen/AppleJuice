#pragma once
#include "x64arm/windows_services.hpp"

namespace x64arm::winrt_header {
// HSTRING_HEADER 的前四個欄位都是 UINT32；字元位址在兩種 ABI 都位於 +16。
inline void write(GuestMemory& memory,std::uint64_t handle,std::uint64_t data,std::uint32_t length,bool reference){
    const std::uint8_t bits=memory.execution_mode()==ExecutionMode::X86?32:64;
    memory.store(GuestAddress{handle},32,reference?1:0);
    memory.store(GuestAddress{handle+4},32,length);
    memory.store(GuestAddress{handle+8},64,0);
    memory.store(GuestAddress{handle+16},bits,data);
}
}
