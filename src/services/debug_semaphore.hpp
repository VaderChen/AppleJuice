#pragma once
// 診斷（X64ARM_DEBUG_SEMAPHORE=1，預設關閉）：號誌計數的每次釋放與取用（站點、控制代碼、前後計數、客體執行緒），
// 用來追查喚醒遺失。只寫 stderr，不改變任何狀態。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <time.h>
namespace x64arm::detail {
inline bool debug_semaphore_enabled() noexcept {static const bool value=std::getenv("X64ARM_DEBUG_SEMAPHORE")!=nullptr;return value;}
inline void debug_semaphore(const char* site,std::uint64_t handle,std::uint64_t before,std::uint64_t after,std::uint64_t thread) noexcept {
    if(!debug_semaphore_enabled())return;
    std::fprintf(stderr,"[debug_sem] t=%llu site=%s handle=0x%llx count=%llu->%llu thread=%llu\n",
        static_cast<unsigned long long>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)/1000),site,static_cast<unsigned long long>(handle),
        static_cast<unsigned long long>(before),static_cast<unsigned long long>(after),static_cast<unsigned long long>(thread));
}
}
