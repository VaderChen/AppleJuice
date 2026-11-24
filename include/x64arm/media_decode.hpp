#pragma once

#include "x64arm/video_codec.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace x64arm::media {

// Windows Media（ASF）內容的軟體解碼：VC-1／WMV 視訊與 WMA 音訊，由內建、只含解碼器的 FFmpeg 提供。
// 時間一律以 100 ns 為單位，與 Media Foundation 相同；資料都由呼叫端持有，不接觸客體記憶體。

// BITMAPINFOHEADER 的 biCompression、尺寸，以及標頭之後的編碼器私有資料（WVC1 含開頭的 ASF binding byte）。
struct CompressedVideo {
    std::uint32_t fourcc{}, width{}, height{};
    std::vector<std::uint8_t> extra;
};
// WAVEFORMATEX 欄位與 cbSize 指定的額外資料。
struct CompressedAudio {
    std::uint16_t format_tag{}, channels{}, block_align{}, bits{};
    std::uint32_t sample_rate{}, bytes_per_second{};
    std::vector<std::uint8_t> extra;
};

bool video_decoder_available(std::uint32_t fourcc);
bool audio_decoder_available(std::uint16_t format_tag);
bool video1_fourcc(std::uint32_t fourcc);
// Cinepak（cvid）：Windows 內建 iccvid 的傳統 VFW 視訊，24／32 位元輸出。
bool cinepak_fourcc(std::uint32_t fourcc);
// 解碼成來源位元深度 DIB 列（由下而上、每列 4 位元組對齊）的傳統 VFW 視訊：Video 1 與 Cinepak。
bool dib_codec_fourcc(std::uint32_t fourcc);
inline constexpr unsigned video1_default_key_rate=15;

// DIB 輸出的視訊解碼器：AVI 預覽、AVIFile 取影格與 VFW 解壓縮共用。
class DibVideoDecoder {
public:
    virtual ~DibVideoDecoder()=default;
    // 回傳完整影格的 DIB 列；資料不完整或解碼失敗時回傳空陣列。
    virtual std::vector<std::uint8_t> decode(std::span<const std::uint8_t> packet,bool key)=0;
};
// 依 biCompression 建立解碼器（palette 為 8 位元格式的 RGBQUAD 色表）；不支援時丟出 std::invalid_argument。
std::unique_ptr<DibVideoDecoder> open_dib_decoder(std::uint32_t fourcc,unsigned width,unsigned height,unsigned depth,std::span<const std::uint8_t> palette);

// Video 1 的點陣圖以 DIB 順序輸出，保留 PAL8 索引或 RGB555 的原始位元。
class Video1Decoder final : public DibVideoDecoder {
public:
    Video1Decoder(unsigned width,unsigned height,unsigned depth,std::span<const std::uint8_t> palette);
    ~Video1Decoder() override;
    std::vector<std::uint8_t> decode(std::span<const std::uint8_t> packet,bool key) override;
private:
    struct State;
    std::unique_ptr<State> state_;
};

class Video1Encoder {
public:
    struct Packet {std::vector<std::uint8_t> bytes;bool key{};};
    Video1Encoder(unsigned width,unsigned height);
    ~Video1Encoder();
    Packet encode(std::span<const std::uint8_t> rgb555,bool key);
private:
    struct State;
    std::unique_ptr<State> state_;
};

// 解碼後的音訊：交錯排列的 32 位元浮點樣本，time 為第一個樣本的時間（未知時為 INT64_MIN）。
struct DecodedAudio {
    std::int64_t time{INT64_MIN};
    unsigned rate{}, channels{};
    std::vector<float> samples;
    std::size_t frames() const { return channels ? samples.size() / channels : 0; }
};

// 視訊輸出緊密 NV12（video::DecodedFrame），時間依顯示順序；pts 與 duration 的 timescale 為 10000000。
class VideoDecoder {
public:
    explicit VideoDecoder(const CompressedVideo& format);
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    // 損毀的封包依解碼器行為略過（不產生畫面），其他錯誤丟出 std::runtime_error。
    std::vector<video::DecodedFrame> decode(std::span<const std::uint8_t> packet, std::int64_t time, bool key);
    // 串流結束：交出延遲中的畫面；之後需 flush 才能再送資料。
    std::vector<video::DecodedFrame> drain();
    // 跳轉或清除：丟棄延遲中的畫面與參考畫面。
    void flush();
private:
    struct State;
    std::unique_ptr<State> state_;
};

class AudioDecoder {
public:
    explicit AudioDecoder(const CompressedAudio& format);
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    std::vector<DecodedAudio> decode(std::span<const std::uint8_t> packet, std::int64_t time);
    std::vector<DecodedAudio> drain();
    void flush();
    // 解碼器回報的輸出格式；開啟時即確定，與 WAVEFORMATEX 的聲道數、取樣率相同。
    unsigned rate() const;
    unsigned channels() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace x64arm::media
