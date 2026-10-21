#include "x64arm/audio_mixer.hpp"
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <cmath>
namespace x64arm::audio {
namespace {
AudioObjectPropertyAddress property(AudioObjectPropertySelector selector,bool capture,AudioObjectPropertyElement element=0){return {selector,capture?kAudioDevicePropertyScopeInput:kAudioDevicePropertyScopeOutput,element};}
template<class T>bool read(AudioDeviceID device,const AudioObjectPropertyAddress& address,T& value){UInt32 size=sizeof(T);return AudioObjectGetPropertyData(device,&address,0,nullptr,&size,&value)==noErr && size==sizeof(T);}
bool writable(AudioDeviceID device,const AudioObjectPropertyAddress& address){Boolean value=false;return AudioObjectIsPropertySettable(device,&address,&value)==noErr && value;}
template<class T>bool write(AudioDeviceID device,const AudioObjectPropertyAddress& address,const T& value){return writable(device,address) && AudioObjectSetPropertyData(device,&address,0,nullptr,sizeof(T),&value)==noErr;}
class CoreAudioMixer final:public MixerBackend {
public:
    std::optional<MixerEndpoint> query(std::uint64_t token) const override {
        const auto device=static_cast<AudioDeviceID>(token>>1);const bool capture=token&1U;
        if(!device || token>>33)return {};
        AudioObjectPropertyAddress name_property{kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal,0};CFStringRef name=nullptr;
        if(!read(device,name_property,name) || !name)return {};
        MixerEndpoint out;out.token=token;out.capture=capture;const auto length=CFStringGetLength(name);
        if(length>0 && length<32768){out.name.resize(static_cast<std::size_t>(length));CFStringGetCharacters(name,CFRangeMake(0,length),reinterpret_cast<UniChar*>(out.name.data()));}CFRelease(name);
        Float32 master=0,left=0,right=0;
        if(read(device,property(kAudioDevicePropertyVolumeScalar,capture),master) && std::isfinite(master)){
            out.uniform_volume=true;out.volume=std::array{std::clamp(master,0.F,1.F),std::clamp(master,0.F,1.F)};
            out.volume_writable=writable(device,property(kAudioDevicePropertyVolumeScalar,capture));
        }else if(read(device,property(kAudioDevicePropertyVolumeScalar,capture,1),left) && read(device,property(kAudioDevicePropertyVolumeScalar,capture,2),right) && std::isfinite(left) && std::isfinite(right)){
            out.volume=std::array{std::clamp(left,0.F,1.F),std::clamp(right,0.F,1.F)};
            out.volume_writable=writable(device,property(kAudioDevicePropertyVolumeScalar,capture,1)) && writable(device,property(kAudioDevicePropertyVolumeScalar,capture,2));
        }
        UInt32 muted=0;if(read(device,property(kAudioDevicePropertyMute,capture),muted)){out.muted=muted!=0;out.mute_writable=writable(device,property(kAudioDevicePropertyMute,capture));}
        return out;
    }
    std::vector<MixerEndpoint> endpoints() const override {
        std::vector<MixerEndpoint> result;
        for(bool capture:{false,true}){
            AudioObjectPropertyAddress address{capture?kAudioHardwarePropertyDefaultInputDevice:kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,0};AudioDeviceID device=0;
            if(read(kAudioObjectSystemObject,address,device))if(auto value=query((std::uint64_t{device}<<1)|capture))result.push_back(std::move(*value));
        }
        return result;
    }
    bool set_volume(std::uint64_t token,std::array<float,2> volume) override {
        const auto old=query(token);if(!old || !old->volume || !old->volume_writable || !std::isfinite(volume[0]) || !std::isfinite(volume[1]) || volume[0]<0 || volume[0]>1 || volume[1]<0 || volume[1]>1)return false;
        const auto device=static_cast<AudioDeviceID>(token>>1);
        if(old->uniform_volume)return volume[0]==volume[1] && write(device,property(kAudioDevicePropertyVolumeScalar,old->capture),volume[0]);
        if(!write(device,property(kAudioDevicePropertyVolumeScalar,old->capture,1),volume[0]))return false;
        if(write(device,property(kAudioDevicePropertyVolumeScalar,old->capture,2),volume[1]))return true;
        (void)write(device,property(kAudioDevicePropertyVolumeScalar,old->capture,1),(*old->volume)[0]);return false;
    }
    bool set_mute(std::uint64_t token,bool mute) override {const auto old=query(token);const UInt32 value=mute;return old && old->muted && old->mute_writable && write(static_cast<AudioDeviceID>(token>>1),property(kAudioDevicePropertyMute,old->capture),value);}
};
}
std::shared_ptr<MixerBackend> make_mixer_backend(){return std::make_shared<CoreAudioMixer>();}
}
