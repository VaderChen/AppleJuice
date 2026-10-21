#include "x64arm/video_codec.hpp"

namespace x64arm::video {
VideoCodecError::VideoCodecError(std::int32_t status, const std::string& operation)
    : std::runtime_error(operation + "，平台狀態=" + std::to_string(status)), status_(status) {}
bool hardware_decode_supported(Codec) noexcept { return false; }
bool hardware_encode_supported(Codec) noexcept { return false; }
struct HardwareEncoder::State {};
struct HardwareDecoder::State {};
namespace {
[[noreturn]] void unavailable() { throw VideoCodecError(-1, "此平台尚未提供硬體影片解碼後端"); }
}
HardwareDecoder::HardwareDecoder(const DecoderConfig&) { unavailable(); }
HardwareEncoder::HardwareEncoder(const EncoderConfig&) { unavailable(); }
HardwareEncoder::~HardwareEncoder() = default;
HardwareEncoder::HardwareEncoder(HardwareEncoder&&) noexcept = default;
HardwareEncoder& HardwareEncoder::operator=(HardwareEncoder&&) noexcept = default;
std::vector<EncodedPacket> HardwareEncoder::encode(std::span<const std::uint8_t>, Timestamp, Timestamp, std::uint64_t, bool) { unavailable(); }
std::vector<EncodedPacket> HardwareEncoder::drain() { unavailable(); }
void HardwareEncoder::flush() { unavailable(); }
bool HardwareEncoder::hardware_accelerated() const noexcept { return false; }
HardwareDecoder::~HardwareDecoder() = default;
HardwareDecoder::HardwareDecoder(HardwareDecoder&&) noexcept = default;
HardwareDecoder& HardwareDecoder::operator=(HardwareDecoder&&) noexcept = default;
std::vector<DecodedFrame> HardwareDecoder::decode(std::span<const std::uint8_t>, Timestamp, Timestamp, std::uint64_t) { unavailable(); }
std::vector<DecodedFrame> HardwareDecoder::drain() { unavailable(); }
void HardwareDecoder::flush() { unavailable(); }
bool HardwareDecoder::hardware_accelerated() const noexcept { return false; }
std::uint32_t HardwareDecoder::width() const { unavailable(); }
std::uint32_t HardwareDecoder::height() const { unavailable(); }
}
