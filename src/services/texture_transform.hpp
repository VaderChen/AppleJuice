#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace x64arm {struct GraphicsResourceDesc;}
namespace x64arm::texture_image {
// 由共用 DDS 解碼器保留 mip／array／cube／volume，並展開舊式像素格式。
void decode_dds(std::span<const std::uint8_t> source,GraphicsResourceDesc& desc,
    std::vector<std::vector<std::uint8_t>>& pixels);
// 輸入為緊密排列的單一 mip／array element；box 為左上前／右下後。
std::vector<std::uint8_t> transform(std::span<const std::uint8_t> source,
    std::uint32_t source_format, std::array<std::uint32_t,3> source_size,
    std::array<std::uint32_t,6> source_box, std::uint32_t target_format,
    std::array<std::uint32_t,3> target_size, std::uint32_t filter);
// 高度圖以 DirectXTex 共用中央差分生成法線，旗標使用 D3DX9 契約。
std::vector<std::uint8_t> normal_map(std::span<const std::uint8_t> source,
    std::uint32_t source_format,std::uint32_t width,std::uint32_t height,
    std::uint32_t target_format,std::uint32_t flags,std::uint32_t channel,float amplitude);
}
