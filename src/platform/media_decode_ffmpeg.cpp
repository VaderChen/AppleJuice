// Windows Media 解碼與 Video 1 編解碼使用內建 FFmpeg libavcodec／libavutil。
// 容器、色彩配置及 PCM 轉換由既有 MF 與 AVI 流程負責。
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
    case make_fourcc('H', '2', '6', '4'): return AV_CODEC_ID_H264;
    case make_fourcc('M', 'P', 'G', '1'): return AV_CODEC_ID_MPEG1VIDEO;
    case make_fourcc('M', 'P', 'G', '2'): return AV_CODEC_ID_MPEG2VIDEO;
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
    case 0x0050: return AV_CODEC_ID_MP2;
    case 0x0055: return AV_CODEC_ID_MP3;
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
bool video1_fourcc(std::uint32_t fourcc){
    switch(upper_fourcc(fourcc)){
    case make_fourcc('M','S','V','C'):
    case make_fourcc('C','R','A','M'):
    case make_fourcc('W','H','A','M'):
        return true;
    default:return false;
    }
}

namespace {
void validate_video1_size(unsigned width,unsigned height){
    if(width<4 || height<4 || width>16384 || height>16384 || (width&3) || (height&3) ||
       std::uint64_t{width}*height>32U*1024*1024)
        throw std::invalid_argument("Video 1 的畫面尺寸無效");
}
bool complete_video1_blocks(std::span<const std::uint8_t> packet,unsigned blocks,bool indexed){
    std::size_t at=0;
    while(blocks){
        if(packet.size()-at<2)return false;
        const unsigned low=packet[at++],high=packet[at++];
        if((high&0xfc)==0x84){
            const auto skipped=low+((high&3)<<8);
            if(!skipped || skipped>blocks)return false;
            blocks-=skipped;
            continue;
        }
        unsigned colors=0;
        if(high<0x80){
            if(indexed)colors=2;
            else{
                if(packet.size()-at<2)return false;
                colors=packet[at+1]&0x80?16:4;
            }
        }else if(indexed && high>=0x90)colors=8;
        if(packet.size()-at<colors)return false;
        at+=colors;
        --blocks;
    }
    return true;
}
}

struct Video1Decoder::State : Codec {using Codec::Codec;};
Video1Decoder::Video1Decoder(unsigned width,unsigned height,unsigned depth,std::span<const std::uint8_t> palette){
    validate_video1_size(width,height);
    if((depth!=8 && depth!=16) || palette.size()>AVPALETTE_SIZE || palette.size()%4)
        throw std::invalid_argument("Video 1 的像素格式無效");
    std::vector<std::uint8_t> color_table(depth==8?AVPALETTE_SIZE:0);
    if(depth==8)std::copy(palette.begin(),palette.end(),color_table.begin());
    state_=std::make_unique<State>(AV_CODEC_ID_MSVIDEO1,color_table,[=](AVCodecContext& context){
        context.width=context.coded_width=static_cast<int>(width);
        context.height=context.coded_height=static_cast<int>(height);
        context.bits_per_coded_sample=static_cast<int>(depth);
    });
}
Video1Decoder::~Video1Decoder()=default;

std::vector<std::uint8_t> Video1Decoder::decode(std::span<const std::uint8_t> packet,bool key){
    std::vector<std::uint8_t> output;
    const auto& format=*state_->context;
    const auto blocks=static_cast<unsigned>(format.width/4)*static_cast<unsigned>(format.height/4);
    if(!complete_video1_blocks(packet,blocks,format.bits_per_coded_sample==8))return output;
    state_->send(packet,INT64_MIN,key,[&](const AVFrame& frame){
        if(frame.width!=format.width || frame.height!=format.height ||
           (frame.format!=AV_PIX_FMT_RGB555 && frame.format!=AV_PIX_FMT_PAL8))return;
        const auto row=static_cast<std::size_t>(frame.width)*(frame.format==AV_PIX_FMT_PAL8?1U:2U);
        output.resize(row*static_cast<std::size_t>(frame.height));
        for(int y=0;y<frame.height;++y)
            std::memcpy(output.data()+row*static_cast<std::size_t>(frame.height-1-y),
                        frame.data[0]+std::ptrdiff_t{y}*frame.linesize[0],row);
    });
    return output;
}

struct Video1Encoder::State {
    std::unique_ptr<AVCodecContext,ContextDeleter> context;
    std::unique_ptr<AVFrame,FrameDeleter> frame{av_frame_alloc()};
    std::unique_ptr<AVPacket,PacketDeleter> packet{av_packet_alloc()};
    std::int64_t frame_number{};
};
Video1Encoder::Video1Encoder(unsigned width,unsigned height){
    validate_video1_size(width,height);
    quiet_logging();
    const auto* codec=avcodec_find_encoder(AV_CODEC_ID_MSVIDEO1);
    if(!codec)throw std::runtime_error("內建 FFmpeg 缺少 Video 1 編碼器");
    state_=std::make_unique<State>();
    state_->context.reset(avcodec_alloc_context3(codec));
    if(!state_->context || !state_->frame || !state_->packet)throw std::bad_alloc{};
    auto& format=*state_->context;
    format.width=static_cast<int>(width);
    format.height=static_cast<int>(height);
    format.time_base={1,25};
    format.pix_fmt=AV_PIX_FMT_RGB555;
    format.keyint_min=static_cast<int>(video1_default_key_rate);
    if(const auto status=avcodec_open2(&format,codec,nullptr);status<0)
        throw std::runtime_error("無法開啟 Video 1 編碼器："+error_text(status));
    auto& frame=*state_->frame;
    frame.width=format.width;
    frame.height=format.height;
    frame.format=format.pix_fmt;
    if(av_frame_get_buffer(&frame,0)<0)throw std::bad_alloc{};
}
Video1Encoder::~Video1Encoder()=default;

Video1Encoder::Packet Video1Encoder::encode(std::span<const std::uint8_t> rgb555,bool key){
    auto& state=*state_;
    auto& frame=*state.frame;
    const auto row=static_cast<std::size_t>(frame.width)*2;
    if(rgb555.size()!=row*static_cast<std::size_t>(frame.height))
        throw std::invalid_argument("Video 1 的 RGB555 資料長度無效");
    if(av_frame_make_writable(&frame)<0)throw std::bad_alloc{};
    for(int y=0;y<frame.height;++y)
        std::memcpy(frame.data[0]+std::ptrdiff_t{y}*frame.linesize[0],
                    rgb555.data()+row*static_cast<std::size_t>(frame.height-1-y),row);
    frame.pts=state.frame_number;
    state.context->keyint_min=key || !state.frame_number?0:static_cast<int>(video1_default_key_rate);
    if(const auto status=avcodec_send_frame(state.context.get(),&frame);status<0)
        throw std::runtime_error("無法送入 Video 1 影格："+error_text(status));
    ++state.frame_number;
    av_packet_unref(state.packet.get());
    if(const auto status=avcodec_receive_packet(state.context.get(),state.packet.get());status<0)
        throw std::runtime_error("無法取得 Video 1 封包："+error_text(status));
    return {{state.packet->data,state.packet->data+state.packet->size},bool(state.packet->flags&AV_PKT_FLAG_KEY)};
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
