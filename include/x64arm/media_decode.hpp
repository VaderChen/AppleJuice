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
