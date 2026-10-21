#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace x64arm::video {
enum class Codec { H264 };
bool hardware_decode_supported(Codec codec) noexcept; // 僅能力查詢；實際 session 仍須確認硬體使用狀態。
bool hardware_encode_supported(Codec codec) noexcept;
struct Timestamp {
    std::int64_t value{};
    std::int32_t timescale{}; // 0 表示時間未知；有效時間的 timescale 必須大於零。
    bool operator==(const Timestamp&) const = default;
};
struct DecoderConfig {
    Codec codec{Codec::H264};
    std::vector<std::vector<std::uint8_t>> parameter_sets; // 原始 SPS/PPS NAL，沒有 start code。
    unsigned nal_length_size{4};
    std::size_t max_packet_bytes{16U * 1024U * 1024U};
    std::size_t max_frame_bytes{256U * 1024U * 1024U};
    std::size_t max_pending_frames{32};
    bool retain_native_frames{}; // GPU／延遲 CPU 消費保留原生 surface；預設維持即時 NV12 回讀。
};
struct EncoderConfig {
    Codec codec{Codec::H264};
    std::uint32_t width{}, height{}, bitrate{4000000};
    std::uint32_t frame_rate_numerator{30}, frame_rate_denominator{1};
    unsigned profile{66}, keyframe_interval{60}; // H.264 Baseline／Main／High：66／77／100。
    std::size_t max_frame_bytes{256U * 1024U * 1024U}, max_packet_bytes{16U * 1024U * 1024U};
};
struct EncodedPacket {
    std::vector<std::uint8_t> avcc; // 4-byte 大端序 NAL 長度，壓縮內容不含 start code。
    std::vector<std::vector<std::uint8_t>> parameter_sets;
    Timestamp pts{}, dts{}, duration{};
    std::uint64_t tag{};
    bool keyframe{};
};
class HardwareEncoder {
public:
    explicit HardwareEncoder(const EncoderConfig& config);
    ~HardwareEncoder();
    HardwareEncoder(HardwareEncoder&&) noexcept;
    HardwareEncoder& operator=(HardwareEncoder&&) noexcept;
    HardwareEncoder(const HardwareEncoder&) = delete;
    HardwareEncoder& operator=(const HardwareEncoder&) = delete;
    // 緊密 NV12、video range；由單一呼叫端序列化。同步完成該幀，適合在共用媒體工作執行緒使用。
    // 目前不重排序 B-frame；PTS 必須有效且嚴格遞增，duration 必須為正。
    std::vector<EncodedPacket> encode(std::span<const std::uint8_t> nv12, Timestamp pts, Timestamp duration, std::uint64_t tag = 0, bool force_keyframe = false);
    std::vector<EncodedPacket> drain();
    void flush();
    bool hardware_accelerated() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
class NativeFrame {
public:
    virtual ~NativeFrame()=default;
    virtual std::vector<std::uint8_t> copy_nv12() const=0;
};
struct DecodedFrame {
    std::uint32_t width{}, height{};
    Timestamp pts{}, duration{};
    std::uint64_t tag{};
    bool full_range{};
    // 緊密 NV12：width*height bytes 的 Y，接續 width*height/2 bytes 的交錯 UV。
    // 資料由呼叫者持有，不引用 VideoToolbox 或客體記憶體。
    std::vector<std::uint8_t> nv12;
    std::shared_ptr<const NativeFrame> native;
    // 由呼叫端序列化；複製的 frame 共享唯讀 surface，各自持有 CPU 像素。
    std::span<const std::uint8_t> materialize(){
        if(nv12.empty() && native)nv12=native->copy_nv12();
        return nv12;
    }
};
class VideoCodecError : public std::runtime_error {
public:
    VideoCodecError(std::int32_t status, const std::string& operation);
    std::int32_t status() const noexcept { return status_; }
private:
    std::int32_t status_;
};
// 每個 instance 由單一呼叫端序列化操作。硬體不可用會失敗，絕不退回軟體解碼。
// 客體 ABI、COM 物件與 Windows HRESULT 轉換由上層服務處理。
class HardwareDecoder {
public:
    explicit HardwareDecoder(const DecoderConfig& config);
    ~HardwareDecoder();
    HardwareDecoder(HardwareDecoder&&) noexcept;
    HardwareDecoder& operator=(HardwareDecoder&&) noexcept;
    HardwareDecoder(const HardwareDecoder&) = delete;
    HardwareDecoder& operator=(const HardwareDecoder&) = delete;
    // packet 中的 SPS/PPS 若存在，必須與 config 相同；重配置由呼叫者在 IDR 建立新 instance。
    std::vector<DecodedFrame> decode(std::span<const std::uint8_t> packet, Timestamp pts = {}, Timestamp duration = {}, std::uint64_t tag = 0);
    std::vector<DecodedFrame> drain();
    void flush(); // 丟棄舊參考畫面，重建相同設定的硬體 session。
    bool hardware_accelerated() const noexcept;
    std::uint32_t width() const;
    std::uint32_t height() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
