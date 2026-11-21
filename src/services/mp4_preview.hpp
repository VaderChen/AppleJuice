#pragma once

#include "avi_frame_image.hpp"
#include "mf_audio_transcoder.hpp"
#include "mf_h264.hpp"
#include "mf_mp4_demux.hpp"
#include "video_preview.hpp"
#include "x64arm/readonly_files.hpp"
#include <cstdlib>
#include <cstdio>
#include <map>

namespace x64arm {

// MCI 共用 Media Foundation 的 MP4 取樣表、VideoToolbox 與 AudioConverter。檔案透過已開啟的沙箱描述元讀取。
class Mp4Preview final : public VideoPreview {
public:
    static std::unique_ptr<Mp4Preview> open(const OpenHostFile& file){
        if(file.error || !file.file || (file.metadata.attributes&0x10U) || file.metadata.size<8)return {};
        std::array<std::uint8_t,8> signature{};
        read(*file.file,0,signature);
        if(!mf::mp4_signature(signature))return {};
        auto movie=mf::read_mp4_movie(file.metadata.size,[&](std::uint64_t offset,std::span<std::uint8_t> bytes){
            read(*file.file,offset,bytes);
        });
        return std::unique_ptr<Mp4Preview>(new Mp4Preview(file.file,std::move(movie)));
    }
    std::uint64_t interval()const override{return interval_;}
    unsigned width()const override{return track_?track_->width:0;}
    unsigned height()const override{return track_?track_->height:0;}
    std::size_t frames()const override{return track_?times_.size():static_cast<std::size_t>((duration_+interval_-1)/interval_);}
    std::uint64_t time_at(std::uint64_t frame)const override{
        if(frame>=frames())return duration_;
        if(track_)return frame?static_cast<std::uint64_t>(times_.at(static_cast<std::size_t>(frame))):0;
        return frame*interval_;
    }
    std::uint64_t frame_at(std::uint64_t time)const override{
        if(time>=duration_)return frames();
        if(!track_)return time/interval_;
        const auto after=std::upper_bound(times_.begin(),times_.end(),static_cast<std::int64_t>(time));
        return after==times_.begin()?0:static_cast<std::uint64_t>(after-times_.begin()-1);
    }
    std::optional<AudioTrack> take_audio() override{return std::exchange(audio_,std::nullopt);}
    std::optional<GuiBitmap> at(std::size_t position) override{
        try{return decode_at(position);}
        catch(const video::VideoCodecError& error){
            if(!hardware_)throw;
            use_software(error.what());
            return decode_at(position);
        }
    }
private:
    std::optional<GuiBitmap> decode_at(std::size_t position){
        if(!track_ || position>=frames())return {};
        const auto target=times_[position];
        if(!cache_.contains(target) && (!started_ || position<cursor_ || position-cursor_>8)){
            const auto after=std::upper_bound(keys_.begin(),keys_.end(),target,
                [](std::int64_t value,const auto& point){return value<point.first;});
            submitted_=after==keys_.begin()?0:std::prev(after)->second;
            if(started_){
                if(hardware_)hardware_->flush();else software_->flush();
            }
            cache_.clear();
            drained_=false;
            started_=true;
        }
        cache_.erase(cache_.begin(),cache_.lower_bound(target));
        const auto retain=[&](std::vector<video::DecodedFrame> frames){
            for(auto& frame:frames)
                if(frame.pts.timescale==10000000 && frame.pts.value>=target &&
                    frame.width==width() && frame.height==height())cache_.try_emplace(frame.pts.value,std::move(frame));
            while(cache_.size()>32)cache_.erase(std::prev(cache_.end()));
        };
        while(submitted_<track_->samples.size() && !cache_.contains(target)){
            const auto& sample=track_->samples[submitted_];
            const auto data=packet(sample);
            if(hardware_)retain(hardware_->decode(mf::h264_packet(data,nal_size_),
                {sample.time,10000000},{sample.duration,10000000}));
            else retain(software_->decode(data,sample.time,sample.key));
            ++submitted_;
        }
        if(!drained_ && submitted_==track_->samples.size() && !cache_.contains(target)){
            retain(hardware_?hardware_->drain():software_->drain());
            drained_=true;
        }
        cursor_=position;
        const auto found=cache_.find(target);
        if(found==cache_.end())return {};
        found->second.materialize();
        const auto rgba=avi::frame_rgba(found->second);
        if(rgba.empty())return {};
        GuiBitmap bitmap{width(),height(),std::vector<std::uint32_t>(static_cast<std::size_t>(width())*height())};
        for(std::size_t i=0;i<bitmap.pixels.size();++i)bitmap.pixels[i]=avi::word(rgba,i*4);
        return bitmap;
    }
    void use_software(const char* reason){
        software_=std::make_unique<media::VideoDecoder>(media::CompressedVideo{0x34363248U,width(),height(),track_->avcc});
        hardware_.reset();
        started_=false;
        if(std::getenv("X64ARM_TRACE_MCI_VIDEO"))
            std::fprintf(stderr,"[mci-video] codec=H264 decoder=FFmpeg hardware=0 reason=%s\n",reason);
    }
    static void read(HostFile& file,std::uint64_t offset,std::span<std::uint8_t> output){
        while(!output.empty()){
            const auto part=file.read(output,offset);
            if(part.error || !part.bytes || part.bytes>output.size())throw std::runtime_error("視訊檔案讀取失敗");
            output=output.subspan(part.bytes);
            offset+=part.bytes;
        }
    }
    std::vector<std::uint8_t> packet(const mf::Mp4Sample& sample)const{
        if(sample.size>16U*1024U*1024U)throw std::invalid_argument("視訊取樣大小無效");
        std::vector<std::uint8_t> data(sample.size);
        read(*file_,sample.offset,data);
        return data;
    }
    explicit Mp4Preview(std::shared_ptr<HostFile> file,std::shared_ptr<const mf::Mp4Movie> movie):
        file_(std::move(file)),movie_(std::move(movie)){
        if(movie_->duration<=0)throw std::invalid_argument("MP4 時間長度無效");
        duration_=static_cast<std::uint64_t>(movie_->duration);
        const mf::Mp4Track* audio=nullptr;
        for(const auto& stream:movie_->tracks){
            if(!track_ && stream.video && (stream.codec==mf::fourcc('a','v','c','1') ||
                stream.codec==mf::fourcc('a','v','c','3')))track_=&stream;
            if(!audio && stream.audio && stream.codec==mf::fourcc('m','p','4','a') && !stream.audio_config.empty())audio=&stream;
        }
        if(track_){
            if(!width() || !height() || (width()&1U) || (height()&1U) ||
               std::uint64_t{width()}*height()>16U*1024U*1024U)throw std::invalid_argument("MP4 畫面尺寸無效");
            times_.assign(std::lower_bound(track_->presentation.begin(),track_->presentation.end(),0),track_->presentation.end());
            if(times_.empty())throw std::invalid_argument("MP4 沒有可呈現畫面");
            if(track_->rate_num)interval_=std::max<std::uint64_t>(1,10000000ULL*track_->rate_den/track_->rate_num);
            for(std::size_t i=0;i<track_->samples.size();++i)
                if(track_->samples[i].key)keys_.emplace_back(track_->samples[i].time,i);
            std::sort(keys_.begin(),keys_.end());
            auto config=mf::h264_config(track_->avcc);
            if(config.parameter_sets.size()<2)throw std::invalid_argument("MP4 缺少 H.264 參數集");
            nal_size_=config.nal_length_size;
            config.nal_length_size=4;
            config.retain_native_frames=true;
            if(video::hardware_decode_supported(video::Codec::H264)){
                try{
                    hardware_=std::make_unique<video::HardwareDecoder>(config);
                    if(std::getenv("X64ARM_TRACE_MCI_VIDEO"))
                        std::fprintf(stderr,"[mci-video] codec=H264 decoder=VideoToolbox hardware=%u\n",unsigned(hardware_->hardware_accelerated()));
                }catch(const video::VideoCodecError& error){use_software(error.what());}
            }else use_software("VideoToolbox hardware unavailable");
        }
        if(audio)audio_=decode_audio(*audio);
        if(!track_ && !audio_)throw std::invalid_argument("MP4 沒有可解碼的軌道");
    }
    AudioTrack decode_audio(const mf::Mp4Track& track){
        if(track.sample_rate<8000 || track.sample_rate>192000 || !track.channels || track.channels>8)
            throw std::invalid_argument("MP4 音訊格式無效");
        constexpr std::uint64_t second=10000000;
        constexpr std::size_t limit=128U*1024U*1024U;
        const auto frames_at=[&](std::int64_t ticks){return static_cast<std::int64_t>(static_cast<__int128>(ticks)*track.sample_rate/second);};
        const auto end=frames_at(track.duration);
        if(end<0 || static_cast<std::uint64_t>(end)>limit/8)throw std::length_error("MP4 解碼音訊超出預覽容量");
        AudioTrack decoded{{2,track.sample_rate,32,8,true},{}};
        mf::AudioInput input{true,track.audio_config,track.sample_rate,track.channels};
        mf::AudioTranscoder converter(input,{track.sample_rate,2,32,true});
        auto cursor=frames_at(track.samples.front().time);
        const auto append=[&](std::vector<std::uint8_t> block){
            const auto count=static_cast<std::int64_t>(block.size()/8);
            const auto start=std::max<std::int64_t>(0,cursor);
            const auto stop=std::min(end,cursor+count);
            if(stop>start){
                if(decoded.pcm.size()<static_cast<std::size_t>(start)*8)decoded.pcm.resize(static_cast<std::size_t>(start)*8);
                const auto begin=(start-cursor)*8;
                const auto length=(stop-start)*8;
                decoded.pcm.insert(decoded.pcm.end(),block.begin()+begin,block.begin()+begin+length);
            }
            cursor+=count;
        };
        for(const auto& sample:track.samples)append(converter.push(packet(sample)));
        append(converter.drain());
        if(decoded.pcm.empty())throw std::invalid_argument("MP4 音訊沒有可解碼樣本");
        return decoded;
    }
    std::shared_ptr<HostFile> file_;
    std::shared_ptr<const mf::Mp4Movie> movie_;
    const mf::Mp4Track* track_{};
    std::vector<std::int64_t> times_;
    std::vector<std::pair<std::int64_t,std::size_t>> keys_;
    std::unique_ptr<video::HardwareDecoder> hardware_;
    std::unique_ptr<media::VideoDecoder> software_;
    std::map<std::int64_t,video::DecodedFrame> cache_;
    std::optional<AudioTrack> audio_;
    std::uint64_t interval_{10000},duration_{};
    std::size_t cursor_{},submitted_{};
    unsigned nal_size_{};
    bool started_{},drained_{};
};

}
