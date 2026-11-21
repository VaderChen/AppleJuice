#pragma once

#include <gsm.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace x64arm::acm {
inline constexpr unsigned gsm_tag=0x31,gsm_block_bytes=65,gsm_samples=320;

// WAV49 將兩個 160 樣本 GSM frame 合成一個 65-byte 區塊。編碼與解碼沿用各串流自己的預測狀態。
class GsmWav49 {
public:
    GsmWav49(){reset();}
    void reset(){
        Instance next(gsm_create(),gsm_destroy);
        if(!next)throw std::bad_alloc{};
        int enabled=1;
        if(gsm_option(next.get(),GSM_OPT_WAV49,&enabled)<0)
            throw std::invalid_argument("libgsm 未啟用 WAV49");
        instance_=std::move(next);
    }
    std::vector<std::uint8_t> decode(std::span<const std::uint8_t> block){
        if(block.size()!=gsm_block_bytes)throw std::invalid_argument("GSM WAV49 區塊必須完整");
        std::array<gsm_byte,gsm_block_bytes> encoded{};
        std::copy(block.begin(),block.end(),encoded.begin());
        std::array<gsm_signal,gsm_samples> samples{};
        if(gsm_decode(instance_.get(),encoded.data(),samples.data())<0 ||
           gsm_decode(instance_.get(),encoded.data()+33,samples.data()+160)<0)
            throw std::invalid_argument("GSM 音訊內容無效");
        std::vector<std::uint8_t> output(sizeof(samples));
        std::memcpy(output.data(),samples.data(),output.size());
        return output;
    }
    void encode(std::span<const std::uint8_t> pcm,std::span<std::uint8_t> block){
        std::array<gsm_signal,gsm_samples> samples{};
        if(pcm.empty() || pcm.size()%2 || pcm.size()>sizeof(samples) || block.size()!=gsm_block_bytes)
            throw std::invalid_argument("GSM WAV49 編碼區塊無效");
        std::memcpy(samples.data(),pcm.data(),pcm.size());
        gsm_encode(instance_.get(),samples.data(),block.data());
        gsm_encode(instance_.get(),samples.data()+160,block.data()+32);
    }
private:
    // 兩種客體 ABI 與本機編譯目標均使用 little-endian PCM16。
    static_assert(std::endian::native==std::endian::little && sizeof(gsm_signal)==2);
    using Instance=std::unique_ptr<gsm_state,decltype(&gsm_destroy)>;
    Instance instance_{nullptr,gsm_destroy};
};
}
