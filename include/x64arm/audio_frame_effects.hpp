#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace x64arm::audio {
// 以整個音框處理、輸入與輸出聲道數可不同的效果（XAudio2 內建 reverb 與 volume meter）。
// 語音圖在混音鎖內逐幀呼叫 process；服務層的參數更新另以效果自己的鎖保護。
class FrameEffect {
public:
    virtual ~FrameEffect()=default;
    // 鎖定處理格式；不支援的聲道組合回傳 false，狀態不變。
    virtual bool configure(unsigned rate,unsigned input_channels,unsigned output_channels)=0;
    virtual void reset()=0;
    // frame 至少有 max(輸入,輸出) 格。enabled=false 依 XAPO 直通處理：保留聲道格式轉換，不套用效果。
    virtual void process(float* frame,bool enabled)=0;
    // 有尾音的效果在輸入靜音後仍會輸出。
    virtual bool tail()const=0;
    unsigned input_channels()const {return input_;}
    unsigned output_channels()const {return output_;}
    unsigned rate()const {return rate_;}
protected:
    unsigned rate_{48000},input_{1},output_{1};
};

// XAUDIO2FX_REVERB_PARAMETERS；預設值與範圍依 xaudio2fx.h。時間與頻率都以 48 kHz 為基準。
struct ReverbParameters {
    float wet_dry_mix{100};
    std::uint32_t reflections_delay{5};
    std::uint8_t reverb_delay{5},rear_delay{5},side_delay{5};
    std::uint8_t position_left{6},position_right{6},position_matrix_left{27},position_matrix_right{27};
    std::uint8_t early_diffusion{8},late_diffusion{8},low_eq_gain{8},low_eq_cutoff{4},high_eq_gain{8},high_eq_cutoff{4};
    float room_filter_freq{5000},room_filter_main{0},room_filter_hf{0},reflections_gain{0},reverb_gain{0};
    float decay_time{1},density{100},room_size{100};
    bool disable_late_field{};
};
// 超出範圍的欄位夾回範圍、非有限浮點數（DecayTime 的 +∞ 除外）改回預設；7.1 輸出時 RearDelay 上限為 20 ms。
ReverbParameters clamp_reverb(const ReverbParameters&,bool seven_one=false);

class Reverb final:public FrameEffect {
public:
    // surround71：XAudio 2.9 另支援 7.1 輸出（SideDelay）。
    explicit Reverb(bool surround71=false);
    ~Reverb() override;
    // 支援的組合：單聲道→單聲道／5.1、立體聲→立體聲／5.1，2.9 另有單聲道或立體聲→7.1。
    static bool supports(unsigned input_channels,unsigned output_channels,bool surround71);
    void set(const ReverbParameters&);
    ReverbParameters parameters()const;
    bool configure(unsigned rate,unsigned input_channels,unsigned output_channels) override;
    void reset() override;
    void process(float* frame,bool enabled) override;
    bool tail()const override {return true;}
private:
    struct State;
    std::unique_ptr<State> state_;
    bool surround71_{};
};

class VolumeMeter final:public FrameEffect {
public:
    struct Levels {std::vector<float> peak,rms;};
    bool configure(unsigned rate,unsigned input_channels,unsigned output_channels) override;
    void reset() override;
    void process(float* frame,bool enabled) override;
    bool tail()const override {return false;}
    // 一次 IXAPO::Process 呼叫或一個 10 ms 混音段結束時發布該段的峰值與 RMS。
    void finish_pass();
    // 每段幀數：語音圖用混音段（configure 預設 rate/100）；應用程式直接呼叫 Process 時以整次呼叫為一段。
    void set_pass(unsigned frames);
    Levels levels()const;
private:
    void publish();
    mutable std::mutex mutex_;
    std::vector<float> peak_,sum_,published_peak_,published_rms_;
    unsigned count_{},pass_{480};
};
}
