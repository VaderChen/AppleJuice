#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "x64arm/audio_effects.hpp"

namespace x64arm::audio {
// Host callback only reads this owned PCM copy, never guest memory.
struct Buffer {
    std::mutex mutex;
    std::vector<std::uint8_t> pcm;
    unsigned channels{2}, bits{16}, rate{44100}, frequency{44100};
    double cursor{};
    float left{1}, right{1};
    bool playing{}, looping{}, streaming{}, floating{};
    struct Notification { std::uint32_t offset; std::uint64_t event; };
    std::vector<Notification> notifications;
    std::vector<std::uint64_t> pending_notifications;
    float spatial_left{1}, spatial_right{1}, doppler{1};
    float spatial_distance{};
    std::vector<std::shared_ptr<Effect>> effects;
    bool capture_failed{};
    std::uint64_t captured_frames{};
    std::uint64_t rendered_frames{};
    // 串流播放中、但客體尚未補入資料而輸出靜音的 frame 數與次數（音訊斷續診斷）。
    std::uint64_t starved_frames{},starved_runs{};bool starving{};
    float rendered_peak{};
    float rendered_left_peak{},rendered_right_peak{};
    double capture_fraction{};
    void stop(bool reset_effects=true); // 呼叫端持有 mutex；callback 只記錄事件。
    unsigned frame_bytes() const { return channels*(bits/8); }
    void render(std::span<float> stereo, unsigned output_rate);
    void capture(std::span<const float> stereo,unsigned input_rate);
};
class Output {
public:
    virtual ~Output() = default;
};
// Throws on unavailable output; the API must not report silent success.
bool output_available();
std::unique_ptr<Output> open_output(std::shared_ptr<Buffer>);
// 指定宿主裝置 UID；空字串為系統預設輸出（隨系統切換預設裝置）。
std::unique_ptr<Output> open_output(std::shared_ptr<Buffer>,std::string_view device);
// 宿主音訊裝置目錄：每個 Core Audio 裝置的穩定 UID、名稱、輸入／輸出聲道與傳輸類型。
struct DeviceInfo {
    std::string uid;
    std::u16string name,manufacturer;
    unsigned output_channels{},input_channels{};
    double sample_rate{};
    std::uint32_t transport{}; // kAudioDevicePropertyTransportType 四字元碼
    bool default_output{},default_input{};
};
std::vector<DeviceInfo> devices();
class Input {public:virtual ~Input()=default;};
// 可替換的輸入裝置來源；未指定時服務層沿用平台的預設 CoreAudio 輸入。
class InputBackend {
public:
    virtual ~InputBackend()=default;
    virtual bool available() const=0;
    virtual std::unique_ptr<Input> open(std::shared_ptr<Buffer>)=0;
};
bool input_available();
std::unique_ptr<Input> open_input(std::shared_ptr<Buffer>);
}
