#pragma once
#include "gui_timing.hpp"
#import <Metal/Metal.h>

namespace x64arm::macos_detail {
// 記錄客體提交、GPU 完成及真正上屏；回呼只保留量測器與值，不延長視窗生命週期。
struct MetalFrameTiming {
    std::shared_ptr<GuiTiming> recorder;
    std::uint64_t surface{},frame{};
    std::uint32_t width{},height{};
    double request{},submission{};

    void submit(id<MTLCommandBuffer> command) const {
        if(!recorder)return;
        const auto record=*this;
        recorder->frame("submitted",surface,frame,width,height,request,submission);
        [command addCompletedHandler:^(id<MTLCommandBuffer> buffer){
            record.recorder->frame("completed",record.surface,record.frame,record.width,record.height,
                                   record.request,record.submission,0,buffer.GPUStartTime,buffer.GPUEndTime);
        }];
    }
    void present(id<MTLDrawable> drawable) const {
        if(!recorder)return;
        const auto record=*this;
        [drawable addPresentedHandler:^(id<MTLDrawable> shown){
            // 沿用 GDI 的回呼格式。零時間戳記保留為未確認上屏，不得由分析器計為顯示幀。
            record.recorder->frame("presented",record.surface,record.frame,record.width,record.height,
                                   record.request,record.submission,shown.presentedTime);
        }];
    }
};
}
