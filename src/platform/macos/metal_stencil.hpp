#pragma once
#import <Metal/Metal.h>
#include <cstdint>
namespace x64arm {
// OpenGL 與 D3D 共用 Metal stencil 描述子；各 API 只轉換自己的 enum／mask。
inline MTLStencilDescriptor* make_metal_stencil(MTLCompareFunction compare,MTLStencilOperation fail,MTLStencilOperation depth_fail,MTLStencilOperation pass,std::uint32_t read_mask,std::uint32_t write_mask){
    auto stencil=[MTLStencilDescriptor new];stencil.stencilCompareFunction=compare;
    stencil.stencilFailureOperation=fail;stencil.depthFailureOperation=depth_fail;stencil.depthStencilPassOperation=pass;
    stencil.readMask=read_mask;stencil.writeMask=write_mask;return stencil;
}
}
