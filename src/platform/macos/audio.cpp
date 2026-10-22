#include "x64arm/audio.hpp"
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace x64arm::audio {
namespace {
class CoreAudioOutput final : public Output {
    AudioUnit unit_{};
    std::shared_ptr<Buffer> buffer_;
    // WASAPI 串流以串流本身的取樣率開啟輸出單元，取樣率轉換由 Core Audio 的轉換器完成；
    // 其他來源（DirectSound、WinMM）維持 48 kHz 混音並沿用既有的頻率／Doppler 處理。
    unsigned rate_=48000;
    // 診斷只保存有界視窗；即時 callback 不配置記憶體或寫入檔案。
    struct TraceWindow {std::uint64_t end{};unsigned frames{},nonzero{};float left{},right{};};
    const bool trace_enabled_=std::getenv("X64ARM_TRACE_AUDIO")!=nullptr;
    std::unique_ptr<std::array<TraceWindow,512>> trace_=trace_enabled_?std::make_unique<std::array<TraceWindow,512>>():nullptr;
    TraceWindow trace_current_{};std::uint64_t trace_count_{},trace_frames_{};
    void observe(std::span<const float> samples){
        if(!trace_enabled_)return;
        for(std::size_t i=0;i+1<samples.size();i+=2){
            const auto left=std::abs(samples[i]),right=std::abs(samples[i+1]);
            trace_current_.left=std::max(trace_current_.left,left);trace_current_.right=std::max(trace_current_.right,right);
            trace_current_.nonzero+=left>0 || right>0;++trace_current_.frames;++trace_frames_;
            if(trace_current_.frames==rate_){trace_current_.end=trace_frames_;(*trace_)[trace_count_++%trace_->size()]=trace_current_;trace_current_={};}
        }
    }
    void report()const{
        if(!trace_enabled_)return;
        const auto emit=[&](const TraceWindow& window){std::fprintf(stderr,"[coreaudio-window] output=%p end_frame=%llu frames=%u nonzero_frames=%u peak_left=%g peak_right=%g\n",static_cast<const void*>(this),static_cast<unsigned long long>(window.end),window.frames,window.nonzero,static_cast<double>(window.left),static_cast<double>(window.right));};
        for(auto i=trace_count_>trace_->size()?trace_count_-trace_->size():0;i<trace_count_;++i)emit((*trace_)[i%trace_->size()]);
        if(trace_current_.frames){auto window=trace_current_;window.end=trace_frames_;emit(window);}
    }
    static OSStatus render(void* context,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32 frames,AudioBufferList* data) {
        auto& self=*static_cast<CoreAudioOutput*>(context);
        if(data->mNumberBuffers!=1 || !data->mBuffers[0].mData || data->mBuffers[0].mDataByteSize<frames*2*sizeof(float))return kAudio_ParamError;
        self.buffer_->render({static_cast<float*>(data->mBuffers[0].mData),static_cast<std::size_t>(frames)*2},self.rate_);
        self.observe({static_cast<const float*>(data->mBuffers[0].mData),static_cast<std::size_t>(frames)*2});
        return noErr;
    }
    void close() {if(unit_){AudioOutputUnitStop(unit_);AudioUnitUninitialize(unit_);AudioComponentInstanceDispose(unit_);unit_=nullptr;}}
public:
    // device 為 kAudioObjectUnknown 時使用 DefaultOutput，隨系統切換預設裝置；否則以 HAL 輸出綁定指定裝置。
    explicit CoreAudioOutput(std::shared_ptr<Buffer> buffer,AudioDeviceID device=kAudioObjectUnknown):buffer_(std::move(buffer)) {
        // 呼叫端（例如 WASAPI 的 Initialize）已持有 buffer mutex；本單元的轉譯回呼在建構完成前尚未啟動，直接讀取即可，不可再上鎖。
        if(buffer_->streaming && buffer_->rate>=8000 && buffer_->rate<=192000)rate_=buffer_->rate;
        AudioComponentDescription desc{};desc.componentType=kAudioUnitType_Output;desc.componentSubType=device==kAudioObjectUnknown?kAudioUnitSubType_DefaultOutput:kAudioUnitSubType_HALOutput;desc.componentManufacturer=kAudioUnitManufacturer_Apple;
        const auto component=AudioComponentFindNext(nullptr,&desc);
        if(!component || AudioComponentInstanceNew(component,&unit_)!=noErr)throw std::runtime_error("Core Audio output unavailable");
        if(device!=kAudioObjectUnknown && AudioUnitSetProperty(unit_,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,sizeof(device))!=noErr){close();throw std::runtime_error("Core Audio output device unavailable");}
        AudioStreamBasicDescription format{};format.mSampleRate=rate_;format.mFormatID=kAudioFormatLinearPCM;format.mFormatFlags=kAudioFormatFlagsNativeFloatPacked;
        format.mBytesPerPacket=8;format.mFramesPerPacket=1;format.mBytesPerFrame=8;format.mChannelsPerFrame=2;format.mBitsPerChannel=32;
        AURenderCallbackStruct callback{render,this};
        if(AudioUnitSetProperty(unit_,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,sizeof(format))!=noErr ||
           AudioUnitSetProperty(unit_,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&callback,sizeof(callback))!=noErr ||
           AudioUnitInitialize(unit_)!=noErr || AudioOutputUnitStart(unit_)!=noErr){close();throw std::runtime_error("Core Audio output initialization failed");}
    }
    ~CoreAudioOutput() override {close();report();}
};
AudioDeviceID input_device(){AudioDeviceID device=kAudioObjectUnknown;UInt32 size=sizeof(device);const AudioObjectPropertyAddress property{kAudioHardwarePropertyDefaultInputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};if(AudioObjectGetPropertyData(kAudioObjectSystemObject,&property,0,nullptr,&size,&device)!=noErr)return kAudioObjectUnknown;return device;}
class CoreAudioInput final:public Input {
    AudioUnit unit_{};std::shared_ptr<Buffer> buffer_;std::vector<float> samples_,stereo_=std::vector<float>(4096*2);unsigned rate_{},channels_{};
    static OSStatus receive(void* context,AudioUnitRenderActionFlags* flags,const AudioTimeStamp* time,UInt32,UInt32 frames,AudioBufferList*){
        auto& self=*static_cast<CoreAudioInput*>(context);if(frames>4096){std::lock_guard lock(self.buffer_->mutex);self.buffer_->capture_failed=true;self.buffer_->stop();return kAudio_ParamError;}
        AudioBufferList data{};data.mNumberBuffers=1;data.mBuffers[0].mNumberChannels=self.channels_;data.mBuffers[0].mDataByteSize=frames*self.channels_*4;data.mBuffers[0].mData=self.samples_.data();
        const auto status=AudioUnitRender(self.unit_,flags,time,1,frames,&data);
        if(status==noErr){for(unsigned i=0;i<frames;++i){self.stereo_[i*2]=self.samples_[i*self.channels_];self.stereo_[i*2+1]=self.samples_[i*self.channels_+(self.channels_>1?1U:0U)];}self.buffer_->capture(std::span<const float>(self.stereo_).first(static_cast<std::size_t>(frames)*2),self.rate_);}
        else {std::lock_guard lock(self.buffer_->mutex);self.buffer_->capture_failed=true;self.buffer_->stop();}return noErr;
    }
    static void check(OSStatus status){if(status!=noErr)throw std::runtime_error("Core Audio 錄音初始化失敗："+std::to_string(status));}
    void close(){if(unit_){AudioOutputUnitStop(unit_);AudioUnitUninitialize(unit_);AudioComponentInstanceDispose(unit_);unit_=nullptr;}}
public:
    explicit CoreAudioInput(std::shared_ptr<Buffer> buffer):buffer_(std::move(buffer)){
        try {
            const auto device=input_device();if(device==kAudioObjectUnknown)throw std::runtime_error("Core Audio 預設錄音裝置不可用");
            AudioComponentDescription desc{};desc.componentType=kAudioUnitType_Output;desc.componentSubType=kAudioUnitSubType_HALOutput;desc.componentManufacturer=kAudioUnitManufacturer_Apple;
            const auto component=AudioComponentFindNext(nullptr,&desc);if(!component)throw std::runtime_error("Core Audio HAL input 不可用");check(AudioComponentInstanceNew(component,&unit_));
            UInt32 enable=1,disable=0;check(AudioUnitSetProperty(unit_,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Input,1,&enable,sizeof(enable)));check(AudioUnitSetProperty(unit_,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Output,0,&disable,sizeof(disable)));check(AudioUnitSetProperty(unit_,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,sizeof(device)));
            AudioStreamBasicDescription format{};UInt32 size=sizeof(format);check(AudioUnitGetProperty(unit_,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,1,&format,&size));rate_=static_cast<unsigned>(format.mSampleRate);channels_=format.mChannelsPerFrame;if(!rate_ || !channels_ || channels_>64)throw std::runtime_error("Core Audio 錄音格式無效");samples_.resize(4096*channels_);
            format.mFormatID=kAudioFormatLinearPCM;format.mFormatFlags=kAudioFormatFlagsNativeFloatPacked;format.mBytesPerPacket=channels_*4;format.mFramesPerPacket=1;format.mBytesPerFrame=channels_*4;format.mBitsPerChannel=32;
            check(AudioUnitSetProperty(unit_,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,1,&format,sizeof(format)));UInt32 maximum=4096;check(AudioUnitSetProperty(unit_,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,sizeof(maximum)));
            AURenderCallbackStruct callback{receive,this};check(AudioUnitSetProperty(unit_,kAudioOutputUnitProperty_SetInputCallback,kAudioUnitScope_Global,0,&callback,sizeof(callback)));check(AudioUnitInitialize(unit_));check(AudioOutputUnitStart(unit_));
        }catch(...){close();throw;}
    }
    ~CoreAudioInput()override{close();}
};
}
bool output_available(){AudioComponentDescription d{};d.componentType=kAudioUnitType_Output;d.componentSubType=kAudioUnitSubType_DefaultOutput;d.componentManufacturer=kAudioUnitManufacturer_Apple;return AudioComponentFindNext(nullptr,&d)!=nullptr;}
std::unique_ptr<Output> open_output(std::shared_ptr<Buffer> buffer){return std::make_unique<CoreAudioOutput>(std::move(buffer));}
namespace {
template<class T>bool device_value(AudioObjectID object,AudioObjectPropertySelector selector,AudioObjectPropertyScope scope,T& value){
    const AudioObjectPropertyAddress address{selector,scope,kAudioObjectPropertyElementMain};UInt32 size=sizeof(T);
    return AudioObjectGetPropertyData(object,&address,0,nullptr,&size,&value)==noErr && size==sizeof(T);
}
std::u16string cf_text(CFStringRef text){
    std::u16string out;if(!text)return out;const auto length=CFStringGetLength(text);
    if(length>0 && length<32768){out.resize(static_cast<std::size_t>(length));CFStringGetCharacters(text,CFRangeMake(0,length),reinterpret_cast<UniChar*>(out.data()));}
    CFRelease(text);return out;
}
unsigned channels(AudioDeviceID device,AudioObjectPropertyScope scope){
    const AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration,scope,kAudioObjectPropertyElementMain};UInt32 size=0;
    if(AudioObjectGetPropertyDataSize(device,&address,0,nullptr,&size)!=noErr || size<sizeof(AudioBufferList) || size>65536)return 0;
    std::vector<std::uint8_t> storage(size);auto* list=reinterpret_cast<AudioBufferList*>(storage.data());
    if(AudioObjectGetPropertyData(device,&address,0,nullptr,&size,list)!=noErr)return 0;
    unsigned total=0;for(UInt32 i=0;i<list->mNumberBuffers;++i)total+=list->mBuffers[i].mNumberChannels;return total;
}
AudioDeviceID device_for(std::string_view uid){
    CFStringRef text=CFStringCreateWithBytes(nullptr,reinterpret_cast<const UInt8*>(uid.data()),static_cast<CFIndex>(uid.size()),kCFStringEncodingUTF8,false);if(!text)return kAudioObjectUnknown;
    AudioDeviceID device=kAudioObjectUnknown;AudioValueTranslation translation{&text,sizeof(text),&device,sizeof(device)};UInt32 size=sizeof(translation);
    const AudioObjectPropertyAddress address{kAudioHardwarePropertyDeviceForUID,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    const auto status=AudioObjectGetPropertyData(kAudioObjectSystemObject,&address,0,nullptr,&size,&translation);CFRelease(text);
    return status==noErr?device:kAudioObjectUnknown;
}
}
std::unique_ptr<Output> open_output(std::shared_ptr<Buffer> buffer,std::string_view uid){
    if(uid.empty())return open_output(std::move(buffer));
    const auto device=device_for(uid);if(device==kAudioObjectUnknown)throw std::runtime_error("Core Audio output device not present");
    return std::make_unique<CoreAudioOutput>(std::move(buffer),device);
}
std::vector<DeviceInfo> devices(){
    const AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};UInt32 size=0;
    if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&address,0,nullptr,&size)!=noErr || size>4096*sizeof(AudioDeviceID))return {};
    std::vector<AudioDeviceID> ids(size/sizeof(AudioDeviceID));if(!ids.empty() && AudioObjectGetPropertyData(kAudioObjectSystemObject,&address,0,nullptr,&size,ids.data())!=noErr)return {};
    ids.resize(size/sizeof(AudioDeviceID));
    AudioDeviceID default_output=kAudioObjectUnknown,default_input=kAudioObjectUnknown;
    (void)device_value(kAudioObjectSystemObject,kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,default_output);
    (void)device_value(kAudioObjectSystemObject,kAudioHardwarePropertyDefaultInputDevice,kAudioObjectPropertyScopeGlobal,default_input);
    std::vector<DeviceInfo> result;
    for(const auto id:ids){
        UInt32 hidden=0;if(device_value(id,kAudioDevicePropertyIsHidden,kAudioObjectPropertyScopeGlobal,hidden) && hidden)continue;
        CFStringRef uid=nullptr;if(!device_value(id,kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,uid) || !uid)continue;
        DeviceInfo info;{std::array<char,1024> text{};const bool converted=CFStringGetCString(uid,text.data(),text.size(),kCFStringEncodingUTF8);CFRelease(uid);if(!converted)continue;info.uid=text.data();}
        CFStringRef name=nullptr,maker=nullptr;
        if(device_value(id,kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal,name))info.name=cf_text(name);
        if(device_value(id,kAudioObjectPropertyManufacturer,kAudioObjectPropertyScopeGlobal,maker))info.manufacturer=cf_text(maker);
        info.output_channels=channels(id,kAudioDevicePropertyScopeOutput);info.input_channels=channels(id,kAudioDevicePropertyScopeInput);
        Float64 rate=0;if(device_value(id,kAudioDevicePropertyNominalSampleRate,kAudioObjectPropertyScopeGlobal,rate))info.sample_rate=rate;
        UInt32 transport=0;if(device_value(id,kAudioDevicePropertyTransportType,kAudioObjectPropertyScopeGlobal,transport))info.transport=transport;
        info.default_output=id==default_output;info.default_input=id==default_input;
        if(info.output_channels || info.input_channels)result.push_back(std::move(info));
    }
    return result;
}
bool input_available(){return input_device()!=kAudioObjectUnknown;}
std::unique_ptr<Input> open_input(std::shared_ptr<Buffer> buffer){return std::make_unique<CoreAudioInput>(std::move(buffer));}
}
