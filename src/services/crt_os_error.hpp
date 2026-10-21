#pragma once
#include "x64arm/memory.hpp"
#include <cstdint>
namespace x64arm::crt_os_error {
// 主執行緒 runtime 與其他執行緒 TEB 都預留八位元組：errno、DOS error。
inline std::uint64_t cell(std::uint64_t errno_cell){return errno_cell+4;}
inline unsigned mapped(unsigned error){
    switch(error){case 2:case 3:return 2;case 4:return 24;case 6:return 9;case 8:return 12;case 17:return 18;
    case 80:case 183:return 17;case 87:return 22;case 109:return 32;case 112:return 28;case 145:return 41;default:return 13;}
}
inline void store(GuestMemory& memory,std::uint64_t errno_cell,unsigned error){
    memory.store(GuestAddress{cell(errno_cell)},32,error);
    memory.store(GuestAddress{errno_cell},32,mapped(error));
}
}
