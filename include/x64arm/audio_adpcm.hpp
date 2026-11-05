#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace x64arm::audio {
// RIFF MS ADPCM 的純資料轉換。服務層負責客體記憶體與 API 各自的格式限制。
struct AdpcmFormat {
    unsigned channels{},rate{},block_bytes{},samples_per_block{};
    std::vector<std::array<std::int16_t,2>> coefficients;
};
inline constexpr std::array<std::array<std::int16_t,2>,7> adpcm_coefficients{{
    {256,0},{512,-256},{0,0},{192,64},{240,0},{460,-208},{392,-232}
}};
inline std::optional<AdpcmFormat> parse_adpcm_format(std::span<const std::uint8_t> bytes){
    if(bytes.size()<22)return {};
    const auto word=[&](unsigned p){return unsigned{bytes[p]}|(unsigned{bytes[p+1]}<<8);};
    const auto rate=word(4)|(word(6)<<16),extra=word(16),count=word(20);
    AdpcmFormat f{word(2),rate,word(12),word(18),{}};
    if(word(0)!=2 || word(14)!=4 || !f.channels || f.channels>2 || !f.rate ||
       !count || count>256 || extra<4+count*4 || bytes.size()<18U+extra ||
       f.block_bytes<7*f.channels || f.samples_per_block<2 ||
       2+(f.block_bytes-7*f.channels)*2/f.channels!=f.samples_per_block)return {};
    f.coefficients.reserve(count);
    for(unsigned i=0;i<count;++i)f.coefficients.push_back({
        std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(word(22+i*4))),
        std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(word(24+i*4)))});
    return f;
}
inline std::vector<std::uint8_t> decode_adpcm(std::span<const std::uint8_t> bytes,const AdpcmFormat& f){
    const auto valid=[&](bool condition){if(!condition)throw std::invalid_argument("ADPCM 格式或資料區塊無效");};
    valid(f.channels && f.channels<=2 && f.block_bytes>=7*f.channels && f.block_bytes<=65535 &&
          f.samples_per_block==2+(f.block_bytes-7*f.channels)*2/f.channels &&
          !f.coefficients.empty() && f.coefficients.size()<=256);
    const auto blocks=bytes.size()/f.block_bytes+(bytes.size()%f.block_bytes!=0);
    const auto block_output=std::size_t{f.samples_per_block}*f.channels*2;
    // 與共用 PCM 佇列的大小上限一致，先驗證擴張量再配置。
    if(blocks && block_output>0x80000000ULL/blocks)throw std::bad_alloc{};
    std::vector<std::uint8_t> output(blocks*block_output);
    constexpr unsigned adaptation[]{230,230,230,230,307,409,512,614,768,614,512,409,307,230,230,230};
    for(std::size_t block=0;block<blocks;++block){
        const auto input=bytes.subspan(block*f.block_bytes,std::min<std::size_t>(f.block_bytes,bytes.size()-block*f.block_bytes));
        valid(input.size()>=7*f.channels);
        const auto word=[&](unsigned p){return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(unsigned{input[p]}|(unsigned{input[p+1]}<<8)));};
        std::array<std::int64_t,2> step{};std::array<std::int32_t,2> recent{},previous{};
        const auto store=[&](unsigned frame,unsigned channel,std::int32_t value){
            const auto at=block*block_output+(std::size_t{frame}*f.channels+channel)*2;
            const auto bits=static_cast<std::uint16_t>(value);output[at]=static_cast<std::uint8_t>(bits);output[at+1]=static_cast<std::uint8_t>(bits>>8);
        };
        for(unsigned channel=0;channel<f.channels;++channel){
            valid(input[channel]<f.coefficients.size());step[channel]=word(f.channels+channel*2);valid(step[channel]>0);
            recent[channel]=word(f.channels*3+channel*2);previous[channel]=word(f.channels*5+channel*2);
            store(0,channel,previous[channel]);store(1,channel,recent[channel]);
        }
        const auto available=(input.size()-7*f.channels)*2;
        for(std::size_t sample=0;sample<available;++sample){
            const auto channel=static_cast<unsigned>(sample%f.channels),frame=2+static_cast<unsigned>(sample/f.channels);
            const auto packed=input[7*f.channels+sample/2];const unsigned nibble=sample%2?packed&15U:packed>>4;
            const auto& coef=f.coefficients[input[channel]];
            valid(step[channel]<=std::numeric_limits<std::int64_t>::max()/768);
            const auto prediction=(std::int64_t{recent[channel]}*coef[0]+std::int64_t{previous[channel]}*coef[1])/256;
            const auto value=std::clamp(prediction+(nibble<8?std::int64_t{nibble}:std::int64_t{nibble}-16)*step[channel],INT64_C(-32768),INT64_C(32767));
            previous[channel]=recent[channel];recent[channel]=static_cast<std::int32_t>(value);store(frame,channel,recent[channel]);
            step[channel]=std::max(INT64_C(16),step[channel]*adaptation[nibble]/256);
        }
        // 最後一個不完整區塊未包含的樣本保持為零，不延伸預測或讀越界。
    }
    return output;
}
}
