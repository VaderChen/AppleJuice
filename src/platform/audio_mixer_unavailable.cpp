#include "x64arm/audio_mixer.hpp"
namespace x64arm::audio {
namespace {
class UnavailableMixer final:public MixerBackend {
public:
    std::vector<MixerEndpoint> endpoints() const override{return {};}
    std::optional<MixerEndpoint> query(std::uint64_t) const override{return {};}
    bool set_volume(std::uint64_t,std::array<float,2>) override{return false;}
    bool set_mute(std::uint64_t,bool) override{return false;}
};
}
std::shared_ptr<MixerBackend> make_mixer_backend(){return std::make_shared<UnavailableMixer>();}
}
