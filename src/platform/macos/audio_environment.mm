#include "x64arm/audio_environment.hpp"
#include "x64arm/audio_channels.hpp"
#import <AVFAudio/AVFAudio.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace x64arm::audio {
namespace {
class Environment final:public EnvironmentRenderer {
    AVAudioEngine* engine_;
    AVAudioEnvironmentNode* environment_;
    AVAudioEngineManualRenderingBlock render_;
    AVAudioPCMBuffer* output_;
    AVAudioSourceNode* object_;
    unsigned channels_,maximum_,frames_{};
    double sample_time_{};
    std::span<const float> input_;
    bool source_error_{};
    static void require(bool okay,const char* what){if(!okay)throw std::runtime_error(what);}
public:
    Environment(unsigned channels,unsigned mask,unsigned rate,unsigned maximum):channels_(channels),maximum_(maximum){
        require(valid_channel_layout(channels,mask) && rate>=8000 && rate<=192000 && maximum && maximum<=4096,"空間混音聲道或取樣格式不支援");
        mask=mask?mask:channel_mask(channels);
        engine_=[AVAudioEngine new];environment_=[AVAudioEnvironmentNode new];
        [engine_ attachNode:environment_];
        auto* stereo=[[AVAudioFormat alloc] initStandardFormatWithSampleRate:rate channels:2];
        auto* mono=[[AVAudioFormat alloc] initStandardFormatWithSampleRate:rate channels:1];
        NSError* error=nil;
        require([engine_ enableManualRenderingMode:AVAudioEngineManualRenderingModeRealtime format:stereo maximumFrameCount:maximum error:&error],"無法設定 AVAudioEngine 手動即時混音");
        [engine_ connect:environment_ to:engine_.mainMixerNode fromBus:0 toBus:0 format:stereo];
        environment_.outputType=AVAudioEnvironmentOutputTypeHeadphones;
        environment_.distanceAttenuationParameters.rolloffFactor=0;
        environment_.reverbParameters.enable=NO;
        if(@available(macOS 15.0,*))environment_.listenerHeadTrackingEnabled=NO;
        require([environment_.applicableRenderingAlgorithms containsObject:@(AVAudio3DMixingRenderingAlgorithmHRTF)],"AVAudioEnvironmentNode 沒有 HRTF 演算法");
        unsigned spatial_bus=0,direct_bus=1;
        for(unsigned channel=0;channel<channels;++channel){
            const auto position=speaker_position(speaker_at(mask,channel));auto* owner=this;
            auto* node=[[AVAudioSourceNode alloc] initWithFormat:mono renderBlock:^OSStatus(BOOL* silence,const AudioTimeStamp* timestamp,AVAudioFrameCount count,AudioBufferList* output){
                const double offset=timestamp->mSampleTime-owner->sample_time_;
                if(!std::isfinite(offset) || offset<0 || offset>owner->frames_ || count>owner->frames_-static_cast<unsigned>(offset) ||
                   output->mNumberBuffers!=1 || !output->mBuffers[0].mData || output->mBuffers[0].mDataByteSize<count*sizeof(float)){
                    owner->source_error_=true;return kAudio_ParamError;
                }
                auto* data=static_cast<float*>(output->mBuffers[0].mData);const auto first=static_cast<unsigned>(offset);bool zero=true;
                for(unsigned i=0;i<count;++i){data[i]=owner->input_[(first+i)*owner->channels_+channel];zero=zero && data[i]==0;}
                *silence=zero;return noErr;
            }];
            [engine_ attachNode:node];
            if(position.lfe){
                // LFE 無方向性，直接送雙耳，保留低頻內容；不用另一個 HRTF 音源冒充方向。
                [engine_ connect:node to:engine_.mainMixerNode fromBus:0 toBus:direct_bus++ format:mono];node.volume=.70710678F;
            }else{
                [engine_ connect:node to:environment_ fromBus:0 toBus:spatial_bus++ format:mono];
                node.position=AVAudioMake3DPoint(position.x,position.y,position.z);
                node.renderingAlgorithm=AVAudio3DMixingRenderingAlgorithmHRTF;
                node.reverbBlend=0;
                if(channels==1)object_=node;
            }
        }
        output_=[[AVAudioPCMBuffer alloc] initWithPCMFormat:stereo frameCapacity:maximum];
        [engine_ prepare];require([engine_ startAndReturnError:&error],"無法啟動 AVAudioEnvironmentNode 混音");
        render_=engine_.manualRenderingBlock;
    }
    ~Environment()override{[engine_ stop];}
    void position(float x,float y,float z)noexcept override {
        if(object_)object_.position=AVAudioMake3DPoint(x,y,z);
    }
    bool render(std::span<const float> input,std::span<float> output) noexcept override {
        const auto count=output.size()/2;
        if(!count || count>maximum_ || output.size()!=count*2 || input.size()!=count*channels_)return false;
        input_=input;frames_=static_cast<unsigned>(count);sample_time_=static_cast<double>(engine_.manualRenderingSampleTime);source_error_=false;
        output_.frameLength=frames_;
        OSStatus error=0;const auto status=render_(frames_,output_.mutableAudioBufferList,&error);
        if(status!=AVAudioEngineManualRenderingStatusSuccess || error || source_error_){std::fill(output.begin(),output.end(),0);return false;}
        for(std::size_t i=0;i<count;++i){output[i*2]=output_.floatChannelData[0][i];output[i*2+1]=output_.floatChannelData[1][i];}
        return true;
    }
};
}
std::unique_ptr<EnvironmentRenderer> make_environment_renderer(unsigned channels,unsigned mask,unsigned rate,unsigned maximum){
    @autoreleasepool {return std::make_unique<Environment>(channels,mask,rate,maximum);}
}
}
