#pragma once

#include "avi_frame_image.hpp"
#include "video_preview.hpp"
#include "x64arm/readonly_files.hpp"
#include <cstring>
#include <map>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

namespace x64arm {

// libavformat 只從沙箱已授權的描述元取得內容。視訊及音訊交由共用的 FFmpeg 解碼後端。
class MpegPreview final : public VideoPreview {
public:
    static std::unique_ptr<MpegPreview> open(const OpenHostFile& file){
        if(!file.file || file.metadata.attributes&0x10U || !file.metadata.size || file.metadata.size>INT64_MAX)return {};
        Input input{*file.file,static_cast<std::int64_t>(file.metadata.size)};
        if(!input.open())return {};
        auto media=std::make_unique<MpegPreview>();
        if(!media->load(input.format))return {};
        return media;
    }
    std::uint64_t interval()const override{return interval_;}
    unsigned width()const override{return video_.width;}
    unsigned height()const override{return video_.height;}
    std::size_t frames()const override{return visual()?times_.size():static_cast<std::size_t>((duration_+interval_-1)/interval_);}
    std::uint64_t time_at(std::uint64_t frame)const override{
        if(frame>=frames())return duration_;
        return visual()?(frame?static_cast<std::uint64_t>(times_[static_cast<std::size_t>(frame)]):0):frame*interval_;
    }
    std::uint64_t frame_at(std::uint64_t time)const override{
        if(time>=duration_)return frames();
        if(!visual())return time/interval_;
        const auto after=std::upper_bound(times_.begin(),times_.end(),static_cast<std::int64_t>(time));
        return after==times_.begin()?0:static_cast<std::uint64_t>(after-times_.begin()-1);
    }
    std::optional<AudioTrack> take_audio() override{return std::exchange(audio_,std::nullopt);}
    std::optional<GuiBitmap> at(std::size_t position) override{
        if(!visual() || position>=times_.size())return {};
        const auto target=times_[position];
        if(!cache_.contains(target) && (!decoder_ || position<previous_ || position-previous_>8)){
            auto end=std::upper_bound(keys_.begin(),keys_.end(),target,
                [](std::int64_t time,const auto& entry){return time<entry.first;});
            submitted_=end==keys_.begin()?0:std::prev(end)->second;
            if(decoder_)decoder_->flush();else decoder_=std::make_unique<media::VideoDecoder>(video_);
            drained_=false;
            cache_.clear();
        }
        cache_.erase(cache_.begin(),cache_.lower_bound(target));
        const auto retain=[&](std::vector<video::DecodedFrame> frames){
            for(auto& frame:frames)
                if(frame.pts.timescale==10000000 && frame.pts.value>=target && frame.width==width() && frame.height==height())
                    cache_.try_emplace(frame.pts.value,std::move(frame));
            while(cache_.size()>32)cache_.erase(std::prev(cache_.end()));
        };
        while(submitted_<video_packets_.size() && !cache_.contains(target)){
            const auto& packet=video_packets_[submitted_++];
            retain(decoder_->decode(packet.data,packet.time,packet.key));
        }
        if(!drained_ && submitted_==video_packets_.size() && !cache_.contains(target)){
            retain(decoder_->drain());
            drained_=true;
        }
        previous_=position;
        const auto found=cache_.find(target);
        if(found==cache_.end())return {};
        const auto rgba=avi::frame_rgba(found->second);
        if(rgba.empty())return {};
        GuiBitmap bitmap{width(),height(),std::vector<std::uint32_t>(static_cast<std::size_t>(width())*height())};
        for(std::size_t i=0;i<bitmap.pixels.size();++i)bitmap.pixels[i]=avi::word(rgba,i*4);
        return bitmap;
    }
private:
    struct Input {
        HostFile& file;
        const std::int64_t size;
        std::int64_t position{};
        AVIOContext* io{};
        AVFormatContext* format{};
        ~Input(){
            avformat_close_input(&format);
            if(io){av_freep(&io->buffer);avio_context_free(&io);}
        }
        static int read(void* opaque,std::uint8_t* buffer,int count){
            auto& self=*static_cast<Input*>(opaque);
            if(count<=0)return AVERROR(EINVAL);
            if(self.position>=self.size)return AVERROR_EOF;
            try{
                const auto read_size=static_cast<std::size_t>(std::min<std::int64_t>(count,self.size-self.position));
                const auto part=self.file.read(std::span(buffer,read_size),static_cast<std::uint64_t>(self.position));
                if(part.error || part.bytes>read_size)return AVERROR(EIO);
                if(!part.bytes)return AVERROR_EOF;
                self.position+=static_cast<std::int64_t>(part.bytes);
                return static_cast<int>(part.bytes);
            }catch(...){return AVERROR(EIO);}
        }
        static std::int64_t seek(void* opaque,std::int64_t displacement,int whence){
            auto& self=*static_cast<Input*>(opaque);
            whence&=~AVSEEK_FORCE;
            if(whence==AVSEEK_SIZE)return self.size;
            if(whence!=SEEK_SET && whence!=SEEK_CUR && whence!=SEEK_END)return AVERROR(EINVAL);
            const auto base=whence==SEEK_SET?0:whence==SEEK_CUR?self.position:self.size;
            const auto next=static_cast<__int128>(base)+displacement;
            if(next<0 || next>self.size)return AVERROR(EINVAL);
            self.position=static_cast<std::int64_t>(next);
            return self.position;
        }
        bool open(){
            constexpr int buffer_size=32768;
            auto* buffer=static_cast<std::uint8_t*>(av_malloc(buffer_size));
            if(!buffer)throw std::bad_alloc();
            io=avio_alloc_context(buffer,buffer_size,0,this,read,nullptr,seek);
            if(!io){av_free(buffer);throw std::bad_alloc();}
            format=avformat_alloc_context();
            if(!format)throw std::bad_alloc();
            format->pb=io;
            format->flags|=AVFMT_FLAG_CUSTOM_IO|AVFMT_FLAG_GENPTS;
            return avformat_open_input(&format,nullptr,nullptr,nullptr)>=0 && avformat_find_stream_info(format,nullptr)>=0;
        }
    };
    struct Packet {
        std::int64_t time{},duration{};
        std::vector<std::uint8_t> data;
        bool key{};
    };
    struct PacketFree {void operator()(AVPacket* packet)const{av_packet_free(&packet);}};
    static constexpr AVRational clock_units{1,10000000};
    static constexpr std::size_t compressed_limit=256U*1024U*1024U,pcm_limit=128U*1024U*1024U;
    bool load(AVFormatContext* input){
        const auto video_index=av_find_best_stream(input,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
        const auto audio_index=av_find_best_stream(input,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
        int visual_stream=-1,sound_stream=-1;
        media::CompressedAudio sound;
        if(video_index>=0){
            AVStream& stream=*input->streams[static_cast<unsigned>(video_index)];
            const auto& format=*stream.codecpar;
            if((format.codec_id==AV_CODEC_ID_MPEG1VIDEO || format.codec_id==AV_CODEC_ID_MPEG2VIDEO) &&
               format.width>0 && format.height>0 && std::int64_t{format.width}*format.height<=16*1024*1024){
                visual_stream=video_index;
                video_.fourcc=format.codec_id==AV_CODEC_ID_MPEG1VIDEO?0x3147504dU:0x3247504dU;
                video_.width=static_cast<unsigned>(format.width);
                video_.height=static_cast<unsigned>(format.height);
                if(format.extradata_size>0)video_.extra.assign(format.extradata,format.extradata+format.extradata_size);
                const auto fps=av_guess_frame_rate(input,&stream,nullptr);
                interval_=fps.num>0 && fps.den>0?static_cast<std::uint64_t>(av_rescale_q(1,av_inv_q(fps),clock_units)):400000;
            }
        }
        if(audio_index>=0){
            const auto& format=*input->streams[static_cast<unsigned>(audio_index)]->codecpar;
            if((format.codec_id==AV_CODEC_ID_MP2 || format.codec_id==AV_CODEC_ID_MP3) &&
                format.sample_rate>=8000 && format.sample_rate<=192000 &&
                (format.ch_layout.nb_channels==1 || format.ch_layout.nb_channels==2)){
                sound_stream=audio_index;
                sound.format_tag=format.codec_id==AV_CODEC_ID_MP2?0x50:0x55;
                sound.sample_rate=static_cast<unsigned>(format.sample_rate);
                sound.channels=static_cast<std::uint16_t>(format.ch_layout.nb_channels);
            }
        }
        if(visual_stream<0 && sound_stream<0)return false;
        std::vector<Packet> sound_packets;
        auto packet=std::unique_ptr<AVPacket,PacketFree>(av_packet_alloc());
        if(!packet)throw std::bad_alloc();
        std::size_t bytes=0;
        for(;;){
            const auto result=av_read_frame(input,packet.get());
            if(result==AVERROR_EOF)break;
            if(result<0)throw std::runtime_error("MPEG 串流讀取失敗");
            const bool is_video=packet->stream_index==visual_stream;
            if((is_video || packet->stream_index==sound_stream) && packet->size>0){
                const auto size=static_cast<std::size_t>(packet->size);
                if(size>compressed_limit-bytes)throw std::length_error("MPEG 串流超出預覽容量");
                bytes+=size;
                auto& packets=is_video?video_packets_:sound_packets;
                const auto base=input->streams[static_cast<unsigned>(packet->stream_index)]->time_base;
                auto duration=packet->duration>0?av_rescale_q(packet->duration,base,clock_units):0;
                if(duration<=0)duration=is_video?static_cast<std::int64_t>(interval_):1152LL*10000000/sound.sample_rate;
                const auto pts=packet->pts!=AV_NOPTS_VALUE?packet->pts:packet->dts;
                const auto time=pts!=AV_NOPTS_VALUE?av_rescale_q(pts,base,clock_units):
                    packets.empty()?0:packets.back().time+packets.back().duration;
                constexpr std::int64_t ticks_per_day=86400LL*10000000;
                if(time< -ticks_per_day || time>ticks_per_day || duration>ticks_per_day)
                    throw std::invalid_argument("MPEG 時間戳記超出支援範圍");
                packets.push_back({time,duration,{packet->data,packet->data+size},bool(packet->flags&AV_PKT_FLAG_KEY)});
            }
            av_packet_unref(packet.get());
        }
        if(video_packets_.empty()){video_={};interval_=10000;}
        if(video_packets_.empty() && sound_packets.empty())return false;
        const auto earliest=[](const std::vector<Packet>& packets){
            const auto first=std::min_element(packets.begin(),packets.end(),
                [](const Packet& a,const Packet& b){return a.time<b.time;});
            return first==packets.end()?INT64_MAX:first->time;
        };
        const auto origin=std::min(earliest(video_packets_),earliest(sound_packets));
        for(const auto packets:{&video_packets_,&sound_packets}){
            for(auto& sample:*packets){
                sample.time-=origin;
                duration_=std::max(duration_,static_cast<std::uint64_t>(sample.time+sample.duration));
            }
        }
        for(std::size_t i=0;i<video_packets_.size();++i){
            const auto& sample=video_packets_[i];
            times_.push_back(sample.time);
            if(sample.key)keys_.emplace_back(sample.time,i);
        }
        std::sort(times_.begin(),times_.end());
        std::sort(keys_.begin(),keys_.end());
        if(!sound_packets.empty())audio_=decode_audio(sound,sound_packets);
        return duration_!=0;
    }
    static AudioTrack decode_audio(const media::CompressedAudio& format,const std::vector<Packet>& packets){
        media::AudioDecoder decoder(format);
        AudioTrack audio{{2,format.sample_rate,32,8,true},{}};
        const auto append=[&](std::vector<media::DecodedAudio> blocks){
            for(const auto& block:blocks){
                if(block.rate!=format.sample_rate || (block.channels!=1 && block.channels!=2))
                    throw std::invalid_argument("MPEG 音訊格式於串流中變更");
                if(block.frames()>(pcm_limit-audio.pcm.size())/8)throw std::length_error("MPEG 解碼音訊超出預覽容量");
                const auto offset=audio.pcm.size();
                audio.pcm.resize(offset+block.frames()*8);
                if(block.channels==2)std::memcpy(audio.pcm.data()+offset,block.samples.data(),block.frames()*8);
                else for(std::size_t frame=0;frame<block.frames();++frame)
                    for(unsigned channel=0;channel<2;++channel)
                        std::memcpy(audio.pcm.data()+offset+frame*8+channel*4,&block.samples[frame],4);
            }
        };
        const auto padding=static_cast<std::uint64_t>(packets.front().time)*format.sample_rate/10000000;
        if(padding>pcm_limit/8)throw std::length_error("MPEG 音訊起點超出預覽容量");
        audio.pcm.resize(static_cast<std::size_t>(padding)*8);
        const auto start=audio.pcm.size();
        for(const auto& packet:packets)append(decoder.decode(packet.data,packet.time));
        append(decoder.drain());
        if(audio.pcm.size()==start)throw std::invalid_argument("MPEG 音訊沒有可解碼樣本");
        return audio;
    }
    media::CompressedVideo video_;
    std::vector<Packet> video_packets_;
    std::vector<std::int64_t> times_;
    std::vector<std::pair<std::int64_t,std::size_t>> keys_;
    std::unique_ptr<media::VideoDecoder> decoder_;
    std::map<std::int64_t,video::DecodedFrame> cache_;
    std::optional<AudioTrack> audio_;
    std::uint64_t interval_{10000},duration_{};
    std::size_t submitted_{},previous_{};
    bool drained_{};
};

}
