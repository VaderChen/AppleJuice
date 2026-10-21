#include "x64arm/audio.hpp"
#include "x64arm/audio_pcm.hpp"
#include <algorithm>
#include <cmath>
#include <bit>

namespace x64arm::audio {
void Buffer::stop(bool reset_effects) {
    if(playing)for(const auto& n:notifications)if(n.offset==UINT32_MAX &&
        std::find(pending_notifications.begin(),pending_notifications.end(),n.event)==pending_notifications.end())pending_notifications.push_back(n.event);
    playing=false;
    if(reset_effects)for(auto& effect:effects)effect->reset();
}
void Buffer::capture(std::span<const float> in,unsigned input_rate){
    std::lock_guard lock(mutex);if(!playing || pcm.empty() || !input_rate)return;
    for(std::size_t i=0;i+1<in.size() && playing;i+=2){
        capture_fraction+=static_cast<double>(rate)/input_rate;
        while(capture_fraction>=1 && playing){capture_fraction-=1;const auto frame=static_cast<std::size_t>(cursor),offset=frame*frame_bytes();
            for(unsigned c=0;c<channels;++c){const auto value=std::clamp(channels==1?(in[i]+in[i+1])*0.5F:in[i+c],-1.0F,1.0F);const auto at=offset+c*(bits/8);
                if(floating){const auto word=std::bit_cast<std::uint32_t>(value);for(unsigned b=0;b<4;++b)pcm[at+b]=static_cast<std::uint8_t>(word>>(b*8));}
                else if(bits==8)pcm[at]=static_cast<std::uint8_t>(std::clamp(value*128+128,0.0F,255.0F));
                else {const auto sample=static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(value*32768,-32768.0F,32767.0F)));pcm[at]=static_cast<std::uint8_t>(sample);pcm[at+1]=static_cast<std::uint8_t>(sample>>8);}
            }
            ++captured_frames;cursor+=1;
            for(const auto& n:notifications)if(n.offset!=UINT32_MAX && n.offset>=offset && n.offset<offset+frame_bytes() && std::find(pending_notifications.begin(),pending_notifications.end(),n.event)==pending_notifications.end())pending_notifications.push_back(n.event);
            if(static_cast<std::size_t>(cursor)*frame_bytes()>=pcm.size()){cursor=0;if(!looping)stop();}
        }
    }
}
void Buffer::render(std::span<float> out,unsigned output_rate) {
    std::fill(out.begin(),out.end(),0.0F);
    std::lock_guard lock(mutex);
    rendered_frames+=out.size()/2;
    if((!playing && effects.empty()) || !output_rate || pcm.empty())return;
    const auto frames=pcm.size()/frame_bytes();
    const auto advance=[&](double previous){
        for(const auto& n:notifications){
            if(n.offset==UINT32_MAX)continue;
            const double target=static_cast<double>(n.offset)/frame_bytes();
            const bool hit=(target>=previous && target<cursor) || (looping && cursor>=static_cast<double>(frames) && (cursor-previous>=static_cast<double>(frames) || target< std::fmod(cursor,static_cast<double>(frames))));
            if(hit && std::find(pending_notifications.begin(),pending_notifications.end(),n.event)==pending_notifications.end())pending_notifications.push_back(n.event);
        }
        if(cursor>=static_cast<double>(frames)){
            if(streaming)cursor=static_cast<double>(frames);
            else if(looping)cursor=std::fmod(cursor,static_cast<double>(frames));
            else {cursor=0;stop(false);}
        }
    };
    const auto left_gain=left*spatial_left,right_gain=right*spatial_right;
    if(effects.empty() && playing && frames && (channels==1 || channels==2) &&
       ((!floating && (bits==8 || bits==16)) || (floating && bits==32)) &&
       std::isfinite(cursor) && cursor>=0 && cursor<static_cast<double>(frames) &&
       std::isfinite(left_gain) && std::isfinite(right_gain) && static_cast<double>(frequency)*doppler/output_rate==1){
        std::size_t done=0;const auto requested=out.size()/2;
        while(done<requested && playing && cursor<static_cast<double>(frames)){
            const auto frame=static_cast<std::size_t>(cursor);auto count=std::min(requested-done,frames-frame);
            // 在最早通知所在 frame 結束區塊，保持未排序通知與重複 event 的原有順序。
            for(const auto& n:notifications)if(n.offset!=UINT32_MAX){const auto target=static_cast<double>(n.offset)/frame_bytes();
                if(target>=cursor && target<cursor+static_cast<double>(count))count=std::min(count,static_cast<std::size_t>(target-cursor)+1);}
            const auto peak=detail::pcm_block(pcm.data()+frame*frame_bytes(),out.data()+done*2,count,bits,channels,left_gain,right_gain);
            rendered_peak=std::max({rendered_peak,peak[0],peak[1]});rendered_left_peak=std::max(rendered_left_peak,peak[0]);rendered_right_peak=std::max(rendered_right_peak,peak[1]);
            const auto previous=cursor+static_cast<double>(count-1);cursor+=static_cast<double>(count);done+=count;advance(previous);
        }
        // 自然停止後保留原逐 frame 乘法的 signed zero；串流耗盡仍保持填入的 +0。
        if(!playing && !streaming)for(std::size_t frame=done;frame<requested;++frame){out[frame*2]=0.F*left_gain;out[frame*2+1]=0.F*right_gain;}
        return;
    }
    const auto sample=[&](std::size_t frame,unsigned channel) {
        const auto at=frame*frame_bytes()+channel*(bits/8);
        if(floating){std::uint32_t word=0;for(unsigned b=0;b<4;++b)word|=static_cast<std::uint32_t>(pcm[at+b])<<(b*8);const auto value=std::bit_cast<float>(word);return std::isfinite(value)?std::clamp(value,-1.0F,1.0F):0.0F;}
        if(bits==8)return (static_cast<float>(pcm[at])-128.0F)/128.0F;
        const auto raw=static_cast<std::uint16_t>(pcm[at]|(static_cast<unsigned>(pcm[at+1])<<8));
        return static_cast<float>(static_cast<std::int16_t>(raw))/32768.0F;
    };
    for(std::size_t i=0;i+1<out.size();i+=2){
        // 串流的 queue 耗盡時輸出靜音，保留播放狀態與讀取游標，讓 API 補入下一包。
        if(streaming && cursor>=static_cast<double>(frames))continue;
        if(!playing){for(auto& effect:effects)effect->process(out[i],out[i+1],spatial_distance);out[i]*=left*spatial_left;out[i+1]*=right*spatial_right;rendered_peak=std::max({rendered_peak,std::abs(out[i]),std::abs(out[i+1])});continue;}
        const auto frame=static_cast<std::size_t>(cursor);
        out[i]=sample(frame,0);out[i+1]=sample(frame,channels==1?0:1);
        for(auto& effect:effects)effect->process(out[i],out[i+1],spatial_distance);
        out[i]*=left*spatial_left;out[i+1]*=right*spatial_right;
        rendered_peak=std::max({rendered_peak,std::abs(out[i]),std::abs(out[i+1])});
        rendered_left_peak=std::max(rendered_left_peak,std::abs(out[i]));rendered_right_peak=std::max(rendered_right_peak,std::abs(out[i+1]));
        const double previous=cursor;
        cursor+=static_cast<double>(frequency)*doppler/output_rate;
        advance(previous);
    }
}
}
