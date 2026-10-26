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
// 在 commands 編碼一次 timestamp 取樣；回傳 sample buffer，配合完成回呼可非同步解析。
struct MetalTimestampSample {id<MTLCounterSampleBuffer> samples;std::shared_ptr<GpuMemoryReservation> reservation;NSUInteger index{};};
// 非同步時間戳的取樣格池：一個 counter sample buffer 含多組取樣格（每次 2 格）依序配出，用完才建立下一個；marker 緩衝共用。
// 省掉每次取樣都向 IOGPU 建立 counter sample buffer 與 marker buffer（D3D12 遊戲每幀數十次）。舊 buffer 由未完成的回呼持有到解析完畢。
struct MetalTimestampPool {
    static constexpr NSUInteger capacity=512;
    id<MTLCounterSet> set;id<MTLCounterSampleBuffer> current;id<MTLBuffer> marker;std::shared_ptr<GpuMemoryReservation> reservation;NSUInteger next{capacity};
};
inline id<MTLCounterSet> metal_timestamp_counter_set(id<MTLDevice> device){
    for(id<MTLCounterSet> candidate in device.counterSets)if([candidate.name isEqualToString:MTLCommonCounterSetTimestamp])return candidate;
    return nil;
}
inline MetalTimestampSample metal_timestamp_encode(id<MTLDevice> device,id<MTLCommandBuffer> commands,MetalTimestampPool* pool=nullptr){
    MetalTimestampSample result;id<MTLBuffer> marker=nil;
    if(pool){
        if(!pool->set){pool->set=metal_timestamp_counter_set(device);if(!pool->set || !metal_timestamp_supported(device))throw std::invalid_argument("Metal 裝置不支援 timestamp counter");}
        if(!pool->marker){pool->marker=[device newBufferWithLength:4 options:MTLResourceStorageModePrivate];if(!pool->marker)throw std::bad_alloc{};}
        if(!pool->current || pool->next+2>MetalTimestampPool::capacity){
            auto reservation=reserve_gpu_memory(MetalTimestampPool::capacity*8);auto descriptor=[MTLCounterSampleBufferDescriptor new];descriptor.counterSet=pool->set;
            descriptor.sampleCount=MetalTimestampPool::capacity;descriptor.storageMode=MTLStorageModeShared;
            NSError* error=nil;auto samples=[device newCounterSampleBufferWithDescriptor:descriptor error:&error];if(!samples)throw std::runtime_error("Metal timestamp buffer 建立失敗");
            pool->current=samples;pool->reservation=std::move(reservation);pool->next=0;
        }
        result={pool->current,pool->reservation,pool->next};pool->next+=2;marker=pool->marker;
    }else{
        id<MTLCounterSet> set=metal_timestamp_counter_set(device);
        if(!set || !metal_timestamp_supported(device))throw std::invalid_argument("Metal 裝置不支援 timestamp counter");
        auto reservation=reserve_gpu_memory(20);auto descriptor=[MTLCounterSampleBufferDescriptor new];descriptor.counterSet=set;descriptor.sampleCount=2;descriptor.storageMode=MTLStorageModeShared;
        NSError* error=nil;auto samples=[device newCounterSampleBufferWithDescriptor:descriptor error:&error];if(!samples)throw std::runtime_error("Metal timestamp buffer 建立失敗");
        marker=[device newBufferWithLength:4 options:MTLResourceStorageModePrivate];if(!marker)throw std::bad_alloc{};
        result={samples,reservation,0};
    }
    const auto first=result.index;
    if([device supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary]){auto encoder=[commands blitCommandEncoder];MetalEncoderScope scope{encoder};[encoder sampleCountersInBuffer:result.samples atSampleIndex:first withBarrier:YES];[encoder fillBuffer:marker range:NSMakeRange(0,4) value:0];[encoder sampleCountersInBuffer:result.samples atSampleIndex:first+1 withBarrier:YES];scope.end();}
    else{auto pass=[MTLBlitPassDescriptor blitPassDescriptor];pass.sampleBufferAttachments[0].sampleBuffer=result.samples;pass.sampleBufferAttachments[0].startOfEncoderSampleIndex=first;pass.sampleBufferAttachments[0].endOfEncoderSampleIndex=first+1;auto encoder=[commands blitCommandEncoderWithDescriptor:pass];MetalEncoderScope scope{encoder};[encoder fillBuffer:marker range:NSMakeRange(0,4) value:0];scope.end();}
    return result;
}
// 命令緩衝完成後解析；失敗以例外回報。
inline GraphicsTimestamp metal_timestamp_resolve(id<MTLCounterSampleBuffer> samples,NSUInteger index=0){
    auto data=[samples resolveCounterRange:NSMakeRange(index,2)];if(data.length<2*sizeof(MTLCounterResultTimestamp))throw std::runtime_error("Metal timestamp 解析失敗");
    MTLCounterResultTimestamp values[2];std::memcpy(values,data.bytes,sizeof(values));if(values[1].timestamp==MTLCounterErrorValue || !values[1].timestamp)throw std::runtime_error("Metal timestamp counter 無效");
    return {values[1].timestamp,1000000000,false};
}
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
