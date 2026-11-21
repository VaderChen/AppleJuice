#pragma once

#include "avi_run_length.hpp"
#include "video_preview.hpp"
#include "x64arm/readonly_files.hpp"
#include <utility>

namespace x64arm {

// 只讀取已映射的 AVI，依串流速率預覽。解碼共用 AVI 的 DIB 與 Windows Media 後端。
class AviPreviewClip final : public VideoPreview {
public:
    static std::unique_ptr<AviPreviewClip> open(ReadOnlyFiles& filesystem,std::string path,bool sound=false) {
        const auto file=filesystem.open_reader(std::move(path));
        constexpr std::uint64_t limit=128ULL*1024*1024;
        if(file.error || !file.file || (file.metadata.attributes&0x10U) ||
           !file.metadata.size || file.metadata.size>limit)return {};
        avi::Bytes bytes(static_cast<std::size_t>(file.metadata.size));
        std::size_t read=0;
        while(read<bytes.size()){
            const auto part=file.file->read(std::span(bytes).subspan(read),read);
            if(part.error || !part.bytes || part.bytes>bytes.size()-read)return {};
            read+=static_cast<std::size_t>(part.bytes);
        }
        auto movie=avi::decode(bytes);
        for(std::size_t index=0;index<movie.streams.size();++index){
            auto& stream=movie.streams[index];
            if(avi::word(stream.header,0)!=avi::video)continue;
            const auto layout=avi::frame_layout(stream.format);
            if(!layout || layout->width>8192 || layout->height>8192 ||
               std::uint64_t{layout->width}*layout->height>16U*1024*1024)continue;
            auto clip=std::make_unique<AviPreviewClip>();
            clip->layout_=*layout;
            clip->format_=std::move(stream.format);
            if(avi::is_run_length(*layout) || media::video1_fourcc(layout->compression)){
                clip->compressed_dib_=avi::frame_layout(avi::uncompressed_frame_header(clip->format_));
                if(!clip->compressed_dib_ || clip->compressed_dib_->top_down)continue;
            }
            for(auto& packet:movie.packets)
                if(packet.stream==index && (packet.id>>16==0x6264U || packet.id>>16==0x6364U))
                    clip->packets_.push_back(std::move(packet));
            if(clip->packets_.empty())continue;
            const auto scale=avi::word(stream.header,20),rate=avi::word(stream.header,24);
            const auto ticks=rate && scale?10000000ULL*scale/rate:10ULL*avi::word(movie.header,0);
            clip->interval_=std::clamp(ticks,100000ULL,600000000ULL);
            if(sound)clip->audio_=read_audio(movie);
            return clip;
        }
        return {};
    }

