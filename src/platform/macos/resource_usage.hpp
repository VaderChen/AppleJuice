#pragma once
#include "x64arm/resource_usage.hpp"
#import <Metal/Metal.h>
#include <functional>
#include <string>
namespace x64arm {
// GPU 卡住防護（metal_gpu_watchdog.mm）：登記即將 commit 的命令緩衝；逾時未完成時印出 describe() 的診斷後結束程序。
void watch_gpu_commands(id<MTLCommandBuffer> commands, std::function<std::string()> describe = {});
// 所有 Metal 後端在 commit 前呼叫：登記 GPU 卡住防護，並在需要時累計 GPU 忙碌區間。
inline void track_gpu_commands(id<MTLCommandBuffer> commands, std::function<std::string()> describe = {}) {
    watch_gpu_commands(commands, std::move(describe));
    if (!resource_usage_enabled() && !gpu_statistics_enabled()) return;
    [commands addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        if (completed.status==MTLCommandBufferStatusCompleted && completed.GPUEndTime>completed.GPUStartTime && completed.GPUStartTime>0)
            record_gpu_interval(completed.GPUStartTime,completed.GPUEndTime);
        else mark_gpu_timing_unavailable();
    }];
}
}
