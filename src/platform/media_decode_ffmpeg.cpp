// Windows Media 內容的軟體解碼後端：只連結 FFmpeg 的 libavcodec／libavutil 解碼器（cmake/FFmpeg.cmake），
// 不使用容器、縮放、重取樣與編碼器。解多工、色彩配置與 PCM 轉換由引擎既有的 MF 流程負責。
#include "x64arm/media_decode.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

namespace x64arm::media {
namespace {

constexpr AVRational media_time_base{1, 10000000};

void quiet_logging() {
    // 客體影片損毀時 FFmpeg 會逐幀警告；只保留致命訊息，避免淹沒引擎診斷輸出。
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(AV_LOG_FATAL); });
}

std::uint32_t upper_fourcc(std::uint32_t fourcc) {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8)
        value |= static_cast<std::uint32_t>(std::toupper(static_cast<int>((fourcc >> shift) & 0xffU))) << shift;
    return value;
}
constexpr std::uint32_t make_fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) | static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16 | static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24;
}

AVCodecID video_codec(std::uint32_t fourcc) {
    switch (upper_fourcc(fourcc)) {
    case make_fourcc('W', 'V', 'C', '1'):
    case make_fourcc('W', 'M', 'V', 'A'): return AV_CODEC_ID_VC1;
    case make_fourcc('W', 'M', 'V', '3'): return AV_CODEC_ID_WMV3;
    case make_fourcc('W', 'M', 'V', '2'): return AV_CODEC_ID_WMV2;
    case make_fourcc('W', 'M', 'V', '1'): return AV_CODEC_ID_WMV1;
    default: return AV_CODEC_ID_NONE;
    }
}
AVCodecID audio_codec(std::uint16_t tag) {
    switch (tag) {
    case 0x0160: return AV_CODEC_ID_WMAV1;
    case 0x0161: return AV_CODEC_ID_WMAV2;
    case 0x0162: return AV_CODEC_ID_WMAPRO;
    case 0x0163: return AV_CODEC_ID_WMALOSSLESS;
    case 0x000a: return AV_CODEC_ID_WMAVOICE;
    default: return AV_CODEC_ID_NONE;
    }
}

std::string error_text(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, text, sizeof text);
    return text;
}

struct ContextDeleter { void operator()(AVCodecContext* p) const { avcodec_free_context(&p); } };
struct FrameDeleter { void operator()(AVFrame* p) const { av_frame_free(&p); } };
struct PacketDeleter { void operator()(AVPacket* p) const { av_packet_free(&p); } };

// 共用的 send／receive 流程：封包損毀（AVERROR_INVALIDDATA）依解碼器慣例略過，其他錯誤視為解碼器故障。
struct Codec {
    std::unique_ptr<AVCodecContext, ContextDeleter> context;
    std::unique_ptr<AVFrame, FrameDeleter> frame{av_frame_alloc()};
    std::unique_ptr<AVPacket, PacketDeleter> packet{av_packet_alloc()};
    bool drained{};

    Codec(AVCodecID id, const std::vector<std::uint8_t>& extra, const auto& configure) {
        quiet_logging();
        if (!frame || !packet) throw std::bad_alloc{};
        const AVCodec* codec = avcodec_find_decoder(id);
        if (!codec) throw std::runtime_error("內建 FFmpeg 未包含此解碼器");
        context.reset(avcodec_alloc_context3(codec));
        if (!context) throw std::bad_alloc{};
        context->pkt_timebase = media_time_base;
        context->thread_count = 1;
        if (!extra.empty()) {
            context->extradata = static_cast<std::uint8_t*>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!context->extradata) throw std::bad_alloc{};
            std::memcpy(context->extradata, extra.data(), extra.size());
            context->extradata_size = static_cast<int>(extra.size());
        }
        configure(*context);
        if (const int status = avcodec_open2(context.get(), codec, nullptr); status < 0)
            throw std::runtime_error("無法開啟解碼器：" + error_text(status));
    }

    template <class Emit> void receive(Emit&& emit) {
        for (;;) {
            const int status = avcodec_receive_frame(context.get(), frame.get());
            if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) return;
            if (status == AVERROR_INVALIDDATA) continue;
            if (status < 0) throw std::runtime_error("解碼失敗：" + error_text(status));
            emit(*frame);
            av_frame_unref(frame.get());
        }
    }
    template <class Emit> void send(std::span<const std::uint8_t> bytes, std::int64_t time, bool key, Emit&& emit) {
        if (drained) flush();
        av_packet_unref(packet.get());
        if (bytes.size() > static_cast<std::size_t>(INT32_MAX - AV_INPUT_BUFFER_PADDING_SIZE)) throw std::runtime_error("壓縮樣本過大");
        if (av_new_packet(packet.get(), static_cast<int>(bytes.size())) < 0) throw std::bad_alloc{};
        if (!bytes.empty()) std::memcpy(packet->data, bytes.data(), bytes.size());
        packet->pts = time == INT64_MIN ? AV_NOPTS_VALUE : time;
        packet->dts = AV_NOPTS_VALUE;
        if (key) packet->flags |= AV_PKT_FLAG_KEY;
        for (;;) {
            const int status = avcodec_send_packet(context.get(), packet.get());
            if (status == AVERROR(EAGAIN)) { receive(emit); continue; }
            if (status == AVERROR_INVALIDDATA) break;
            if (status < 0) throw std::runtime_error("送入壓縮樣本失敗：" + error_text(status));
            break;
        }
        av_packet_unref(packet.get());
        receive(emit);
    }
    template <class Emit> void drain(Emit&& emit) {
        if (drained) return;
        const int status = avcodec_send_packet(context.get(), nullptr);
        if (status < 0 && status != AVERROR_EOF) throw std::runtime_error("清空解碼器失敗：" + error_text(status));
        receive(emit);
        drained = true;
    }
    void flush() { avcodec_flush_buffers(context.get()); drained = false; }
};

