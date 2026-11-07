#include "x64arm/midi.hpp"
#include "audio_capture.hpp"
#include "audio_device.hpp"
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <CoreServices/CoreServices.h>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
namespace x64arm::midi {
namespace {
class Synth final:public Output {
    AudioUnit synth_{},output_{};std::shared_ptr<Playback> playback_;double sample_time_{};
    std::unique_ptr<audio::OutputCapture> capture_;
    struct BankUnit {AudioUnit unit{};std::shared_ptr<const SoundBank> bank;std::string path;};
    std::vector<BankUnit> banks_;std::array<unsigned,16> bank_msb_{},bank_lsb_{},program_{};
    std::array<std::array<std::uint8_t,3>,16*131> controls_{};
    std::array<float,4096> bank_left_{},bank_right_{};
    bool message(std::uint8_t status,std::uint8_t a,std::uint8_t b,unsigned offset){
        const auto kind=status&0xf0U,channel=status&15U;
        if(kind==0xb0){if(a==0)bank_msb_[channel]=b;else if(a==32)bank_lsb_[channel]=b;}
        else if(kind==0xc0)program_[channel]=a;
        if(kind>=0xb0 && kind<=0xe0)controls_[channel*131+(kind==0xb0?a:kind==0xc0?128U:kind==0xd0?129U:130U)]={status,a,b};
        AudioUnit target=synth_;
        if(kind==0x90 && b){const auto patch=(channel==9?0x80000000U:0U)|(bank_msb_[channel]<<16)|(bank_lsb_[channel]<<8)|program_[channel];
            for(auto i=banks_.rbegin();i!=banks_.rend();++i)if(std::any_of(i->bank->instruments.begin(),i->bank->instruments.end(),[&](const auto& x){return x.patch==patch;})){target=i->unit;break;}}
        if(kind==0x90 && b)return !target || MusicDeviceMIDIEvent(target,status,a,b,offset)==noErr;
        bool ok=MusicDeviceMIDIEvent(synth_,status,a,b,offset)==noErr;for(const auto& bank:banks_)if(bank.unit)ok=(MusicDeviceMIDIEvent(bank.unit,status,a,b,offset)==noErr)&&ok;return ok;
    }
    static void send(void* context,std::uint8_t status,std::uint8_t a,std::uint8_t b,unsigned offset){
        auto& self=*static_cast<Synth*>(context);if(!self.message(status,a,b,offset))self.playback_->failed=true;
    }
    bool exclusive(std::span<const std::uint8_t> bytes){
        bool ok=MusicDeviceSysEx(synth_,bytes.data(),static_cast<UInt32>(bytes.size()))==noErr;for(const auto& bank:banks_)if(bank.unit)ok=(MusicDeviceSysEx(bank.unit,bytes.data(),static_cast<UInt32>(bytes.size()))==noErr)&&ok;
        if(ok && bytes.size()==6 && bytes[0]==0xf0 && bytes[1]==0x7e && bytes[3]==9 && (bytes[4]==1 || bytes[4]==3) && bytes[5]==0xf7){bank_msb_={};bank_lsb_={};program_={};controls_={};}
        return ok;
    }
    static void sysex(void* context,std::span<const std::uint8_t> bytes){auto& self=*static_cast<Synth*>(context);if(!self.exclusive(bytes))self.playback_->failed=true;}
    static OSStatus render(void* context,AudioUnitRenderActionFlags* flags,const AudioTimeStamp*,UInt32,UInt32 frames,AudioBufferList* data){
        auto& self=*static_cast<Synth*>(context);std::lock_guard lock(self.playback_->mutex);
        if(self.playback_->reset)for(const auto& effect:self.playback_->effects)effect->reset();
        self.playback_->advance(frames,send,&self,sysex);
        AudioTimeStamp time{};time.mFlags=kAudioTimeStampSampleTimeValid;time.mSampleTime=self.sample_time_;self.sample_time_+=frames;
        auto status=AudioUnitRender(self.synth_,flags,&time,0,frames,data);
        if(frames>self.bank_left_.size())status=kAudioUnitErr_TooManyFramesToProcess;
        if(status==noErr && data->mNumberBuffers==2)for(const auto& bank:self.banks_){
            if(!bank.unit)continue;
            struct List {UInt32 count;AudioBuffer buffers[2];} buffers{2,{{1,frames*4,self.bank_left_.data()},{1,frames*4,self.bank_right_.data()}}};
            AudioUnitRenderActionFlags bank_flags=0;status=AudioUnitRender(bank.unit,&bank_flags,&time,0,frames,reinterpret_cast<AudioBufferList*>(&buffers));if(status!=noErr)break;
            auto* left=static_cast<float*>(data->mBuffers[0].mData);auto* right=static_cast<float*>(data->mBuffers[1].mData);if(left && right)for(UInt32 n=0;n<frames;++n){left[n]+=self.bank_left_[n];right[n]+=self.bank_right_[n];}
        }
        if(status!=noErr || self.playback_->failed){self.playback_->failed=true;self.playback_->playing=false;for(UInt32 i=0;i<data->mNumberBuffers;++i)if(data->mBuffers[i].mData)std::memset(data->mBuffers[i].mData,0,data->mBuffers[i].mDataByteSize);}
        if(status==noErr && !self.playback_->failed){
            if(data->mNumberBuffers==2){auto* left=static_cast<float*>(data->mBuffers[0].mData);auto* right=static_cast<float*>(data->mBuffers[1].mData);if(left && right){const auto count=std::min<UInt32>(frames,std::min(data->mBuffers[0].mDataByteSize,data->mBuffers[1].mDataByteSize)/sizeof(float));for(UInt32 n=0;n<count;++n)for(const auto& effect:self.playback_->effects)effect->process(left[n],right[n]);}}
            if(data->mNumberBuffers==2 && self.playback_->mono){auto* left=static_cast<float*>(data->mBuffers[0].mData);auto* right=static_cast<float*>(data->mBuffers[1].mData);if(left && right)for(UInt32 n=0;n<frames;++n)left[n]=right[n]=(left[n]+right[n])*0.5F;}
            const auto& p=*self.playback_;
            for(UInt32 i=0;i<data->mNumberBuffers;++i){auto& b=data->mBuffers[i];if(!b.mData)continue;auto* samples=static_cast<float*>(b.mData);const auto count=std::min<std::size_t>(b.mDataByteSize/sizeof(float),static_cast<std::size_t>(frames)*b.mNumberChannels);for(std::size_t n=0;n<count;++n){const auto factor=p.gain_remaining?p.gain+(p.gain_target-p.gain)*static_cast<float>(std::min<std::uint64_t>(n/b.mNumberChannels+1,p.gain_remaining))/static_cast<float>(p.gain_remaining):p.gain;const auto channel=data->mNumberBuffers==1?n%2:i%2;samples[n]*=p.active?factor*(channel?p.right_gain:p.left_gain):0.0F;}}
            if(self.playback_->gain_remaining){const auto step=std::min<std::uint64_t>(frames,self.playback_->gain_remaining);self.playback_->gain+=(self.playback_->gain_target-self.playback_->gain)*static_cast<float>(step)/static_cast<float>(self.playback_->gain_remaining);self.playback_->gain_remaining-=step;}
            self.playback_->rendered_frames+=frames;
            for(UInt32 i=0;i<data->mNumberBuffers;++i){const auto& buffer=data->mBuffers[i];if(!buffer.mData)continue;
                const auto* samples=static_cast<const float*>(buffer.mData);const auto count=std::min<std::size_t>(buffer.mDataByteSize/sizeof(float),static_cast<std::size_t>(frames)*buffer.mNumberChannels);
                for(std::size_t n=0;n<count;++n){const auto peak=std::abs(samples[n]);self.playback_->rendered_peak=std::max(self.playback_->rendered_peak,peak);const auto channel=data->mNumberBuffers==1?n%2:i%2;self.playback_->rendered_channel_peak[channel]=std::max(self.playback_->rendered_channel_peak[channel],peak);}}
        }
        if(self.capture_ && data->mNumberBuffers==2 && data->mBuffers[0].mData && data->mBuffers[1].mData)self.capture_->push_planar(static_cast<const float*>(data->mBuffers[0].mData),static_cast<const float*>(data->mBuffers[1].mData),frames);
        return noErr;
    }
    static void check(OSStatus status){if(status!=noErr)throw std::runtime_error("Core Audio MIDI synthesizer unavailable: "+std::to_string(status));}
    static AudioUnit make(OSType type,OSType subtype){AudioComponentDescription d{};d.componentType=type;d.componentSubType=subtype;d.componentManufacturer=kAudioUnitManufacturer_Apple;const auto c=AudioComponentFindNext(nullptr,&d);if(!c)throw std::runtime_error("Core Audio MIDI component missing");AudioUnit u{};check(AudioComponentInstanceNew(c,&u));return u;}
    static void dispose_bank(const BankUnit& bank){if(bank.unit){AudioUnitUninitialize(bank.unit);AudioComponentInstanceDispose(bank.unit);}if(!bank.path.empty())::unlink(bank.path.c_str());}
    void close(){if(output_){AudioOutputUnitStop(output_);AudioUnitUninitialize(output_);AudioComponentInstanceDispose(output_);output_=nullptr;if(std::getenv("X64ARM_TRACE_DMUSIC")){std::lock_guard lock(playback_->mutex);std::fprintf(stderr,"[midi] output frames=%llu peak_left=%g peak_right=%g failed=%u banks=%zu\n",static_cast<unsigned long long>(playback_->rendered_frames),playback_->rendered_channel_peak[0],playback_->rendered_channel_peak[1],playback_->failed,banks_.size());}}if(synth_){AudioUnitUninitialize(synth_);AudioComponentInstanceDispose(synth_);synth_=nullptr;}for(const auto& bank:banks_)dispose_bank(bank);banks_.clear();}
public:
    explicit Synth(std::shared_ptr<Playback> playback):playback_(std::move(playback)){
        try {
            synth_=make(kAudioUnitType_MusicDevice,kAudioUnitSubType_DLSSynth);output_=make(kAudioUnitType_Output,playback_->output_device.empty()?kAudioUnitSubType_DefaultOutput:kAudioUnitSubType_HALOutput);
            if(!playback_->output_device.empty()){const auto device=audio::device_for(playback_->output_device);if(device==kAudioObjectUnknown)throw std::runtime_error("MIDI 輸出裝置不存在");check(AudioUnitSetProperty(output_,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,sizeof(device)));}
            if(!playback_->effects.empty()){UInt32 disabled=0;check(AudioUnitSetProperty(synth_,kAudioUnitProperty_UsesInternalReverb,kAudioUnitScope_Global,0,&disabled,sizeof(disabled)));}
            AudioStreamBasicDescription f{};f.mSampleRate=48000;f.mFormatID=kAudioFormatLinearPCM;f.mFormatFlags=static_cast<UInt32>(kAudioFormatFlagsNativeFloatPacked)|static_cast<UInt32>(kAudioFormatFlagIsNonInterleaved);f.mBytesPerPacket=4;f.mFramesPerPacket=1;f.mBytesPerFrame=4;f.mChannelsPerFrame=2;f.mBitsPerChannel=32;
            check(AudioUnitSetProperty(synth_,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&f,sizeof(f)));
            check(AudioUnitSetProperty(output_,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&f,sizeof(f)));
            UInt32 maximum=4096;check(AudioUnitSetProperty(synth_,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,sizeof(maximum)));
            capture_=audio::OutputCapture::open("midi",48000,this);
            AURenderCallbackStruct callback{render,this};check(AudioUnitSetProperty(output_,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&callback,sizeof(callback)));
            check(AudioUnitInitialize(synth_));check(AudioUnitInitialize(output_));check(AudioOutputUnitStart(output_));
        }catch(...){close();throw;}
    }
    bool short_message(std::uint8_t status,std::uint8_t first,std::uint8_t second) override {
        std::lock_guard lock(playback_->mutex);return !playback_->failed && message(status,first,second,0);
    }
    bool system_exclusive(std::span<const std::uint8_t> bytes) override {
        std::lock_guard lock(playback_->mutex);return !playback_->failed && exclusive(bytes);
    }
    bool load_sound_bank(std::shared_ptr<const SoundBank> bank) override {
        if(!bank)return false;
        {std::lock_guard lock(playback_->mutex);if(std::any_of(banks_.begin(),banks_.end(),[&](const auto& x){return x.bank==bank;}))return true;if(banks_.size()>=32)return false;}
        const bool silent=std::all_of(bank->instruments.begin(),bank->instruments.end(),[&](const auto& i){return i.header+4<=bank->bytes.size() && std::all_of(bank->bytes.begin()+static_cast<std::ptrdiff_t>(i.header),bank->bytes.begin()+static_cast<std::ptrdiff_t>(i.header+4),[](auto b){return b==0;});});
        if(silent){std::lock_guard lock(playback_->mutex);banks_.push_back({nullptr,std::move(bank),{}});return true;}
        // Apple DLS 合成器依副檔名辨識音色庫；FSRef 不會取代這項格式檢查。
        auto pattern=(std::filesystem::temp_directory_path()/"x64arm-dls-XXXXXX.dls").string();std::vector<char> path(pattern.begin(),pattern.end());path.push_back(0);
        const auto fd=::mkstemps(path.data(),4);if(fd<0)return false;(void)::fcntl(fd,F_SETFD,FD_CLOEXEC);
        bool written=true;for(std::size_t at=0;at<bank->bytes.size();){const auto n=::write(fd,bank->bytes.data()+at,bank->bytes.size()-at);if(n<0 && errno==EINTR)continue;if(n<=0){written=false;break;}at+=static_cast<std::size_t>(n);}::close(fd);
        AudioUnit unit{};
        try {
            if(!written)throw std::runtime_error("DLS 暫存寫入失敗");unit=make(kAudioUnitType_MusicDevice,kAudioUnitSubType_DLSSynth);
            FSRef file{};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            check(FSPathMakeRef(reinterpret_cast<const UInt8*>(path.data()),&file,nullptr));
#pragma clang diagnostic pop
            check(AudioUnitSetProperty(unit,kMusicDeviceProperty_SoundBankFSRef,kAudioUnitScope_Global,0,&file,sizeof(file)));
            AudioStreamBasicDescription format{};UInt32 size=sizeof(format);check(AudioUnitGetProperty(synth_,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&format,&size));
            check(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&format,sizeof(format)));UInt32 maximum=4096;check(AudioUnitSetProperty(unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,sizeof(maximum)));
            if(!playback_->effects.empty()){UInt32 disabled=0;check(AudioUnitSetProperty(unit,kAudioUnitProperty_UsesInternalReverb,kAudioUnitScope_Global,0,&disabled,sizeof(disabled)));}
            check(AudioUnitInitialize(unit));
            std::lock_guard lock(playback_->mutex);for(const auto& event:controls_)if(event[0])check(MusicDeviceMIDIEvent(unit,event[0],event[1],event[2],0));
            banks_.push_back({unit,std::move(bank),path.data()});if(std::getenv("X64ARM_TRACE_DMUSIC"))std::fprintf(stderr,"[midi] sound-bank loaded instruments=%zu banks=%zu\n",banks_.back().bank->instruments.size(),banks_.size());return true;
        }catch(const std::exception& error){if(std::getenv("X64ARM_TRACE_DMUSIC"))std::fprintf(stderr,"[midi] sound-bank load failed: %s\n",error.what());if(unit){AudioUnitUninitialize(unit);AudioComponentInstanceDispose(unit);}::unlink(path.data());return false;}
    }
    bool unload_sound_bank(const std::shared_ptr<const SoundBank>& bank) override {
        BankUnit retired;
        {std::lock_guard lock(playback_->mutex);const auto found=std::find_if(banks_.begin(),banks_.end(),[&](const auto& x){return x.bank==bank;});if(found==banks_.end())return false;retired=std::move(*found);banks_.erase(found);}
        // render 不再能看到此 AU，於呼叫端釋放波形及暫存，不在音訊回呼中銷毀。
        dispose_bank(retired);return true;
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