    std::uint64_t interval()const override{return interval_;}
    unsigned width()const override{return layout_.width;}
    unsigned height()const override{return layout_.height;}
    std::size_t frames()const override{return packets_.size();}
    std::optional<AudioTrack> take_audio() override{return std::exchange(audio_,std::nullopt);}
    std::optional<GuiBitmap> at(std::size_t position) override {
        if(position>=packets_.size())return {};
        if(position!=cursor_){
            cursor_=position;
            if(media::video_decoder_available(layout_.compression)){
                submitted_=position;
                while(submitted_ && !(packets_[submitted_].flags&0x10U))--submitted_;
                decoded_.clear();
                drained_=false;
                decoder_.reset();
            }
        }
        return next();
    }
    bool has_next()const{return cursor_<packets_.size();}
    std::optional<GuiBitmap> next() {
        if(!has_next())return {};
        const auto pos=cursor_++;
        const auto& packet=packets_[pos];
        const auto count=static_cast<std::size_t>(layout_.width)*layout_.height;
        GuiBitmap bitmap{layout_.width,layout_.height,std::vector<std::uint32_t>(count)};
        const auto draw_rows=[&](const avi::FrameLayout& dib,avi::View pixels){
            if(pixels.size()<dib.image_size())return false;
            const DibRowCodec codec(dib.depth,dib.masks,dib.palette);
            for(unsigned y=0;y<dib.height;++y){
                const auto row=static_cast<std::size_t>(dib.top_down?y:dib.height-y-1)*dib.stride;
                auto out=std::span(bitmap.pixels).subspan(static_cast<std::size_t>(y)*dib.width,dib.width);
                if(!codec.decode(pixels.subspan(row,dib.stride),out))return false;
            }
            return true;
        };
        if(layout_.raw()){
            if(!draw_rows(layout_,packet.bytes))return {};
        }else if(compressed_dib_){
            if(!draw_rows(*compressed_dib_,decode_compressed_dib(pos)))return {};
        }else{
            avi::Bytes rgba;
            if(media::video_decoder_available(layout_.compression))rgba=decode_media(pos);
            else if(!avi::decode_frame_rgba(format_,layout_,packet.bytes,rgba))return {};
            if(rgba.size()!=count*4)return {};
            for(std::size_t i=0;i<count;++i)bitmap.pixels[i]=avi::word(rgba,i*4);
        }
        for(auto& color:bitmap.pixels)color|=0xff000000U;
        return bitmap;
    }
private:
    avi::View decode_compressed_dib(std::size_t position){
        const bool video1=media::video1_fourcc(layout_.compression);
        if(compressed_frame_.empty() || position+1<dib_submitted_ || position>dib_submitted_){
            dib_submitted_=position;
            while(dib_submitted_ && !(packets_[dib_submitted_].flags&0x10U))--dib_submitted_;
            if(video1)video1_decoder_=avi::open_video1(format_,layout_);
            else compressed_frame_.assign(compressed_dib_->image_size(),0);
        }
        while(dib_submitted_<=position){
            const auto& packet=packets_[dib_submitted_++];
            const bool key=(packet.flags&0x10U)!=0;
            if(video1){
                compressed_frame_=video1_decoder_->decode(packet.bytes,key);
                if(compressed_frame_.size()!=compressed_dib_->image_size()){
                    compressed_frame_.clear();
                    return {};
                }
                continue;
            }
            if(key)std::fill(compressed_frame_.begin(),compressed_frame_.end(),0);
            if(!avi::decode_run_length(packet.bytes,*compressed_dib_,compressed_frame_)){
                compressed_frame_.clear();
                return {};
            }
        }
        return compressed_frame_;
    }
    static std::optional<AudioTrack> read_audio(const avi::Movie& movie){
        constexpr std::size_t limit=64U*1024*1024;
        for(std::size_t i=0;i<movie.streams.size();++i){
            const auto& stream=movie.streams[i];
            if(avi::word(stream.header,0)!=avi::audio)continue;
            const auto format=parse_pcm_wave_format(stream.format,true,true);
            if(!format)continue;
            AudioTrack track{*format,{}};
            for(const auto& packet:movie.packets)if(packet.stream==i && packet.id>>16==0x6277U){
                if(packet.bytes.size()>limit-track.pcm.size())return {};
                track.pcm.insert(track.pcm.end(),packet.bytes.begin(),packet.bytes.end());
            }
            if(!track.pcm.empty() && track.pcm.size()%track.format.align==0)return track;
        }
        return {};
    }
    avi::Bytes decode_media(std::size_t position) {
        if(!decoder_){
            media::CompressedVideo format{layout_.compression,layout_.width,layout_.height,{format_.begin()+40,format_.end()}};
            decoder_=std::make_unique<media::VideoDecoder>(format);
        }
        const auto frame_bytes=std::uint64_t{layout_.width}*layout_.height*4;
        const auto reserve=std::clamp<std::uint64_t>((128ULL*1024*1024)/frame_bytes,1,16);
        const auto retain=[&](const std::vector<video::DecodedFrame>& frames){
            for(const auto& decoded:frames){
                const auto index=static_cast<std::size_t>(decoded.pts.value);
                if((index==position || decoded_.size()<reserve) && decoded.pts.timescale &&
                   decoded.pts.value>=static_cast<std::int64_t>(position) &&
                   decoded.pts.value<static_cast<std::int64_t>(packets_.size()) &&
                   decoded.width==layout_.width && decoded.height==layout_.height)
                    decoded_.try_emplace(index,avi::frame_rgba(decoded));
            }
        };
        while(submitted_<packets_.size() && submitted_<=position+32 && !decoded_.contains(position)){
            const auto& packet=packets_[submitted_];
            retain(decoder_->decode(packet.bytes,static_cast<std::int64_t>(submitted_++),bool(packet.flags&0x10U)));
        }
        if(!drained_ && submitted_==packets_.size() && !decoded_.contains(position)){
            drained_=true;
            retain(decoder_->drain());
        }
        auto found=decoded_.extract(position);
        return found.empty()?avi::Bytes{}:std::move(found.mapped());
    }
    avi::FrameLayout layout_;
    avi::Bytes format_;
    std::vector<avi::Packet> packets_;
    std::uint64_t interval_{1000000};
    std::size_t cursor_{},submitted_{};
    bool drained_{};
    std::optional<AudioTrack> audio_;
    std::unique_ptr<media::VideoDecoder> decoder_;
    std::map<std::size_t,avi::Bytes> decoded_;
    std::optional<avi::FrameLayout> compressed_dib_;
    avi::Bytes compressed_frame_;
    std::unique_ptr<media::Video1Decoder> video1_decoder_;
    std::size_t dib_submitted_{};
};
}