std::int64_t frame_time(const AVFrame& frame) {
    const auto time = frame.best_effort_timestamp != AV_NOPTS_VALUE ? frame.best_effort_timestamp : frame.pts;
    return time == AV_NOPTS_VALUE ? INT64_MIN : time;
}

video::DecodedFrame to_nv12(const AVFrame& frame) {
    const auto format = static_cast<AVPixelFormat>(frame.format);
    if (frame.width <= 0 || frame.height <= 0) throw std::runtime_error("解碼畫面尺寸無效");
    const auto width = static_cast<std::uint32_t>(frame.width), height = static_cast<std::uint32_t>(frame.height);
    const auto chroma_rows = (height + 1) / 2, pairs = (width + 1) / 2;
    video::DecodedFrame out;
    out.width = width;
    out.height = height;
    out.full_range = frame.color_range == AVCOL_RANGE_JPEG || format == AV_PIX_FMT_YUVJ420P;
    // 緊密 NV12：UV 每列 width bytes（奇數寬度時最後一組只放得下 U）。
    const auto uv_row = std::max<std::size_t>(width, std::size_t{pairs} * 2);
    out.nv12.resize(std::size_t{width} * height + uv_row * chroma_rows);
    auto* luma = out.nv12.data();
    for (std::uint32_t y = 0; y < height; ++y)
        std::memcpy(luma + std::size_t{y} * width, frame.data[0] + static_cast<std::ptrdiff_t>(y) * frame.linesize[0], width);
    auto* chroma = out.nv12.data() + std::size_t{width} * height;
    switch (format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        for (std::uint32_t y = 0; y < chroma_rows; ++y) {
            const auto* u = frame.data[1] + static_cast<std::ptrdiff_t>(y) * frame.linesize[1];
            const auto* v = frame.data[2] + static_cast<std::ptrdiff_t>(y) * frame.linesize[2];
            auto* row = chroma + std::size_t{y} * width;
            for (std::uint32_t x = 0; x < pairs && x * 2 < width; ++x) {
                row[x * 2] = u[x];
                if (x * 2 + 1 < width) row[x * 2 + 1] = v[x];
            }
        }
        break;
    case AV_PIX_FMT_NV12:
        for (std::uint32_t y = 0; y < chroma_rows; ++y)
            std::memcpy(chroma + std::size_t{y} * width, frame.data[1] + static_cast<std::ptrdiff_t>(y) * frame.linesize[1], width);
        break;
    default: throw std::runtime_error(std::string("不支援的解碼畫面格式：") + (av_get_pix_fmt_name(format) ? av_get_pix_fmt_name(format) : "?"));
    }
    const auto time = frame_time(frame);
    if (time != INT64_MIN) out.pts = {time, 10000000};
    if (frame.duration > 0) out.duration = {frame.duration, 10000000};
    return out;
}

