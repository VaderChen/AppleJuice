#pragma once
#include <cstdint>
#include <memory>
#include <span>

namespace x64arm::audio {
// DirectSound 參數由 API 執行緒驗證；render callback 僅處理已配置的宿主 DSP。
class Effect {
public:
    // 前九種是 DirectSound DMO；Fx* 是 XAPOFX（xapofx.h）的四種效果，參數字組順序即對應的 *_PARAMETERS 結構。
    enum class Type : unsigned { Gargle,Chorus,Flanger,Echo,Distortion,Compressor,ParamEq,I3DL2Reverb,WavesReverb,FxEq,FxMasteringLimiter,FxReverb,FxEcho };
    explicit Effect(Type,unsigned sample_rate=48000);
    ~Effect();
    Type type()const;
    std::span<const std::uint32_t> parameters()const;
    bool set_parameters(std::span<const std::uint32_t>);
    // XAPOFX 的 SetParameters 沒有回傳值：超出 xapofx.h 範圍的欄位夾回範圍、非有限值改回預設。其他種類等同 set_parameters。
    bool set_parameters_clamped(std::span<const std::uint32_t>);
    bool set_preset(unsigned);
    unsigned preset()const;
    bool set_quality(unsigned);
    unsigned quality()const;
    void reset();
    void process(float& left,float& right,float distance=0);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
