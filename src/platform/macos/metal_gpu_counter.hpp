#pragma once
#import <Metal/Metal.h>
#include "metal_command_encoder.hpp"
#include "x64arm/windows_graphics.hpp"
#include "x64arm/gpu_memory.hpp"
#include <cstring>
#include <functional>
namespace x64arm {
inline bool metal_timestamp_supported(id<MTLDevice> device){
    if(!device || (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary] && ![device supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary]))return false;
    for(id<MTLCounterSet> set in device.counterSets)if([set.name isEqualToString:MTLCommonCounterSetTimestamp])return true;
    return false;
}
// 共用 GPU counter 編碼、配置配額、encoder 生命週期及回讀驗證；caller 提供自己的 queue 邊界。
inline GraphicsTimestamp metal_timestamp_sample(id<MTLDevice> device,id<MTLCommandBuffer> commands,const std::function<void()>& finish){
    id<MTLCounterSet> set=nil;for(id<MTLCounterSet> candidate in device.counterSets)if([candidate.name isEqualToString:MTLCommonCounterSetTimestamp]){set=candidate;break;}
    if(!set || !metal_timestamp_supported(device))throw std::invalid_argument("Metal 裝置不支援 timestamp counter");
    auto reservation=reserve_gpu_memory(20);auto descriptor=[MTLCounterSampleBufferDescriptor new];descriptor.counterSet=set;descriptor.sampleCount=2;descriptor.storageMode=MTLStorageModeShared;
    NSError* error=nil;auto samples=[device newCounterSampleBufferWithDescriptor:descriptor error:&error];if(!samples)throw std::runtime_error("Metal timestamp buffer 建立失敗");
    auto marker=[device newBufferWithLength:4 options:MTLResourceStorageModePrivate];if(!marker)throw std::bad_alloc{};
    if([device supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary]){auto encoder=[commands blitCommandEncoder];MetalEncoderScope scope{encoder};[encoder sampleCountersInBuffer:samples atSampleIndex:0 withBarrier:YES];[encoder fillBuffer:marker range:NSMakeRange(0,4) value:0];[encoder sampleCountersInBuffer:samples atSampleIndex:1 withBarrier:YES];scope.end();}
    else{auto pass=[MTLBlitPassDescriptor blitPassDescriptor];pass.sampleBufferAttachments[0].sampleBuffer=samples;pass.sampleBufferAttachments[0].startOfEncoderSampleIndex=0;pass.sampleBufferAttachments[0].endOfEncoderSampleIndex=1;auto encoder=[commands blitCommandEncoderWithDescriptor:pass];MetalEncoderScope scope{encoder};[encoder fillBuffer:marker range:NSMakeRange(0,4) value:0];scope.end();}
    finish();if(commands.status==MTLCommandBufferStatusError)throw std::runtime_error("Metal timestamp command 失敗");auto data=[samples resolveCounterRange:NSMakeRange(0,2)];if(data.length<2*sizeof(MTLCounterResultTimestamp))throw std::runtime_error("Metal timestamp 回讀不完整");
    MTLCounterResultTimestamp values[2];std::memcpy(values,data.bytes,sizeof(values));if(values[1].timestamp==MTLCounterErrorValue || !values[1].timestamp)throw std::runtime_error("Metal timestamp counter 無效");
    return {values[1].timestamp,1000000000,false};
}
}
