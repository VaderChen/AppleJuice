#pragma once
#include <cstdint>
#include <memory>
#include <span>

namespace x64arm::audio {
// DirectSound 參數由 API 執行緒驗證；render callback 僅處理已配置的宿主 DSP。
class Effect {
public:
    enum class Type : unsigned { Gargle,Chorus,Flanger,Echo,Distortion,Compressor,ParamEq,I3DL2Reverb,WavesReverb };
    explicit Effect(Type,unsigned sample_rate=48000);
    ~Effect();
    Type type()const;
    std::span<const std::uint32_t> parameters()const;
    bool set_parameters(std::span<const std::uint32_t>);
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
