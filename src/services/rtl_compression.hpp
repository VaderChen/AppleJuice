#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// RtlCompressBuffer／RtlDecompressBuffer 的 LZNT1 與 XPRESS（純 LZ77）格式，依 MS-XCA 規格在宿主端原生實作。
namespace x64arm::rtl_compression {
// 尋找最長相符：以三位元組雜湊串列限制每個位置的候選數，避免平方成本。
class Matcher {
public:
    explicit Matcher(std::span<const std::uint8_t> data):data_(data),previous_(data.size(),-1){head_.fill(-1);}
    // 回傳（距離, 長度）；長度 < 3 表示沒有相符。window 為允許的最大距離，maximum 為允許的最大長度，begin 為不可越過的起點。
    std::pair<std::size_t,std::size_t> find(std::size_t at,std::size_t begin,std::size_t window,std::size_t maximum)const{
        std::size_t best_length=0,best_distance=0;if(at+3>data_.size() || maximum<3)return {0,0};
        unsigned tries=0;
        for(auto candidate=head_[hash(at)];candidate>=0 && tries<64;candidate=previous_[static_cast<std::size_t>(candidate)],++tries){
            const auto from=static_cast<std::size_t>(candidate);if(from<begin || at-from>window)break;
            std::size_t length=0;while(length<maximum && at+length<data_.size() && data_[from+length]==data_[at+length])++length;
            if(length>best_length){best_length=length;best_distance=at-from;if(length==maximum)break;}
        }
        return {best_distance,best_length};
    }
    void insert(std::size_t at){if(at+3>data_.size())return;const auto key=hash(at);previous_[at]=head_[key];head_[key]=static_cast<std::ptrdiff_t>(at);}
private:
    std::size_t hash(std::size_t at)const{return ((std::size_t{data_[at]}<<10)^(std::size_t{data_[at+1]}<<5)^data_[at+2])&4095U;}
    std::span<const std::uint8_t> data_;std::array<std::ptrdiff_t,4096> head_{};std::vector<std::ptrdiff_t> previous_;
};
// LZNT1 複製記號的位移位元數隨目前在區塊中的位置而增加：位置 ≤ 16 時位移 4 位元、長度 12 位元。
inline unsigned lznt1_length_bits(std::size_t position){unsigned bits=12;for(auto i=position-1;i>=0x10;i>>=1)--bits;return bits;}
// 每個區塊最多 4096 位元組；壓縮後不比原文小的完整區塊改存未壓縮區塊（標頭 0x3fff）。超過 limit 回 nullopt。
inline std::optional<std::vector<std::uint8_t>> lznt1_compress(std::span<const std::uint8_t> input,std::size_t limit){
    std::vector<std::uint8_t> output;Matcher matcher(input);
    for(std::size_t chunk=0;chunk<input.size();chunk+=4096){
        const auto size=std::min<std::size_t>(4096,input.size()-chunk);std::vector<std::uint8_t> body;
        for(std::size_t at=chunk;at<chunk+size;){
            const auto flag_at=body.size();body.push_back(0);
            for(unsigned bit=0;bit<8 && at<chunk+size;++bit){
                const auto position=at-chunk;std::pair<std::size_t,std::size_t> match{0,0};
                if(position){const auto bits=lznt1_length_bits(position);match=matcher.find(at,chunk,std::size_t{1}<<(16-bits),std::min((std::size_t{1}<<bits)+2,chunk+size-at));}
                if(match.second>=3){
                    const auto bits=lznt1_length_bits(position);const auto token=static_cast<std::uint16_t>(((match.first-1)<<bits)|(match.second-3));
                    body.push_back(static_cast<std::uint8_t>(token));body.push_back(static_cast<std::uint8_t>(token>>8));body[flag_at]|=static_cast<std::uint8_t>(1U<<bit);
                    for(std::size_t k=0;k<match.second;++k)matcher.insert(at+k);at+=match.second;
                }else {body.push_back(input[at]);matcher.insert(at);++at;}
            }
        }
        const bool raw=size==4096 && body.size()>=4096;const auto total=(raw?4096:body.size())+2;
        const auto header=static_cast<std::uint16_t>((raw?0:0x8000U)|0x3000U|(total-3));
        if(output.size()+total>limit)return std::nullopt;
        output.push_back(static_cast<std::uint8_t>(header));output.push_back(static_cast<std::uint8_t>(header>>8));
        if(raw)output.insert(output.end(),input.begin()+static_cast<std::ptrdiff_t>(chunk),input.begin()+static_cast<std::ptrdiff_t>(chunk+size));
        else output.insert(output.end(),body.begin(),body.end());
    }
    return output;
}
// 解壓到 limit 為止；遇到零標頭或資料結尾停止。格式錯誤回 nullopt。
inline std::optional<std::vector<std::uint8_t>> lznt1_decompress(std::span<const std::uint8_t> input,std::size_t limit){
    std::vector<std::uint8_t> output;std::size_t in=0;
    while(in+2<=input.size() && output.size()<limit){
        const auto header=static_cast<std::uint16_t>(input[in]|(input[in+1]<<8));if(!header)break;
        if(((header>>12)&7U)!=3U)return std::nullopt;
        const auto total=static_cast<std::size_t>(header&0x0fffU)+3;if(in+total>input.size())return std::nullopt;
        const auto body=input.subspan(in+2,total-2);in+=total;const auto start=output.size();
        if(!(header&0x8000U)){for(const auto b:body){if(output.size()>=limit)break;output.push_back(b);}continue;}
        for(std::size_t at=0;at<body.size() && output.size()<limit;){
            const auto flags=body[at++];
            for(unsigned bit=0;bit<8 && at<body.size() && output.size()<limit;++bit){
                if(!(flags&(1U<<bit))){output.push_back(body[at++]);continue;}
                if(at+2>body.size())return std::nullopt;
                const auto token=static_cast<std::uint16_t>(body[at]|(body[at+1]<<8));at+=2;
                const auto position=output.size()-start;if(!position)return std::nullopt;
                const auto bits=lznt1_length_bits(position);const std::size_t distance=(token>>bits)+1U,length=(token&((1U<<bits)-1U))+3U;
                if(distance>position)return std::nullopt;
                for(std::size_t k=0;k<length && output.size()<limit;++k)output.push_back(output[output.size()-distance]);
            }
        }
    }
    return output;
}
// XPRESS：32 位元旗標字由高位元往低位元使用，1 為相符；相符長度 ≥ 10 時以共用的半位元組、再以位元組／16／32 位元延伸。
inline std::optional<std::vector<std::uint8_t>> xpress_compress(std::span<const std::uint8_t> input,std::size_t limit){
    std::vector<std::uint8_t> output;Matcher matcher(input);std::size_t flag_at=0,nibble_at=0;std::uint32_t flags=0;unsigned count=0;bool open=false;
    const auto put32=[&](std::size_t at,std::uint32_t value){for(unsigned i=0;i<4;++i)output[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
    const auto add=[&](bool match){if(!open){flag_at=output.size();output.insert(output.end(),4,0);open=true;}flags=(flags<<1)|(match?1U:0U);if(++count==32){put32(flag_at,flags);flags=0;count=0;open=false;}};
    for(std::size_t at=0;at<input.size();){
        const auto match=matcher.find(at,0,8192,input.size()-at);
        if(match.second>=3){
            add(true);auto length=match.second-3;const auto offset=match.first-1;
            const auto token=static_cast<std::uint16_t>((offset<<3)|std::min<std::size_t>(length,7));output.push_back(static_cast<std::uint8_t>(token));output.push_back(static_cast<std::uint8_t>(token>>8));
            if(length>=7){
                const auto rest=length-7;const auto nibble=static_cast<std::uint8_t>(std::min<std::size_t>(rest,15));
                if(!nibble_at){nibble_at=output.size();output.push_back(nibble);}else {output[nibble_at]|=static_cast<std::uint8_t>(nibble<<4);nibble_at=0;}
                if(rest>=15){
                    if(rest-15<255)output.push_back(static_cast<std::uint8_t>(rest-15));
                    else {output.push_back(255);
                        if(length<65536){output.push_back(static_cast<std::uint8_t>(length));output.push_back(static_cast<std::uint8_t>(length>>8));}
                        else {output.insert(output.end(),{0,0});for(unsigned i=0;i<4;++i)output.push_back(static_cast<std::uint8_t>(length>>(8*i)));}}
                }
            }
            for(std::size_t k=0;k<match.second;++k)matcher.insert(at+k);at+=match.second;
        }else {add(false);output.push_back(input[at]);matcher.insert(at);++at;}
        if(output.size()>limit)return std::nullopt;
    }
    // 結尾以 1 旗標表示結束：補滿目前旗標字；剛好用完時另加全為 1 的旗標字。
    if(open){const auto rest=32-count;put32(flag_at,(flags<<rest)|((rest==32?0U:(1U<<rest))-1U));}
    else {flag_at=output.size();output.insert(output.end(),4,0);put32(flag_at,UINT32_MAX);}
    if(output.size()>limit)return std::nullopt;
    return output;
}
inline std::optional<std::vector<std::uint8_t>> xpress_decompress(std::span<const std::uint8_t> input,std::size_t limit){
    std::vector<std::uint8_t> output;std::size_t in=0,half=0;std::uint32_t flags=0;unsigned count=0;
    const auto read=[&](unsigned bytes,std::uint32_t& value){if(in+bytes>input.size())return false;value=0;for(unsigned i=0;i<bytes;++i)value|=std::uint32_t{input[in+i]}<<(8*i);in+=bytes;return true;};
    while(output.size()<limit){
        if(!count){if(in==input.size())break;if(!read(4,flags))return std::nullopt;count=32;}
        --count;
        if(!(flags&(1U<<count))){if(in==input.size())break;output.push_back(input[in++]);continue;}
        if(in==input.size())break;
        std::uint32_t token=0;if(!read(2,token))return std::nullopt;
        std::size_t length=token&7U;const std::size_t offset=(token>>3)+1U;
        if(length==7){
            if(!half){if(in>=input.size())return std::nullopt;half=in;length=input[in++]&15U;}else {length=input[half]>>4;half=0;}
            if(length==15){
                std::uint32_t value=0;if(!read(1,value))return std::nullopt;length=value;
                if(length==255){if(!read(2,value))return std::nullopt;if(!value && !read(4,value))return std::nullopt;if(value<22)return std::nullopt;length=value-22;}
                length+=15;
            }
            length+=7;
        }
        length+=3;if(offset>output.size())return std::nullopt;
        for(std::size_t k=0;k<length && output.size()<limit;++k)output.push_back(output[output.size()-offset]);
    }
    return output;
}
}
