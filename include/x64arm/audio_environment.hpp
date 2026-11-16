#pragma once
#include <memory>
#include <span>

namespace x64arm::audio {
// 多聲道宿主 PCM → 耳機立體聲；不開啟裝置、不改變播放時鐘、不依賴頭部感測。
class EnvironmentRenderer {
public:
    virtual ~EnvironmentRenderer()=default;
    virtual bool render(std::span<const float> interleaved,std::span<float> stereo) noexcept=0;
    virtual void position(float x,float y,float z) noexcept=0;
};
// 建構／設定在開啟裝置前完成。render 的區塊大小不超過 maximum_frames，且不配置工作緩衝。
std::unique_ptr<EnvironmentRenderer> make_environment_renderer(unsigned channels,unsigned mask,unsigned rate,unsigned maximum_frames);
}
