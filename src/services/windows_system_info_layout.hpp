// GetSystemInfo／GetNativeSystemInfo 的 SYSTEM_INFO 內容（Win32 36 bytes、Win64 48 bytes）。協調者服務與 worker 平行服務共用，
// 兩邊寫入的位元組完全相同。Win64 客體兩個 API 都回報 AMD64；Win32 客體的 GetSystemInfo 回報 INTEL、GetNativeSystemInfo 回報 AMD64。
#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace x64arm {
struct SystemInfoBytes {std::array<std::uint8_t,48> bytes{};unsigned size{};};
inline SystemInfoBytes system_info_bytes(bool x86,bool native,unsigned processors,std::uint64_t maximum_address) noexcept {
    SystemInfoBytes info;info.size=x86?36U:48U;
    const auto put=[&](unsigned offset,std::uint64_t value,unsigned size){std::memcpy(info.bytes.data()+offset,&value,size);}; // 小端序
    const auto mask=processors>=64?UINT64_MAX:(UINT64_C(1)<<processors)-1;
    if(x86){
        put(0,native?9:0,2);put(4,4096,4);put(8,65536,4);put(12,maximum_address,4);put(16,mask,4);
        put(20,processors,4);put(24,586,4);put(28,65536,4);put(32,6,2);
    }else{
        put(0,9,2);put(4,4096,4);put(8,65536,8);put(16,maximum_address,8);put(24,mask,8);
        put(32,processors,4);put(36,8664,4);put(40,65536,4);put(44,6,2);
    }
    return info;
}
}
