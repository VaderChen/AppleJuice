#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace x64arm::audio {
struct MixerEndpoint {
    std::uint64_t token{};
    std::u16string name;
    bool capture{},uniform_volume{},volume_writable{},mute_writable{};
    unsigned channels{2};
    std::optional<std::array<float,2>> volume;
    std::optional<bool> muted;
};
class MixerBackend {
public:
    virtual ~MixerBackend()=default;
    virtual std::vector<MixerEndpoint> endpoints() const=0;
    virtual std::optional<MixerEndpoint> query(std::uint64_t token) const=0;
    virtual bool set_volume(std::uint64_t token,std::array<float,2>)=0;
    virtual bool set_mute(std::uint64_t token,bool)=0;
};
std::shared_ptr<MixerBackend> make_mixer_backend();
}
