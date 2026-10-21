#pragma once
#include <chrono>
#include <cstdint>
#include <ctime>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace x64arm::detail {
// 排程、共用時間與關機期限使用同一條 100 ns 單調時間軸。
// macOS 直接讀取硬體計數器（mach_continuous_time，含睡眠期間，與 Windows QPC 相同語義）；
// 各程序讀到同一個硬體時間軸，宿主時鐘模式不以程序內累計值推算時間。
inline std::uint64_t monotonic_ticks() noexcept {
#if defined(__APPLE__)
    static const auto timebase=[] {mach_timebase_info_data_t value{};if(mach_timebase_info(&value) || !value.denom)value={1,1};return value;}();
    const auto counter=mach_continuous_time();
    // 128 位元乘除，避免長時間運作後溢位。
    const auto numerator=static_cast<unsigned __int128>(counter)*timebase.numer;
    return static_cast<std::uint64_t>(numerator/(static_cast<unsigned __int128>(timebase.denom)*100U));
#else
    using Tick=std::chrono::duration<std::int64_t,std::ratio<1,10000000>>;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<Tick>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
// 不含睡眠期間的硬體時間（Windows unbiased interrupt time）；macOS 為 mach_absolute_time。
inline std::uint64_t unbiased_ticks() noexcept {
#if defined(__APPLE__)
    static const auto timebase=[] {mach_timebase_info_data_t value{};if(mach_timebase_info(&value) || !value.denom)value={1,1};return value;}();
    const auto numerator=static_cast<unsigned __int128>(mach_absolute_time())*timebase.numer;
    return static_cast<std::uint64_t>(numerator/(static_cast<unsigned __int128>(timebase.denom)*100U));
#else
    timespec now{};::clock_gettime(CLOCK_MONOTONIC,&now);
    return static_cast<std::uint64_t>(now.tv_sec)*10000000U+static_cast<std::uint64_t>(now.tv_nsec)/100U;
#endif
}
}
