#include "x64arm/midi.hpp"
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace x64arm::midi {
namespace {
class Synth final:public Output {
    AudioUnit synth_{},output_{};std::shared_ptr<Playback> playback_;double sample_time_{};
    static void send(void* context,std::uint8_t status,std::uint8_t a,std::uint8_t b,unsigned offset){
        auto& self=*static_cast<Synth*>(context);if(MusicDeviceMIDIEvent(self.synth_,status,a,b,offset)!=noErr)self.playback_->failed=true;
    }
    static void sysex(void* context,std::span<const std::uint8_t> bytes){auto& self=*static_cast<Synth*>(context);if(MusicDeviceSysEx(self.synth_,bytes.data(),static_cast<UInt32>(bytes.size()))!=noErr)self.playback_->failed=true;}
    static OSStatus render(void* context,AudioUnitRenderActionFlags* flags,const AudioTimeStamp*,UInt32,UInt32 frames,AudioBufferList* data){
        auto& self=*static_cast<Synth*>(context);std::lock_guard lock(self.playback_->mutex);
        self.playback_->advance(frames,send,&self,sysex);
        AudioTimeStamp time{};time.mFlags=kAudioTimeStampSampleTimeValid;time.mSampleTime=self.sample_time_;self.sample_time_+=frames;
        const auto status=AudioUnitRender(self.synth_,flags,&time,0,frames,data);
        if(status!=noErr || self.playback_->failed){self.playback_->failed=true;self.playback_->playing=false;for(UInt32 i=0;i<data->mNumberBuffers;++i)if(data->mBuffers[i].mData)std::memset(data->mBuffers[i].mData,0,data->mBuffers[i].mDataByteSize);}
        if(status==noErr && !self.playback_->failed){
            if(data->mNumberBuffers==2 && self.playback_->mono){auto* left=static_cast<float*>(data->mBuffers[0].mData);auto* right=static_cast<float*>(data->mBuffers[1].mData);if(left && right)for(UInt32 n=0;n<frames;++n)left[n]=right[n]=(left[n]+right[n])*0.5F;}
            const auto& p=*self.playback_;
            for(UInt32 i=0;i<data->mNumberBuffers;++i){auto& b=data->mBuffers[i];if(!b.mData)continue;auto* samples=static_cast<float*>(b.mData);const auto count=std::min<std::size_t>(b.mDataByteSize/sizeof(float),static_cast<std::size_t>(frames)*b.mNumberChannels);for(std::size_t n=0;n<count;++n){const auto factor=p.gain_remaining?p.gain+(p.gain_target-p.gain)*static_cast<float>(std::min<std::uint64_t>(n/b.mNumberChannels+1,p.gain_remaining))/static_cast<float>(p.gain_remaining):p.gain;samples[n]*=p.active?factor:0.0F;}}
            if(self.playback_->gain_remaining){const auto step=std::min<std::uint64_t>(frames,self.playback_->gain_remaining);self.playback_->gain+=(self.playback_->gain_target-self.playback_->gain)*static_cast<float>(step)/static_cast<float>(self.playback_->gain_remaining);self.playback_->gain_remaining-=step;}
            self.playback_->rendered_frames+=frames;
            for(UInt32 i=0;i<data->mNumberBuffers;++i){const auto& buffer=data->mBuffers[i];if(!buffer.mData)continue;
                const auto* samples=static_cast<const float*>(buffer.mData);const auto count=std::min<std::size_t>(buffer.mDataByteSize/sizeof(float),static_cast<std::size_t>(frames)*buffer.mNumberChannels);
                for(std::size_t n=0;n<count;++n)self.playback_->rendered_peak=std::max(self.playback_->rendered_peak,std::abs(samples[n]));}
        }
        return noErr;
    }
    static void check(OSStatus status){if(status!=noErr)throw std::runtime_error("Core Audio MIDI synthesizer unavailable: "+std::to_string(status));}
    static AudioUnit make(OSType type,OSType subtype){AudioComponentDescription d{};d.componentType=type;d.componentSubType=subtype;d.componentManufacturer=kAudioUnitManufacturer_Apple;const auto c=AudioComponentFindNext(nullptr,&d);if(!c)throw std::runtime_error("Core Audio MIDI component missing");AudioUnit u{};check(AudioComponentInstanceNew(c,&u));return u;}
    void close(){if(output_){AudioOutputUnitStop(output_);AudioUnitUninitialize(output_);AudioComponentInstanceDispose(output_);output_=nullptr;}if(synth_){AudioUnitUninitialize(synth_);AudioComponentInstanceDispose(synth_);synth_=nullptr;}}
public:
    explicit Synth(std::shared_ptr<Playback> playback):playback_(std::move(playback)){
        try {
            synth_=make(kAudioUnitType_MusicDevice,kAudioUnitSubType_DLSSynth);output_=make(kAudioUnitType_Output,kAudioUnitSubType_DefaultOutput);
            AudioStreamBasicDescription f{};f.mSampleRate=48000;f.mFormatID=kAudioFormatLinearPCM;f.mFormatFlags=static_cast<UInt32>(kAudioFormatFlagsNativeFloatPacked)|static_cast<UInt32>(kAudioFormatFlagIsNonInterleaved);f.mBytesPerPacket=4;f.mFramesPerPacket=1;f.mBytesPerFrame=4;f.mChannelsPerFrame=2;f.mBitsPerChannel=32;
            check(AudioUnitSetProperty(synth_,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&f,sizeof(f)));
            check(AudioUnitSetProperty(output_,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&f,sizeof(f)));
            UInt32 maximum=4096;check(AudioUnitSetProperty(synth_,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,sizeof(maximum)));
            AURenderCallbackStruct callback{render,this};check(AudioUnitSetProperty(output_,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&callback,sizeof(callback)));
            check(AudioUnitInitialize(synth_));check(AudioUnitInitialize(output_));check(AudioOutputUnitStart(output_));
        }catch(...){close();throw;}
    }
    bool short_message(std::uint8_t status,std::uint8_t first,std::uint8_t second) override {
        std::lock_guard lock(playback_->mutex);return !playback_->failed && MusicDeviceMIDIEvent(synth_,status,first,second,0)==noErr;
    }
    bool system_exclusive(std::span<const std::uint8_t> bytes) override {
        std::lock_guard lock(playback_->mutex);return !playback_->failed && MusicDeviceSysEx(synth_,bytes.data(),static_cast<UInt32>(bytes.size()))==noErr;
    }
    ~Synth()override{close();}
};
}
bool output_available(){
    AudioComponentDescription synth{};synth.componentType=kAudioUnitType_MusicDevice;synth.componentSubType=kAudioUnitSubType_DLSSynth;synth.componentManufacturer=kAudioUnitManufacturer_Apple;
    AudioComponentDescription output{};output.componentType=kAudioUnitType_Output;output.componentSubType=kAudioUnitSubType_DefaultOutput;output.componentManufacturer=kAudioUnitManufacturer_Apple;
    return AudioComponentFindNext(nullptr,&synth) && AudioComponentFindNext(nullptr,&output);
}
std::unique_ptr<Output> open_output(std::shared_ptr<Playback> playback){return std::make_unique<Synth>(std::move(playback));}
}
