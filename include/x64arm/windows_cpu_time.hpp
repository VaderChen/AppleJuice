#pragma once
#include <cstdint>
#include <optional>
#if !defined(_WIN32)
#include <sys/resource.h>
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
}
