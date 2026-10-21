#include "x64arm/video_codec.hpp"
#include "video_metal.hpp"
#include "x64arm/gpu_memory.hpp"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>

namespace x64arm::video {
VideoCodecError::VideoCodecError(std::int32_t status, const std::string& operation)
    : std::runtime_error(operation + "，OSStatus=" + std::to_string(status)), status_(status) {}
bool hardware_decode_supported(Codec codec) noexcept {
    return codec == Codec::H264 && VTIsHardwareDecodeSupported(kCMVideoCodecType_H264);
}
namespace {
void check(OSStatus status, const char* operation) {
    if (status != noErr) throw VideoCodecError(status, operation);
}
template<class T> struct Ref {
    T value{};
    Ref() = default;
    ~Ref() { if (value) CFRelease(value); }
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
};
struct DecoderSession {
    VTDecompressionSessionRef value{};
    ~DecoderSession() {
        if (value) { VTDecompressionSessionInvalidate(value); CFRelease(value); }
    }
};
CMTime time(Timestamp stamp, bool duration) {
    if (stamp.timescale < 0 || (duration && stamp.timescale && stamp.value < 0))
        throw std::invalid_argument("媒體時間或 duration 不合法");
    return stamp.timescale ? CMTimeMake(stamp.value, stamp.timescale) : kCMTimeInvalid;
}
Timestamp time(CMTime stamp) {
    return CMTIME_IS_NUMERIC(stamp) && stamp.timescale > 0 ? Timestamp{stamp.value, stamp.timescale} : Timestamp{};
}
// 只讀取宣告範圍所需的 SPS 欄位，不以硬體的自動像素轉換掩蓋 10-bit/4:4:4 輸入。
void validate_sps(std::span<const std::uint8_t> nal) {
    if (nal.size() < 4 || (nal.front() & 0x80U) || (nal.front() & 31U) != 7)
        throw std::invalid_argument("H.264 SPS 標頭不合法");
    std::vector<std::uint8_t> rbsp;
    unsigned zeros = 0;
    for (const auto byte : nal.subspan(1)) {
        if (zeros == 2 && byte == 3) { zeros = 0; continue; }
        rbsp.push_back(byte); zeros = byte == 0 ? std::min(zeros + 1, 2U) : 0;
    }
    std::size_t cursor = 0;
    const auto bits = [&](unsigned count) {
        if (count > 32 || cursor + count > rbsp.size() * 8) throw std::invalid_argument("H.264 SPS 截斷");
        std::uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i, ++cursor) result = (result << 1) | ((rbsp[cursor / 8] >> (7 - cursor % 8)) & 1U);
        return result;
    };
    const auto ue = [&] {
        unsigned leading = 0;
        while (!bits(1)) if (++leading > 30) throw std::invalid_argument("H.264 Exp-Golomb 過大");
        return ((UINT32_C(1) << leading) - 1) + bits(leading);
    };
    const auto profile = bits(8); (void)bits(8); (void)bits(8); (void)ue();
    switch (profile) {
    case 66: case 77: case 88: break;
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135:
        if (ue() != 1 || ue() != 0 || ue() != 0) throw std::invalid_argument("硬體解碼目前只支援 H.264 8-bit 4:2:0");
        break;
    default: throw std::invalid_argument("尚未支援此 H.264 profile");
    }
}
void validate_config(const DecoderConfig& config) {
    if (config.codec != Codec::H264 || (config.nal_length_size != 1 && config.nal_length_size != 2 && config.nal_length_size != 4))
        throw std::invalid_argument("不支援的 codec 或 NAL 長度欄位");
    if (config.parameter_sets.size() < 2 || config.parameter_sets.size() > 32 || !config.max_packet_bytes || !config.max_frame_bytes || !config.max_pending_frames)
        throw std::invalid_argument("硬體解碼設定或容量上限不合法");
    bool sps = false, pps = false;
    for (const auto& nal : config.parameter_sets) {
        if (nal.empty() || nal.size() > 65535 || (nal.front() & 0x80U)) throw std::invalid_argument("H.264 parameter set 不合法");
        const auto type = nal.front() & 31U;
        if (type == 7) { validate_sps(nal); sps = true; }
        else if (type == 8) pps = true;
        else throw std::invalid_argument("硬體解碼設定只接受 SPS/PPS NAL");
    }
    if (!sps || !pps) throw std::invalid_argument("H.264 解碼需要 SPS 與 PPS");
}
void validate_packet(std::span<const std::uint8_t> packet, const DecoderConfig& config) {
    if (packet.empty() || packet.size() > config.max_packet_bytes) throw std::invalid_argument("H.264 packet 為空或超過容量上限");
    bool picture = false;
    for (std::size_t cursor = 0, count = 0; cursor < packet.size(); ++count) {
        if (count >= 1024 || packet.size() - cursor < config.nal_length_size) throw std::invalid_argument("H.264 packet NAL 表截斷或過多");
        std::uint32_t length = 0;
        for (unsigned i = 0; i < config.nal_length_size; ++i) length = (length << 8) | packet[cursor++];
        if (!length || length > packet.size() - cursor || (packet[cursor] & 0x80U)) throw std::invalid_argument("H.264 packet NAL 長度或標頭不合法");
        const auto type = packet[cursor] & 31U;
        if (!type || type >= 24) throw std::invalid_argument("尚未支援此 H.264 NAL 類型");
        if (type == 7 || type == 8) {
            const auto parameter = packet.subspan(cursor, length);
            if (type == 7) validate_sps(parameter);
            // 此 instance 的參數集固定；動態更換須由上層在 IDR 建立新 session。
            if (std::none_of(config.parameter_sets.begin(), config.parameter_sets.end(), [&](const auto& configured) {
                return configured.size() == parameter.size() && std::equal(configured.begin(), configured.end(), parameter.begin());
            })) throw std::invalid_argument("串流內參數集與硬體解碼設定不符");
        }
        picture |= type == 1 || type == 5;
        cursor += length;
    }
    if (!picture) throw std::invalid_argument("H.264 packet 必須含有一個存取單元的 VCL NAL");
}
struct PixelLock {
    CVPixelBufferRef value;
    explicit PixelLock(CVPixelBufferRef pixel) : value(pixel) { check(CVPixelBufferLockBaseAddress(value, kCVPixelBufferLock_ReadOnly), "鎖定解碼 pixel buffer 失敗"); }
    ~PixelLock() { (void)CVPixelBufferUnlockBaseAddress(value, kCVPixelBufferLock_ReadOnly); }
};
}
namespace {
class PixelFrame final:public NativeFrame {
public:
    CVPixelBufferRef image{};
    std::uint32_t width{},height{};
    std::shared_ptr<GpuMemoryReservation> lease;
    PixelFrame(CVPixelBufferRef source,std::uint32_t w,std::uint32_t h,bool retained):width(w),height(h){
        if(retained)lease=reserve_gpu_memory(CVPixelBufferGetDataSize(source));image=CVPixelBufferRetain(source);
    }
    ~PixelFrame() override {if(image)CVPixelBufferRelease(image);}
    std::vector<std::uint8_t> copy_nv12()const override {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width)*height*3/2);PixelLock lock(image);std::size_t offset=0;
        for(std::size_t plane=0;plane<2;++plane){
            const auto rows=plane?height/2:height;const auto stride=CVPixelBufferGetBytesPerRowOfPlane(image,plane);
            const auto* source=static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(image,plane));
            if(!source || stride<width || CVPixelBufferGetHeightOfPlane(image,plane)<rows)throw VideoCodecError(kVTVideoDecoderBadDataErr,"解碼 plane 範圍不合法");
            for(std::uint32_t row=0;row<rows;++row){std::memcpy(pixels.data()+offset,source+row*stride,width);offset+=width;}
        }
        return pixels;
    }
};
}
struct MetalVideoFrame::State {
    std::shared_ptr<const PixelFrame> frame;
    CVMetalTextureCacheRef cache{};std::array<CVMetalTextureRef,2> planes{};
    ~State(){for(auto plane:planes)if(plane)CFRelease(plane);if(cache)CFRelease(cache);}
};
MetalVideoFrame::MetalVideoFrame(const DecodedFrame& frame,id<MTLDevice> device):state_(std::make_shared<State>()){
    state_->frame=std::dynamic_pointer_cast<const PixelFrame>(frame.native);
    if(!device || !state_->frame)throw std::invalid_argument("Metal 影片需要保留的原生畫面與裝置");
    check(CVMetalTextureCacheCreate(kCFAllocatorDefault,nullptr,device,nullptr,&state_->cache),"建立影片 Metal texture cache 失敗");
    for(unsigned plane=0;plane<2;++plane){
        const auto image=state_->frame->image;
        check(CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,state_->cache,image,nullptr,plane?MTLPixelFormatRG8Unorm:MTLPixelFormatR8Unorm,
            CVPixelBufferGetWidthOfPlane(image,plane),CVPixelBufferGetHeightOfPlane(image,plane),plane,&state_->planes[plane]),"建立影片 Metal plane 失敗");
        if(!CVMetalTextureGetTexture(state_->planes[plane]))throw VideoCodecError(kCVReturnError,"影片 Metal plane 不存在");
    }
}
id<MTLTexture> MetalVideoFrame::luma()const{return CVMetalTextureGetTexture(state_->planes[0]);}
id<MTLTexture> MetalVideoFrame::chroma()const{return CVMetalTextureGetTexture(state_->planes[1]);}
struct HardwareDecoder::State {
    DecoderConfig config;
    Ref<CMVideoFormatDescriptionRef> format;
    DecoderSession session;
    std::uint32_t width{}, height{};
    bool hardware{}, drained{};
    std::mutex mutex;
    std::vector<DecodedFrame> outputs;
    std::exception_ptr callback_error;
    struct Submission { std::uint64_t tag; };
    explicit State(const DecoderConfig& requested) : config(requested) {
        validate_config(config);
        std::vector<const std::uint8_t*> pointers;
        std::vector<std::size_t> sizes;
        for (const auto& nal : config.parameter_sets) { pointers.push_back(nal.data()); sizes.push_back(nal.size()); }
        check(CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, pointers.size(), pointers.data(), sizes.data(), static_cast<int>(config.nal_length_size), &format.value), "建立 H.264 format description 失敗");
        const auto dimensions = CMVideoFormatDescriptionGetDimensions(format.value);
        if (dimensions.width <= 0 || dimensions.height <= 0 || (dimensions.width & 1) || (dimensions.height & 1))
            throw std::invalid_argument("NV12 解碼需要正偶數影像尺寸");
        width = static_cast<std::uint32_t>(dimensions.width); height = static_cast<std::uint32_t>(dimensions.height);
        const auto bytes = std::uint64_t{width} * height * 3 / 2;
        if (bytes > config.max_frame_bytes) throw std::invalid_argument("解碼影像超過容量上限");
        Ref<CFMutableDictionaryRef> specification, attributes;
        specification.value = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        attributes.value = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!specification.value || !attributes.value) throw std::bad_alloc();
        CFDictionarySetValue(specification.value, kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder, kCFBooleanTrue);
        const std::int32_t pixel_format = static_cast<std::int32_t>(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
        Ref<CFNumberRef> pixel;
        pixel.value = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);
        if (!pixel.value) throw std::bad_alloc();
        CFDictionarySetValue(attributes.value, kCVPixelBufferPixelFormatTypeKey, pixel.value);
        CFDictionarySetValue(attributes.value,kCVPixelBufferMetalCompatibilityKey,kCFBooleanTrue);
        Ref<CFDictionaryRef> surface_attributes;
        surface_attributes.value=CFDictionaryCreate(kCFAllocatorDefault,nullptr,nullptr,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        if(!surface_attributes.value)throw std::bad_alloc();
        CFDictionarySetValue(attributes.value,kCVPixelBufferIOSurfacePropertiesKey,surface_attributes.value);
        VTDecompressionOutputCallbackRecord callback{decoded, this};
        check(VTDecompressionSessionCreate(kCFAllocatorDefault, format.value, specification.value, attributes.value, &callback, &session.value), "建立硬體解碼 session 失敗");
        Ref<CFTypeRef> actual;
        check(VTSessionCopyProperty(session.value, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, kCFAllocatorDefault, &actual.value), "查詢實際硬體解碼狀態失敗");
        hardware = actual.value && CFGetTypeID(actual.value) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(actual.value));
        if (!hardware) throw VideoCodecError(kVTVideoDecoderNotAvailableNowErr, "未取得硬體解碼器");
    }
    static void decoded(void* opaque, void* reference, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime pts, CMTime duration) noexcept {
        auto& self = *static_cast<State*>(opaque);
        try {
            check(status, "硬體解碼 callback 失敗");
            if (!image || (flags & kVTDecodeInfo_FrameDropped)) throw VideoCodecError(kVTVideoDecoderBadDataErr, "硬體解碼未回傳畫面");
            const auto pixel_format = CVPixelBufferGetPixelFormatType(image);
            if (pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)
                throw VideoCodecError(kVTVideoDecoderUnsupportedDataFormatErr, "解碼輸出不是 NV12");
            if (CVPixelBufferGetWidth(image) != self.width || CVPixelBufferGetHeight(image) != self.height || CVPixelBufferGetPlaneCount(image) != 2)
                throw VideoCodecError(kVTVideoDecoderBadDataErr, "解碼輸出尺寸或 plane 數不符");
            DecodedFrame frame;
            frame.width = self.width; frame.height = self.height; frame.pts = time(pts); frame.duration = time(duration);
            frame.tag = static_cast<Submission*>(reference)->tag;
            frame.full_range = pixel_format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
            auto native=std::make_shared<PixelFrame>(image,self.width,self.height,self.config.retain_native_frames);
            if(self.config.retain_native_frames)frame.native=std::move(native);
            else frame.nv12=native->copy_nv12();
            std::lock_guard guard(self.mutex);
            if (self.outputs.size() >= self.config.max_pending_frames) throw std::length_error("硬體解碼輸出佇列已滿");
            self.outputs.push_back(std::move(frame));
        } catch (...) {
            std::lock_guard guard(self.mutex);
            if (!self.callback_error) self.callback_error = std::current_exception();
        }
    }
    std::vector<DecodedFrame> take_outputs() {
        std::lock_guard guard(mutex);
        if (callback_error) { const auto error = std::exchange(callback_error, {}); outputs.clear(); std::rethrow_exception(error); }
        return std::exchange(outputs, {});
    }
};
HardwareDecoder::HardwareDecoder(const DecoderConfig& config) : state_(std::make_unique<State>(config)) {}
HardwareDecoder::~HardwareDecoder() = default;
HardwareDecoder::HardwareDecoder(HardwareDecoder&&) noexcept = default;
HardwareDecoder& HardwareDecoder::operator=(HardwareDecoder&&) noexcept = default;
std::vector<DecodedFrame> HardwareDecoder::decode(std::span<const std::uint8_t> packet, Timestamp pts, Timestamp duration, std::uint64_t tag) {
    if (!state_) throw std::logic_error("硬體解碼器已移轉");
    if (state_->drained) throw std::logic_error("drain 後須先 flush 才能提交新的畫面");
    validate_packet(packet, state_->config);
    CMSampleTimingInfo timing{time(duration, true), time(pts, false), kCMTimeInvalid};
    Ref<CMBlockBufferRef> block;
    check(CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, packet.size(), kCFAllocatorDefault, nullptr, 0, packet.size(), 0, &block.value), "配置壓縮畫面 buffer 失敗");
    check(CMBlockBufferReplaceDataBytes(packet.data(), block.value, 0, packet.size()), "複製壓縮畫面失敗");
    Ref<CMSampleBufferRef> sample;
    const auto size = packet.size();
    check(CMSampleBufferCreateReady(kCFAllocatorDefault, block.value, state_->format.value, 1, 1, &timing, 1, &size, &sample.value), "建立壓縮 sample 失敗");
    State::Submission submission{tag};
    // 同步模式保證 callback 在 DecodeFrame 返回前完成，stack refcon 不跨呼叫存活。
    const auto status = VTDecompressionSessionDecodeFrame(state_->session.value, sample.value, 0, &submission, nullptr);
    check(status, "提交硬體解碼失敗");
    return state_->take_outputs();
}
std::vector<DecodedFrame> HardwareDecoder::drain() {
    if (!state_) throw std::logic_error("硬體解碼器已移轉");
    if (state_->drained) return {};
    check(VTDecompressionSessionFinishDelayedFrames(state_->session.value), "送出延遲解碼畫面失敗");
    check(VTDecompressionSessionWaitForAsynchronousFrames(state_->session.value), "等待解碼結束失敗");
    state_->drained = true;
    return state_->take_outputs();
}
void HardwareDecoder::flush() {
    if (!state_) throw std::logic_error("硬體解碼器已移轉");
    auto replacement = std::make_unique<State>(state_->config);
    state_ = std::move(replacement);
}
bool HardwareDecoder::hardware_accelerated() const noexcept { return state_ && state_->hardware; }
std::uint32_t HardwareDecoder::width() const { if (!state_) throw std::logic_error("硬體解碼器已移轉"); return state_->width; }
std::uint32_t HardwareDecoder::height() const { if (!state_) throw std::logic_error("硬體解碼器已移轉"); return state_->height; }
#include "video_encoder.inc"
}
