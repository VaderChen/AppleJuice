#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace x64arm::video {

// Windows SDK / MinGW dxva.h 使用 pack(1)；以 bytes 解讀，不依賴宿主結構 ABI。
inline constexpr std::size_t dxva_h264_picture_bytes = 1040;
inline constexpr std::size_t dxva_h264_matrix_bytes = 224;
inline constexpr std::size_t dxva_h264_short_slice_bytes = 10;

class DxvaH264Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct H264ReferencePicture {
    std::uint8_t surface{};
    bool long_term{};
    std::uint16_t frame_num_or_long_term_index{};
    std::array<std::int32_t, 2> field_order_count{};
};

struct DxvaH264Packet {
    // raw SPS/PPS NAL（包含 NAL header，不含 start code）；sample 使用 4-byte BE 長度。
    std::vector<std::vector<std::uint8_t>> parameter_sets;
    std::vector<std::uint8_t> sample;
    std::uint32_t coded_width{}, coded_height{}, picture_parameter_set_id{};
    std::uint8_t target_surface{};
    std::uint16_t frame_num{};
    std::array<std::int32_t, 2> field_order_count{};
    bool idr{}, reference_picture{};
    std::vector<H264ReferencePicture> references;
};

// 僅支援 ConfigBitstreamRaw=2 的完整 short slices、8-bit 4:2:0 progressive、
// DXVA H.264 VLD 的 constrained-baseline/Main/High，不支援 profile mismatch。
// POC type 0/2、無 FMO/ASO。POC type1 缺少 DXVA 未傳遞的 cycle offsets，明確拒絕。
// 參數集描述 coded rectangle；不捏造原串流的 crop/VUI/color metadata。
// DXVA 不提供原 profile/level：合成 High profile，以 MinLumaBipredSize8x8Flag
// 選 level 3.0 或 5.2 並驗證該級別的 frame/DPB 尺寸；此欄位影響解碼語意。
// PPS id 從 slice 解析保留；MMCO、reference-list、weights 及 entropy payload 原封保留。
// 接收端必須保持同一序列的解碼 session/DPB，依 references 驗證 surface 生命週期，
// flush 後從 IDR 恢復；此無狀態轉換函式不保證引用畫面已在接收端解碼。
[[nodiscard]] DxvaH264Packet convert_dxva_h264(
    std::span<const std::uint8_t> picture_parameters,
    std::span<const std::uint8_t> quantization_matrix,
    std::span<const std::uint8_t> short_slice_control,
    std::span<const std::uint8_t> annex_b_bitstream);

} // namespace x64arm::video
