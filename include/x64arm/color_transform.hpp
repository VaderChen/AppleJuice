#pragma once
// 宿主原生色彩轉換（macOS 為 ColorSync）：以 ICC 設定檔序列建立轉換，轉換交錯排列的 32 位元浮點通道（裝置值 0..1）。
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace x64arm {
struct NativeColorTransform;
// profiles 依序為來源裝置、（模擬用的）中間裝置與目的裝置；中間設定檔先轉成裝置值再回到 PCS，以重現其色域。
// intents 依 ICC 0 感知、1 相對色度、2 飽和度、3 絕對色度，少於段數時沿用最後一個。無法建立時回 nullptr。
std::shared_ptr<NativeColorTransform> native_color_transform(const std::vector<std::vector<std::uint8_t>>& profiles,const std::vector<std::uint32_t>& intents);
// count 個像素；extended 允許輸出超出 0..1（色域檢查用）。
bool native_color_convert(const NativeColorTransform& transform,std::size_t count,unsigned input_channels,const float* input,unsigned output_channels,float* output,bool extended);
}
