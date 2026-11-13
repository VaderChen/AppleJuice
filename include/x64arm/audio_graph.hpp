#pragma once
#include "x64arm/audio.hpp"
#include <array>
#include <optional>

namespace x64arm::audio {
// XAudio2 音源／子混音需要的宿主圖。裝置、PCM 解碼與 DSP 沿用 audio 後台。
// 所有控制方法與 render 共用一把鎖；客體回呼由服務層在 take_events 後派送。
class VoiceGraph final:public RenderSource {
public:
    using Voice=std::uint64_t;
    enum class Kind {Source,Submix,Master};
    struct Format {unsigned channels{2},rate{48000},bits{16};bool floating{};unsigned channel_mask{};};
    struct Packet {
        std::vector<std::uint8_t> pcm;
        unsigned play_begin{},play_length{},loop_begin{},loop_length{},loop_count{};
        std::uint64_t context{};bool end_stream{};
    };
    struct Filter {unsigned type{};float frequency{1},one_over_q{1};};
    struct Send {Voice destination{};std::vector<float> matrix;bool use_filter{};};
    struct State {
        Kind kind{};Format format;unsigned flags{},stage{},queued{};
        bool playing{};float volume{1},ratio{1};std::uint64_t samples{},context{};
        std::vector<float> channel_volumes;Filter filter;unsigned creation_flags{};
    };
    enum class EventKind {BufferStart,BufferEnd,LoopEnd,StreamEnd,CriticalError};
    struct Event {EventKind kind{};Voice voice{};std::uint64_t context{};};
    enum class Operation {Start,Stop,Volume,ChannelVolumes,Frequency,Filter,OutputFilter,Matrix,ExitLoop,EnableEffect,DisableEffect};
    struct Change {
        Operation operation{};Voice voice{},destination{};float value{};
        std::vector<float> values;Filter filter;bool tails{};
        unsigned effect{}; // EnableEffect／DisableEffect 的效果鏈索引
    };
    // 效果鏈的一格。Effect 是立體聲 DSP，每兩聲道一個（奇數的最後一個聲道單獨處理），數量須為 (聲道數+1)/2。
    struct EffectSlot {std::vector<std::shared_ptr<Effect>> pairs;bool enabled{true};};
    VoiceGraph();
    ~VoiceGraph() override;
    Voice create(Kind,Format,unsigned flags=0,unsigned stage=0,float maximum_ratio=2);
    void destroy(Voice);
    void set_sends(Voice,std::span<const Send>);
    void submit(Voice,Packet);
    void flush(Voice);
    void discontinuity(Voice);
    void set_sample_rate(Voice,unsigned);
    State state(Voice) const;
    std::uint32_t required_bytes(Voice,unsigned output_frames,unsigned output_rate) const;
    std::vector<float> matrix(Voice,Voice) const;
    Filter output_filter(Voice,Voice) const;
    // set=0 立即套用；非零集合只在 commit 時原子生效。集合內容不得部分更新。
    void change(Change,std::uint32_t set=0);
    void commit(std::uint32_t set=0);
    void set_effects(Voice,std::vector<std::shared_ptr<Effect>>);
    // 整條效果鏈原子替換；尚未 commit 的啟用／停用隨舊鏈失效。
    void set_effect_chain(Voice,std::vector<EffectSlot>);
    bool effect_enabled(Voice,unsigned index) const;
    void start(bool);
    bool running() const;
    std::vector<Event> take_events();
    // 在客體回呼邊界停止。可回傳 0（BufferStart 先於讀樣本）；呼叫端派送事件後續跑。
    std::size_t render_until_event(std::span<float>,unsigned);
    void render(std::span<float>,unsigned) noexcept override;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
    std::size_t render_frames(std::span<float>,unsigned,bool);
};
}
