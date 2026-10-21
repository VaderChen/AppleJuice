#pragma once
#import <Metal/Metal.h>
namespace x64arm {
// 相同固定 MSL 同時供離線建置與無 Metal toolchain 時的可攜回退使用。
// force_source 僅供 library／pipeline 分開量測，正常執行使用內嵌 metallib。
id<MTLLibrary> fixed_metal_library(id<MTLDevice>,NSError**,bool force_source=false);
bool fixed_metal_offline_available() noexcept;
}
