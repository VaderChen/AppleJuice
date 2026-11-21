#pragma once

#include "pcm_wave_format.hpp"
#include "x64arm/windows_gui.hpp"
#include <algorithm>
#include <optional>
#include <vector>

namespace x64arm {

// MCI 與 VFW 使用同一種預覽畫面及時間單位。時間以 100 ns 計，最後一個索引代表媒體結尾。
class VideoPreview {
public:
    struct AudioTrack {PcmWaveFormat format;std::vector<std::uint8_t> pcm;};
    virtual ~VideoPreview()=default;
    virtual std::uint64_t interval()const=0;
    virtual unsigned width()const=0;
    virtual unsigned height()const=0;
    virtual std::size_t frames()const=0;
    virtual std::optional<AudioTrack> take_audio()=0;
    virtual std::optional<GuiBitmap> at(std::size_t position)=0;
    virtual std::uint64_t time_at(std::uint64_t frame)const{return std::min<std::uint64_t>(frame,frames())*interval();}
    virtual std::uint64_t frame_at(std::uint64_t time)const{return std::min<std::uint64_t>(time/interval(),frames());}
    bool visual()const{return width()!=0 && height()!=0;}
};

}
