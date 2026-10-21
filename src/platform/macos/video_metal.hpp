#pragma once
#include "x64arm/video_codec.hpp"
#import <Metal/Metal.h>
namespace x64arm::video {
// CVPixelBuffer、texture cache 與兩個 CVMetalTexture 同時存活；GPU commands 完成前須保留此物件。
// 兩個 texture 直接引用同一 IOSurface，沒有 CPU NV12 複製；range 沿用 DecodedFrame.full_range。
class MetalVideoFrame {
public:
    MetalVideoFrame(const DecodedFrame&,id<MTLDevice>);
    id<MTLTexture> luma()const;
    id<MTLTexture> chroma()const;
private:
    struct State;std::shared_ptr<State> state_;
};
}
