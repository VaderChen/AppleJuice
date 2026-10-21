#pragma once
#import <Metal/Metal.h>
#include <stdexcept>

namespace x64arm {
// Metal 同一個 command buffer 同時只能有一個 encoder。資源配置、
// 參數驗證及綁定皆可能丟出例外，必須在解開堆疊時一併結束 encoding。
class MetalEncoderScope final {
    id<MTLCommandEncoder> encoder_;
public:
    explicit MetalEncoderScope(id<MTLCommandEncoder> encoder):encoder_(encoder) {
        if (!encoder_) throw std::runtime_error("Metal command encoder 建立失敗");
    }
    MetalEncoderScope(const MetalEncoderScope&)=delete;
    MetalEncoderScope& operator=(const MetalEncoderScope&)=delete;
    ~MetalEncoderScope(){end();}
    void end() noexcept {if(encoder_){[encoder_ endEncoding];encoder_=nil;}}
};
}
