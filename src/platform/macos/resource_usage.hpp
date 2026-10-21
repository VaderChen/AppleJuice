#pragma once
#include "x64arm/resource_usage.hpp"
#import <Metal/Metal.h>
namespace x64arm {
inline void track_gpu_commands(id<MTLCommandBuffer> commands) {
    if (!resource_usage_enabled() && !gpu_statistics_enabled()) return;
    [commands addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        if (completed.status==MTLCommandBufferStatusCompleted && completed.GPUEndTime>completed.GPUStartTime && completed.GPUStartTime>0)
            record_gpu_interval(completed.GPUStartTime,completed.GPUEndTime);
        else mark_gpu_timing_unavailable();
    }];
}
}
