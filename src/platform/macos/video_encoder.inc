// VideoToolbox 共用編碼後端；客體 ABI 與 Annex B 包裝由媒體前端處理。
bool hardware_encode_supported(Codec codec) noexcept {
    if (codec != Codec::H264) return false;
    Ref<CFArrayRef> encoders;
    if (VTCopyVideoEncoderList(nullptr, &encoders.value) || !encoders.value) return false;
    for (CFIndex n = 0; n < CFArrayGetCount(encoders.value); ++n) {
        const auto row = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(encoders.value, n));
        if (!row || CFGetTypeID(row) != CFDictionaryGetTypeID()) continue;
        const auto kind = static_cast<CFNumberRef>(CFDictionaryGetValue(row, kVTVideoEncoderList_CodecType));
        const auto hardware = static_cast<CFBooleanRef>(CFDictionaryGetValue(row, kVTVideoEncoderList_IsHardwareAccelerated));
        std::int64_t value{};
        if (kind && CFGetTypeID(kind) == CFNumberGetTypeID() && CFNumberGetValue(kind, kCFNumberSInt64Type, &value) && value == kCMVideoCodecType_H264 &&
            hardware && CFGetTypeID(hardware) == CFBooleanGetTypeID() && CFBooleanGetValue(hardware)) return true;
    }
    return false;
}
namespace {
struct EncoderSession {
    VTCompressionSessionRef value{};
    void reset() { if (value) { VTCompressionSessionInvalidate(value); CFRelease(value); value = nullptr; } }
    ~EncoderSession() { reset(); }
};
void encoder_number(VTCompressionSessionRef session, CFStringRef key, double value) {
    Ref<CFNumberRef> number;
    number.value = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &value);
    if (!number.value) throw std::bad_alloc();
    check(VTSessionSetProperty(session, key, number.value), "設定硬體編碼參數失敗");
}
}
struct HardwareEncoder::State {
    EncoderConfig config;
    EncoderSession session;
    bool hardware{}, drained{};
    CMTime previous{kCMTimeInvalid};
    std::mutex mutex;
    std::vector<EncodedPacket> outputs;
    std::exception_ptr error;
    std::uint64_t completed_frames{};
    struct Submission { std::uint64_t tag; };
    explicit State(const EncoderConfig& requested) : config(requested) {
        if (config.codec != Codec::H264 || !config.width || !config.height || config.width > 8192 || config.height > 8192 ||
            (config.width & 1) || (config.height & 1) || !config.bitrate || !config.frame_rate_numerator || !config.frame_rate_denominator ||
            !config.keyframe_interval || (config.profile != 66 && config.profile != 77 && config.profile != 100) ||
            std::uint64_t{config.width} * config.height * 3 / 2 > config.max_frame_bytes || !config.max_packet_bytes)
            throw std::invalid_argument("硬體 H.264 編碼設定不合法");
        Ref<CFMutableDictionaryRef> specification, attributes;
        specification.value = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        attributes.value = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        Ref<CFDictionaryRef> surface;
        surface.value = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        const std::int32_t pixel_format = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        Ref<CFNumberRef> pixel;
        pixel.value = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixel_format);
        if (!specification.value || !attributes.value || !surface.value || !pixel.value) throw std::bad_alloc();
        CFDictionarySetValue(specification.value, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
        CFDictionarySetValue(attributes.value, kCVPixelBufferPixelFormatTypeKey, pixel.value);
        CFDictionarySetValue(attributes.value, kCVPixelBufferIOSurfacePropertiesKey, surface.value);
        check(VTCompressionSessionCreate(kCFAllocatorDefault, static_cast<std::int32_t>(config.width), static_cast<std::int32_t>(config.height),
            kCMVideoCodecType_H264, specification.value, attributes.value, nullptr, encoded, this, &session.value), "建立硬體編碼 session 失敗");
        auto profile = config.profile == 66 ? kVTProfileLevel_H264_Baseline_AutoLevel : config.profile == 77 ? kVTProfileLevel_H264_Main_AutoLevel : kVTProfileLevel_H264_High_AutoLevel;
        // Constrained Baseline 是 Baseline 的合法子集，可與 DXVA 的硬體 profile 契約互通。
        // 交由編碼器產生正確位元流，不改寫 SPS 宣告來冒充不同 profile。
        if(config.profile==66){if(@available(macOS 12.0,*))profile=kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel;}
        check(VTSessionSetProperty(session.value, kVTCompressionPropertyKey_ProfileLevel, profile), "設定 H.264 profile 失敗");
        check(VTSessionSetProperty(session.value, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse), "設定編碼順序失敗");
        check(VTSessionSetProperty(session.value, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse), "設定編碼即時性失敗");
        encoder_number(session.value, kVTCompressionPropertyKey_AverageBitRate, config.bitrate);
        encoder_number(session.value, kVTCompressionPropertyKey_ExpectedFrameRate, double(config.frame_rate_numerator) / config.frame_rate_denominator);
        encoder_number(session.value, kVTCompressionPropertyKey_MaxKeyFrameInterval, config.keyframe_interval);
        check(VTCompressionSessionPrepareToEncodeFrames(session.value), "準備硬體編碼失敗");
        Ref<CFTypeRef> actual;
        check(VTSessionCopyProperty(session.value, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, kCFAllocatorDefault, &actual.value), "查詢實際硬體編碼狀態失敗");
        hardware = actual.value && CFGetTypeID(actual.value) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(actual.value));
        if (!hardware) throw VideoCodecError(kVTVideoEncoderNotAvailableNowErr, "未取得硬體編碼器");
    }
    ~State() {
        session.reset(); // 在 callback 所需的 mutex／佇列銷毀前，先終止 VT。
        if(const auto* trace=std::getenv("X64ARM_VIDEO_CODEC_TRACE");trace && std::string_view(trace)=="1")
            std::fprintf(stderr,"[video] backend=VideoToolbox operation=encode codec=H264 hardware=%u width=%u height=%u profile=%u completed_frames=%llu\n",unsigned(hardware),config.width,config.height,config.profile,static_cast<unsigned long long>(completed_frames));
    }
    static void encoded(void* opaque, void* reference, OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample) noexcept {
        auto& self = *static_cast<State*>(opaque);
        try {
            check(status, "硬體編碼 callback 失敗");
            if (!sample || (flags & kVTEncodeInfo_FrameDropped) || !CMSampleBufferDataIsReady(sample))
                throw VideoCodecError(kVTVideoEncoderMalfunctionErr, "硬體編碼未回傳完整畫面");
            EncodedPacket packet;
            const auto block = CMSampleBufferGetDataBuffer(sample);
            const auto size = block ? CMBlockBufferGetDataLength(block) : 0;
            if (!size || size > self.config.max_packet_bytes) throw std::invalid_argument("硬體編碼輸出超過容量上限");
            packet.avcc.resize(size);
            check(CMBlockBufferCopyDataBytes(block, 0, size, packet.avcc.data()), "複製硬體編碼輸出失敗");
            const auto format = CMSampleBufferGetFormatDescription(sample);
            std::size_t count{}; int length{};
            check(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, nullptr, nullptr, &count, &length), "讀取編碼參數集數量失敗");
            if (length != 4 || count < 2 || count > 32) throw std::invalid_argument("硬體編碼 NAL 容器不合法");
            for (std::size_t n = 0; n < count; ++n) {
                const std::uint8_t* bytes{}; std::size_t parameter_size{};
                check(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, n, &bytes, &parameter_size, nullptr, nullptr), "讀取編碼參數集失敗");
                if (!bytes || !parameter_size || parameter_size > 65535) throw std::invalid_argument("硬體編碼參數集範圍不合法");
                packet.parameter_sets.emplace_back(bytes, bytes + parameter_size);
            }
            packet.pts = time(CMSampleBufferGetPresentationTimeStamp(sample));
            packet.dts = time(CMSampleBufferGetDecodeTimeStamp(sample));
            if (!packet.dts.timescale) packet.dts = packet.pts; // 不重排序時 DTS 與 PTS 相同。
            packet.duration = time(CMSampleBufferGetDuration(sample));
            packet.tag = reference ? static_cast<Submission*>(reference)->tag : 0;
            packet.keyframe = true;
            const auto attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
            if (attachments && CFArrayGetCount(attachments)) {
                const auto first = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
                packet.keyframe = CFDictionaryGetValue(first, kCMSampleAttachmentKey_NotSync) != kCFBooleanTrue;
            }
            std::lock_guard lock(self.mutex);
            if (self.outputs.size() >= 32) throw std::invalid_argument("硬體編碼輸出佇列已滿");
            self.outputs.push_back(std::move(packet));
            ++self.completed_frames;
        } catch (...) { std::lock_guard lock(self.mutex); self.error = std::current_exception(); }
    }
    std::vector<EncodedPacket> take() {
        std::lock_guard lock(mutex);
        if (error) { auto failure = std::exchange(error, {}); outputs.clear(); std::rethrow_exception(failure); }
        return std::exchange(outputs, {});
    }
};
HardwareEncoder::HardwareEncoder(const EncoderConfig& config) : state_(std::make_unique<State>(config)) {}
HardwareEncoder::~HardwareEncoder() = default;
HardwareEncoder::HardwareEncoder(HardwareEncoder&&) noexcept = default;
HardwareEncoder& HardwareEncoder::operator=(HardwareEncoder&&) noexcept = default;
std::vector<EncodedPacket> HardwareEncoder::encode(std::span<const std::uint8_t> nv12, Timestamp pts, Timestamp duration, std::uint64_t tag, bool force_keyframe) {
    if (!state_ || !state_->session.value || state_->drained) throw std::logic_error("硬體編碼器已移轉、失效或已 drain");
    const auto presentation = time(pts, false), interval = time(duration, true);
    if (!CMTIME_IS_NUMERIC(presentation) || !CMTIME_IS_NUMERIC(interval) || duration.value <= 0 ||
        (CMTIME_IS_NUMERIC(state_->previous) && CMTimeCompare(presentation, state_->previous) <= 0))
        throw std::invalid_argument("編碼 PTS 必須嚴格遞增，duration 必須為正");
    const auto& config = state_->config;
    if (nv12.size() != std::uint64_t{config.width} * config.height * 3 / 2) throw std::invalid_argument("NV12 編碼輸入大小不符");
    const auto pool = VTCompressionSessionGetPixelBufferPool(state_->session.value);
    if (!pool) throw VideoCodecError(kCVReturnInvalidPoolAttributes, "硬體編碼缺少 pixel buffer pool");
    Ref<CVPixelBufferRef> pixel;
    check(CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixel.value), "配置編碼 pixel buffer 失敗");
    check(CVPixelBufferLockBaseAddress(pixel.value, 0), "鎖定編碼 pixel buffer 失敗");
    struct Unlock { CVPixelBufferRef value; ~Unlock() { (void)CVPixelBufferUnlockBaseAddress(value, 0); } };
    {
        Unlock unlock{pixel.value};
        if (CVPixelBufferGetPlaneCount(pixel.value) != 2) throw std::invalid_argument("編碼 pixel buffer 不是 NV12");
        std::size_t offset{};
        for (unsigned plane = 0; plane < 2; ++plane) {
            auto* destination = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixel.value, plane));
            const auto stride = CVPixelBufferGetBytesPerRowOfPlane(pixel.value, plane);
            const auto rows = plane ? config.height / 2 : config.height;
            if (!destination || stride < config.width || CVPixelBufferGetHeightOfPlane(pixel.value, plane) < rows) throw std::invalid_argument("編碼 NV12 plane 範圍不符");
            for (unsigned row = 0; row < rows; ++row) { std::memcpy(destination + row * stride, nv12.data() + offset, config.width); offset += config.width; }
        }
    }
    Ref<CFDictionaryRef> properties;
    if (force_keyframe) {
        const void* key = kVTEncodeFrameOptionKey_ForceKeyFrame; const void* value = kCFBooleanTrue;
        properties.value = CFDictionaryCreate(kCFAllocatorDefault, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (!properties.value) throw std::bad_alloc();
    }
    State::Submission submission{tag};
    const auto submitted = VTCompressionSessionEncodeFrame(state_->session.value, pixel.value, presentation, interval, properties.value, &submission, nullptr);
    // 完成此 PTS 前的所有 callback，讓輸入及 refcon 所有權不跨呼叫存活；不重建 session／DPB。
    const auto completed = VTCompressionSessionCompleteFrames(state_->session.value, presentation);
    if (completed) state_->session.reset(); // 失敗時先取消尚存 callback，再釋放 stack refcon。
    check(submitted, "提交硬體編碼失敗"); check(completed, "等待硬體編碼失敗");
    state_->previous = presentation;
    return state_->take();
}
std::vector<EncodedPacket> HardwareEncoder::drain() {
    if (!state_ || !state_->session.value) throw std::logic_error("硬體編碼器已移轉或失效");
    if (state_->drained) return {};
    check(VTCompressionSessionCompleteFrames(state_->session.value, kCMTimeInvalid), "完成硬體編碼串流失敗");
    state_->drained = true;
    return state_->take();
}
void HardwareEncoder::flush() {
    if (!state_) throw std::logic_error("硬體編碼器已移轉");
    auto replacement = std::make_unique<State>(state_->config);
    state_ = std::move(replacement);
}
bool HardwareEncoder::hardware_accelerated() const noexcept { return state_ && state_->session.value && state_->hardware; }
