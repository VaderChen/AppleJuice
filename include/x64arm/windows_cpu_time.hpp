#pragma once
#include <cstdint>
#include <optional>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach_time.h>
#endif
namespace x64arm {
struct ProcessCpuTime {std::uint64_t user{},kernel{};};
inline std::optional<ProcessCpuTime> host_process_cpu_time() {
#if !defined(_WIN32)
    rusage usage{};if(::getrusage(RUSAGE_SELF,&usage))return {};
    const auto ticks=[](const timeval& time){return static_cast<std::uint64_t>(time.tv_sec)*10000000+static_cast<std::uint64_t>(time.tv_usec)*10;};
    return ProcessCpuTime{ticks(usage.ru_utime),ticks(usage.ru_stime)};
#else
    return {};
#endif
}
// 同一使用者的另一宿主程序的 CPU 時間（100 ns 單位）；proc_pid_rusage 以 mach tick 計，須換算。
inline std::optional<ProcessCpuTime> host_process_cpu_time(int pid) {
#if defined(__APPLE__)
    rusage_info_v0 info{};if(::proc_pid_rusage(pid,RUSAGE_INFO_V0,reinterpret_cast<rusage_info_t*>(&info)))return {};
    static const auto timebase=[] {mach_timebase_info_data_t value{};::mach_timebase_info(&value);return value;}();
    if(!timebase.denom)return {};
    const auto ticks=[](std::uint64_t mach){
        return static_cast<std::uint64_t>(static_cast<unsigned __int128>(mach)*timebase.numer/timebase.denom/100U);};
    return ProcessCpuTime{ticks(info.ri_user_time),ticks(info.ri_system_time)};
#else
    (void)pid;return {};
#endif
}
}
