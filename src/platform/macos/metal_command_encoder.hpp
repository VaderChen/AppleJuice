#pragma once
#import <Metal/Metal.h>
#include <stdexcept>

namespace x64arm {
// D3D12 描述元表（bindless）經 argument buffer 存取的資源不在 Metal 自動危害追蹤內，D3D12 屏障也不經過 Metal。
// 使用過描述元表後，編碼執行緒的每個 encoder 先等待前一個 encoder 更新的 fence、結束時再更新，
// 依提交順序串接（D3D12 資源屏障的保守對應）。由 MetalGl 在開始編碼時設定；其他執行緒（呈現）不受影響。
inline thread_local __unsafe_unretained id<MTLFence> metal_encoder_chain=nil;
inline void metal_chain_wait(id<MTLCommandEncoder> encoder){
    if(!metal_encoder_chain || !encoder)return;
    if([encoder conformsToProtocol:@protocol(MTLRenderCommandEncoder)])[(id<MTLRenderCommandEncoder>)encoder waitForFence:metal_encoder_chain beforeStages:MTLRenderStageVertex];
    else if([encoder conformsToProtocol:@protocol(MTLComputeCommandEncoder)])[(id<MTLComputeCommandEncoder>)encoder waitForFence:metal_encoder_chain];
    else if([encoder conformsToProtocol:@protocol(MTLBlitCommandEncoder)])[(id<MTLBlitCommandEncoder>)encoder waitForFence:metal_encoder_chain];
}
inline void metal_chain_update(id<MTLCommandEncoder> encoder){
    if(!metal_encoder_chain || !encoder)return;
    if([encoder conformsToProtocol:@protocol(MTLRenderCommandEncoder)])[(id<MTLRenderCommandEncoder>)encoder updateFence:metal_encoder_chain afterStages:MTLRenderStageFragment];
    else if([encoder conformsToProtocol:@protocol(MTLComputeCommandEncoder)])[(id<MTLComputeCommandEncoder>)encoder updateFence:metal_encoder_chain];
    else if([encoder conformsToProtocol:@protocol(MTLBlitCommandEncoder)])[(id<MTLBlitCommandEncoder>)encoder updateFence:metal_encoder_chain];
}
// Metal 同一個 command buffer 同時只能有一個 encoder。資源配置、
// 參數驗證及綁定皆可能丟出例外，必須在解開堆疊時一併結束 encoding。
class MetalEncoderScope final {
    id<MTLCommandEncoder> encoder_;
public:
    explicit MetalEncoderScope(id<MTLCommandEncoder> encoder):encoder_(encoder) {
        if (!encoder_) throw std::runtime_error("Metal command encoder 建立失敗");
        metal_chain_wait(encoder_);
    }
    MetalEncoderScope(const MetalEncoderScope&)=delete;
    MetalEncoderScope& operator=(const MetalEncoderScope&)=delete;
    ~MetalEncoderScope(){end();}
    void end() noexcept {if(encoder_){metal_chain_update(encoder_);[encoder_ endEncoding];encoder_=nil;}}
};
}
