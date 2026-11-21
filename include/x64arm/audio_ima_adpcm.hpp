#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace x64arm::audio {
inline constexpr unsigned ima_adpcm_tag=0x11;
struct ImaAdpcmFormat {
    unsigned channels{},block_bytes{},samples_per_block{};
    bool valid()const{
        if(!channels || channels>2 || block_bytes<4*channels || block_bytes>65535)return false;
        const auto payload=block_bytes-4*channels;
        return payload%channels==0 && samples_per_block==1+payload/channels*2;
    }
};

namespace ima {
inline constexpr std::array steps{
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
    107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,
    796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,
    4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,
    16818,18500,20350,22385,24623,27086,29794,32767
};
inline int get_sample(std::span<const std::uint8_t> data,std::size_t at){
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(unsigned{data[at]}|(unsigned{data[at+1]}<<8)));
}
inline void put_sample(std::span<std::uint8_t> data,std::size_t at,int sample){
    const auto word=static_cast<std::uint16_t>(sample);
    data[at]=static_cast<std::uint8_t>(word);
    data[at+1]=static_cast<std::uint8_t>(word>>8);
}
struct Channel {
    int sample{};
    unsigned index{};
    int apply(unsigned nibble){
        const int step=steps[index];
        const int delta=(step>>3)+((nibble&1U)?step>>2:0)+((nibble&2U)?step>>1:0)+((nibble&4U)?step:0);
        sample=std::clamp(sample+((nibble&8U)?-delta:delta),-32768,32767);
        constexpr std::array adjustment{-1,-1,-1,-1,2,4,6,8};
        index=static_cast<unsigned>(std::clamp(static_cast<int>(index)+adjustment[nibble&7U],0,88));
        return sample;
    }
    unsigned encode(int target){
        int delta=target-sample;
        unsigned code=delta<0?8U:0U;
        if(delta<0)delta=-delta;
        const int step=steps[index];
        for(unsigned bit=4,shift=0;bit;bit>>=1,++shift){
            const int magnitude=step>>shift;
            if(delta>=magnitude){
                code|=bit;
                delta-=magnitude;
            }
        }
        apply(code);
        return code;
    }
};
}

// RIFF 的立體聲資料以每聲道最多四個位元組為一組，組內低 nibble 先解碼。
inline std::vector<std::uint8_t> decode_ima_adpcm_block(std::span<const std::uint8_t> block,const ImaAdpcmFormat& format){
    const auto channels=format.channels;
    const auto header=4*channels;
    if(!format.valid() || block.size()<header || block.size()>format.block_bytes)
        throw std::invalid_argument("IMA ADPCM 區塊無效");
    std::array<ima::Channel,2> state{};
    std::vector<std::uint8_t> pcm(std::size_t{format.samples_per_block}*channels*2);
    for(unsigned channel=0;channel<channels;++channel){
        const auto index=block[channel*4+2];
        if(index>=ima::steps.size())throw std::invalid_argument("IMA ADPCM 量化索引無效");
        state[channel]={ima::get_sample(block,channel*4),index};
        ima::put_sample(pcm,channel*2,state[channel].sample);
    }
    unsigned frame=1;
    for(unsigned offset=header;offset<block.size();offset+=4*channels){
        const auto group=std::min(4U,(format.block_bytes-offset)/channels);
        const auto last=offset+(channels-1)*group;
        if(last>=block.size())break;
        const auto available=static_cast<unsigned>(std::min<std::size_t>(group,block.size()-last));
        for(unsigned channel=0;channel<channels;++channel){
            for(unsigned i=0;i<available;++i){
                const auto packed=block[offset+channel*group+i];
                ima::put_sample(pcm,((frame+2*i)*channels+channel)*2,state[channel].apply(packed&15U));
                ima::put_sample(pcm,((frame+2*i+1)*channels+channel)*2,state[channel].apply(packed>>4));
            }
        }
        frame+=available*2;
    }
    pcm.resize(std::size_t{frame}*channels*2);
    return pcm;
}

inline void encode_ima_adpcm_block(std::span<const std::uint8_t> pcm,std::span<std::uint8_t> block,
                                  const ImaAdpcmFormat& format){
    const auto channels=format.channels;
    if(!format.valid() || block.size()!=format.block_bytes || pcm.empty() || pcm.size()%(2*channels) ||
       pcm.size()>std::size_t{format.samples_per_block}*channels*2)
        throw std::invalid_argument("IMA ADPCM 編碼區塊無效");
    const auto frames=static_cast<unsigned>(pcm.size()/(2*channels));
    const auto sample=[&](unsigned frame,unsigned channel){
        return ima::get_sample(pcm,(std::min(frame,frames-1)*channels+channel)*2);
    };
    std::array<ima::Channel,2> state{};
    for(unsigned channel=0;channel<channels;++channel){
        const int first=sample(0,channel),next=sample(1,channel);
        const int difference=first<next?next-first:first-next;
        const auto index=std::lower_bound(ima::steps.begin(),ima::steps.end(),std::min(difference,32767));
        state[channel]={first,static_cast<unsigned>(index-ima::steps.begin())};
        ima::put_sample(block,channel*4,first);
        block[channel*4+2]=static_cast<std::uint8_t>(state[channel].index);
        block[channel*4+3]=0;
    }
    unsigned frame=1;
    for(unsigned offset=4*channels;offset<block.size();offset+=4*channels){
        const auto group=std::min(4U,(format.block_bytes-offset)/channels);
        for(unsigned channel=0;channel<channels;++channel){
            for(unsigned i=0;i<group;++i){
                const auto low=state[channel].encode(sample(frame+2*i,channel));
                const auto high=state[channel].encode(sample(frame+2*i+1,channel));
                block[offset+channel*group+i]=static_cast<std::uint8_t>(low|(high<<4));
            }
        }
        frame+=group*2;
    }
}
}
