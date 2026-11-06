#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace x64arm::mf {
// 由 sink 要求的 PCM 決定輸出；AAC 解碼、重取樣與聲道對應都交給宿主 AudioConverter，保留立體聲資料。
struct AudioInput {bool aac{};std::vector<std::uint8_t> config;unsigned rate{},channels{},bits{},align{};bool floating{};};
struct AudioOutput {
    unsigned rate{},channels{},bits{16};bool floating{};
    unsigned frame_bytes()const{return channels*(bits/8U);}
    bool operator==(const AudioOutput&)const=default;
};
class AudioTranscoder {
public:
#ifdef __APPLE__
    AudioTranscoder(const AudioInput& input,const AudioOutput& output):input_(input),output_(output){
        if(!output.rate || !output.channels || !output.frame_bytes())throw std::invalid_argument("音訊輸出格式無效");
        AudioStreamBasicDescription source{},target{};
        target.mSampleRate=output.rate;target.mFormatID=kAudioFormatLinearPCM;target.mChannelsPerFrame=output.channels;target.mBitsPerChannel=output.bits;target.mFramesPerPacket=1;
        target.mBytesPerFrame=target.mBytesPerPacket=output.frame_bytes();target.mFormatFlags=kAudioFormatFlagIsPacked|(output.floating?kAudioFormatFlagIsFloat:output.bits>8?kAudioFormatFlagIsSignedInteger:0);
        std::vector<std::uint8_t> cookie;
        if(input.aac){
            if(input.config.empty())throw std::invalid_argument("AAC 缺少 AudioSpecificConfig");
            cookie=esds_cookie(input.config);source.mFormatID=kAudioFormatMPEG4AAC;source.mSampleRate=input.rate;source.mChannelsPerFrame=input.channels;source.mFramesPerPacket=1024;
            UInt32 size=sizeof(source);
            if(AudioFormatGetProperty(kAudioFormatProperty_FormatInfo,static_cast<UInt32>(cookie.size()),cookie.data(),&size,&source)!=noErr)throw std::runtime_error("AAC 設定無法解析");
        }else{
            source.mSampleRate=input.rate;source.mFormatID=kAudioFormatLinearPCM;source.mChannelsPerFrame=input.channels;source.mBitsPerChannel=input.bits;source.mFramesPerPacket=1;
            source.mBytesPerFrame=source.mBytesPerPacket=input.align;source.mFormatFlags=kAudioFormatFlagIsPacked|(input.floating?kAudioFormatFlagIsFloat:input.bits>8?kAudioFormatFlagIsSignedInteger:0);
        }
        if(AudioConverterNew(&source,&target,&converter_)!=noErr)throw std::runtime_error("無法建立音訊轉換器");
        if(!cookie.empty() && AudioConverterSetProperty(converter_,kAudioConverterDecompressionMagicCookie,static_cast<UInt32>(cookie.size()),cookie.data())!=noErr){AudioConverterDispose(converter_);converter_=nullptr;throw std::runtime_error("無法套用 AAC 設定");}
    }
    ~AudioTranscoder(){if(converter_)AudioConverterDispose(converter_);}
    AudioTranscoder(const AudioTranscoder&)=delete;
    AudioTranscoder& operator=(const AudioTranscoder&)=delete;
    std::vector<std::uint8_t> push(std::span<const std::uint8_t> packet){pending_.assign(packet.begin(),packet.end());retained_.clear();return pull();}
    std::vector<std::uint8_t> drain(){pending_.clear();retained_.clear();draining_=true;auto bytes=pull();draining_=false;return bytes;}
    void reset(){pending_.clear();retained_.clear();draining_=false;if(converter_ && AudioConverterReset(converter_)!=noErr)throw std::runtime_error("音訊轉換器重設失敗");}
private:
    static constexpr OSStatus more_input=1;
    static std::vector<std::uint8_t> esds_cookie(std::span<const std::uint8_t> config){
        const auto size=static_cast<unsigned>(config.size());std::vector<std::uint8_t> bytes;
        const auto tag=[&](std::uint8_t id,unsigned length){bytes.push_back(id);for(int shift=21;shift>0;shift-=7)bytes.push_back(static_cast<std::uint8_t>(((length>>shift)&0x7fU)|0x80U));bytes.push_back(static_cast<std::uint8_t>(length&0x7fU));};
        tag(3,3+5+13+5+size);bytes.insert(bytes.end(),{0,0,0});
        tag(4,13+5+size);bytes.insert(bytes.end(),{0x40,0x15,0,0,0,0,0,0,0,0,0,0,0});
        tag(5,size);bytes.insert(bytes.end(),config.begin(),config.end());return bytes;
    }
    static OSStatus supply(AudioConverterRef,UInt32* count,AudioBufferList* buffers,AudioStreamPacketDescription** packets,void* opaque){
        auto& self=*static_cast<AudioTranscoder*>(opaque);
        buffers->mNumberBuffers=1;buffers->mBuffers[0].mNumberChannels=self.input_.channels;
        if(self.pending_.empty()){*count=0;buffers->mBuffers[0].mData=nullptr;buffers->mBuffers[0].mDataByteSize=0;return self.draining_?noErr:more_input;}
        // 回呼傳回的資料須存活到下一次回呼。
        self.retained_=std::exchange(self.pending_,{});buffers->mBuffers[0].mData=self.retained_.data();buffers->mBuffers[0].mDataByteSize=static_cast<UInt32>(self.retained_.size());
        if(self.input_.aac){
            *count=1;self.description_={0,0,static_cast<UInt32>(self.retained_.size())};if(packets)*packets=&self.description_;
        }else *count=static_cast<UInt32>(self.retained_.size()/self.input_.align);
        return noErr;
    }
    std::vector<std::uint8_t> pull(){
        std::vector<std::uint8_t> result,chunk(8192U*output_.frame_bytes());
        for(;;){
            UInt32 frames=8192;AudioBufferList list{};list.mNumberBuffers=1;list.mBuffers[0].mNumberChannels=output_.channels;list.mBuffers[0].mData=chunk.data();list.mBuffers[0].mDataByteSize=static_cast<UInt32>(chunk.size());
            const auto status=AudioConverterFillComplexBuffer(converter_,supply,this,&frames,&list,nullptr);
            if(status!=noErr && status!=more_input)throw std::runtime_error("音訊轉換失敗");
            result.insert(result.end(),chunk.begin(),chunk.begin()+static_cast<std::ptrdiff_t>(frames*output_.frame_bytes()));
            if(status==more_input || !frames)return result;
        }
    }
    AudioInput input_;AudioOutput output_;AudioConverterRef converter_{};
    std::vector<std::uint8_t> pending_,retained_;AudioStreamPacketDescription description_{};bool draining_{};
#else
    AudioTranscoder(const AudioInput&,const AudioOutput&){throw std::runtime_error("此平台沒有音訊轉換器");}
    std::vector<std::uint8_t> push(std::span<const std::uint8_t>){return {};}
    std::vector<std::uint8_t> drain(){return {};}
    void reset(){}
#endif
};
}