DecodedAudio to_float(const AVFrame& frame) {
    DecodedAudio out;
    out.time = frame_time(frame);
    out.rate = static_cast<unsigned>(frame.sample_rate);
    out.channels = static_cast<unsigned>(frame.ch_layout.nb_channels);
    const auto count = static_cast<std::size_t>(std::max(frame.nb_samples, 0));
    if (!out.channels) throw std::runtime_error("解碼音訊沒有聲道");
    out.samples.resize(count * out.channels);
    const auto planar = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame.format)) != 0;
    const auto sample = [&](unsigned channel, std::size_t index) -> const std::uint8_t* {
        const auto bytes = static_cast<std::size_t>(av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame.format)));
        return planar ? frame.extended_data[channel] + index * bytes : frame.extended_data[0] + (index * out.channels + channel) * bytes;
    };
    const auto packed = av_get_packed_sample_fmt(static_cast<AVSampleFormat>(frame.format));
    for (std::size_t i = 0; i < count; ++i)
        for (unsigned c = 0; c < out.channels; ++c) {
            const auto* p = sample(c, i);
            float value = 0;
            switch (packed) {
            case AV_SAMPLE_FMT_FLT: std::memcpy(&value, p, 4); break;
            case AV_SAMPLE_FMT_DBL: { double d; std::memcpy(&d, p, 8); value = static_cast<float>(d); break; }
            case AV_SAMPLE_FMT_S16: { std::int16_t s; std::memcpy(&s, p, 2); value = static_cast<float>(s) / 32768.0f; break; }
            case AV_SAMPLE_FMT_S32: { std::int32_t s; std::memcpy(&s, p, 4); value = static_cast<float>(static_cast<double>(s) / 2147483648.0); break; }
            case AV_SAMPLE_FMT_U8: value = (static_cast<float>(*p) - 128.0f) / 128.0f; break;
            default: throw std::runtime_error("不支援的解碼音訊樣本格式");
            }
            out.samples[i * out.channels + c] = value;
        }
    return out;
}

} // namespace

bool video_decoder_available(std::uint32_t fourcc) {
    const auto id = video_codec(fourcc);
    return id != AV_CODEC_ID_NONE && avcodec_find_decoder(id);
}
bool audio_decoder_available(std::uint16_t format_tag) {
    const auto id = audio_codec(format_tag);
    return id != AV_CODEC_ID_NONE && avcodec_find_decoder(id);
}

struct VideoDecoder::State : Codec {
    using Codec::Codec;
};
VideoDecoder::VideoDecoder(const CompressedVideo& format) {
    const auto id = video_codec(format.fourcc);
    if (id == AV_CODEC_ID_NONE) throw std::invalid_argument("不支援的視訊 FOURCC");
    if (!format.width || !format.height || format.width > 16384 || format.height > 16384) throw std::invalid_argument("視訊尺寸無效");
    state_ = std::make_unique<State>(id, format.extra, [&](AVCodecContext& context) {
        context.codec_tag = format.fourcc;
        context.width = context.coded_width = static_cast<int>(format.width);
        context.height = context.coded_height = static_cast<int>(format.height);
    });
}
VideoDecoder::~VideoDecoder() = default;
std::vector<video::DecodedFrame> VideoDecoder::decode(std::span<const std::uint8_t> packet, std::int64_t time, bool key) {
    std::vector<video::DecodedFrame> frames;
    state_->send(packet, time, key, [&](const AVFrame& frame) { frames.push_back(to_nv12(frame)); });
    return frames;
}
std::vector<video::DecodedFrame> VideoDecoder::drain() {
    std::vector<video::DecodedFrame> frames;
    state_->drain([&](const AVFrame& frame) { frames.push_back(to_nv12(frame)); });
    return frames;
}
void VideoDecoder::flush() { state_->flush(); }

struct AudioDecoder::State : Codec {
    using Codec::Codec;
};
AudioDecoder::AudioDecoder(const CompressedAudio& format) {
    const auto id = audio_codec(format.format_tag);
    if (id == AV_CODEC_ID_NONE) throw std::invalid_argument("不支援的音訊 wFormatTag");
    if (!format.channels || !format.sample_rate || format.channels > 8) throw std::invalid_argument("音訊格式無效");
    state_ = std::make_unique<State>(id, format.extra, [&](AVCodecContext& context) {
        context.codec_tag = format.format_tag;
        context.sample_rate = static_cast<int>(format.sample_rate);
        av_channel_layout_default(&context.ch_layout, format.channels);
        context.block_align = format.block_align;
        context.bit_rate = static_cast<std::int64_t>(format.bytes_per_second) * 8;
        context.bits_per_coded_sample = format.bits;
    });
}
AudioDecoder::~AudioDecoder() = default;
std::vector<DecodedAudio> AudioDecoder::decode(std::span<const std::uint8_t> packet, std::int64_t time) {
    std::vector<DecodedAudio> blocks;
    state_->send(packet, time, true, [&](const AVFrame& frame) { if (frame.nb_samples > 0) blocks.push_back(to_float(frame)); });
    return blocks;
}
std::vector<DecodedAudio> AudioDecoder::drain() {
    std::vector<DecodedAudio> blocks;
    state_->drain([&](const AVFrame& frame) { if (frame.nb_samples > 0) blocks.push_back(to_float(frame)); });
    return blocks;
}
void AudioDecoder::flush() { state_->flush(); }
unsigned AudioDecoder::rate() const { return static_cast<unsigned>(state_->context->sample_rate); }
unsigned AudioDecoder::channels() const { return static_cast<unsigned>(state_->context->ch_layout.nb_channels); }

} // namespace x64arm::media
