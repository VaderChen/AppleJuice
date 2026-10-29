#pragma once
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <array>
#include <cstdint>
#include <stdexcept>

namespace x64arm {
// D3D12 描述元表（bindless）經 argument buffer 存取的資源不在 Metal 自動危害追蹤內，D3D12 屏障也不經過 Metal。
// 使用過描述元表後，編碼執行緒的每個 encoder 先等待前一個 encoder 更新的 fence、結束時再更新，
// 依提交順序串接（D3D12 資源屏障的保守對應）。由 MetalGl 在開始編碼時設定；其他執行緒（呈現）不受影響。
inline thread_local __unsafe_unretained id<MTLFence> metal_encoder_chain=nil;
// 編碼器種類以類別快取：conformsToProtocol 逐一走訪協定清單（取樣約占命令執行緒 3.6%），每個 encoder 開始與結束各要判斷。
// 各種類只有少數實作類別（依裝置而定），記住最近見過的類別，未見過才查協定。
enum class MetalEncoderKind:std::uint8_t{other,render,compute,blit};
inline MetalEncoderKind metal_encoder_kind(id<MTLCommandEncoder> encoder){
    struct Seen{__unsafe_unretained Class type;MetalEncoderKind kind;};
    static thread_local std::array<Seen,8> seen{};static thread_local unsigned next=0;
    const Class type=object_getClass(encoder);
    for(const auto& entry:seen)if(entry.type==type)return entry.kind;
    const auto kind=[encoder conformsToProtocol:@protocol(MTLRenderCommandEncoder)]?MetalEncoderKind::render:
        [encoder conformsToProtocol:@protocol(MTLComputeCommandEncoder)]?MetalEncoderKind::compute:
        [encoder conformsToProtocol:@protocol(MTLBlitCommandEncoder)]?MetalEncoderKind::blit:MetalEncoderKind::other;
    seen[next++%seen.size()]={type,kind};return kind;
}
inline void metal_chain_wait(id<MTLCommandEncoder> encoder){
    if(!metal_encoder_chain || !encoder)return;
    switch(metal_encoder_kind(encoder)){
    case MetalEncoderKind::render:[(id<MTLRenderCommandEncoder>)encoder waitForFence:metal_encoder_chain beforeStages:MTLRenderStageVertex];break;
    case MetalEncoderKind::compute:[(id<MTLComputeCommandEncoder>)encoder waitForFence:metal_encoder_chain];break;
    case MetalEncoderKind::blit:[(id<MTLBlitCommandEncoder>)encoder waitForFence:metal_encoder_chain];break;
    case MetalEncoderKind::other:break;
    }
}
inline void metal_chain_update(id<MTLCommandEncoder> encoder){
    if(!metal_encoder_chain || !encoder)return;
    switch(metal_encoder_kind(encoder)){
    case MetalEncoderKind::render:[(id<MTLRenderCommandEncoder>)encoder updateFence:metal_encoder_chain afterStages:MTLRenderStageFragment];break;
    case MetalEncoderKind::compute:[(id<MTLComputeCommandEncoder>)encoder updateFence:metal_encoder_chain];break;
    case MetalEncoderKind::blit:[(id<MTLBlitCommandEncoder>)encoder updateFence:metal_encoder_chain];break;
    case MetalEncoderKind::other:break;
    }
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
