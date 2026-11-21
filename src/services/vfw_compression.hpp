#pragma once

#include "vfw_codec.hpp"
#include "avi_run_length.hpp"
#include "texture_image.hpp"

namespace x64arm::vfw {
struct CompressionSequence {
    std::uint64_t hic{},input{},output{},output_storage{},bits{},previous{},caller_output{};
    std::size_t input_size{},output_size{};
    std::uint32_t frame{},last_key{},quality{};
    bool busy{},has_previous{};
};
struct CompVarsAbi {
    unsigned unit;
    unsigned size()const{return unit==4?64:96;}
    unsigned input()const{return 16+unit;}
    unsigned output()const{return input()+unit;}
    unsigned bits()const{return input()+2*unit;}
    unsigned previous()const{return input()+3*unit;}
    unsigned frame()const{return input()+4*unit;}
    unsigned key()const{return frame()+4;}
    unsigned rate()const{return frame()+8;}
    unsigned quality()const{return frame()+12;}
    unsigned key_count()const{return frame()+16;}
    unsigned state()const{return unit==4?56:80;}
};
struct CompressorChoice {
    struct Entry {std::uint64_t hic{};std::uint32_t handler{};std::u16string label;};
    std::vector<Entry> choices;
    std::uint64_t stream{};
    std::uint32_t selected{},quality{UINT32_MAX},key{},rate{};
    bool busy{},preview_visible{};
};
inline constexpr int choice_codec=1001, choice_quality=1002, choice_key_enable=1003, choice_key=1004,
    choice_rate_enable=1005, choice_rate=1006, choice_preview=1007, choice_view=1008,
    choice_configure=1009, choice_about=1010;

inline bool compression_source(Backend backend,const avi::FrameLayout& src) {
    if(!src.raw())return false;
    if(backend==Backend::RunLength)return src.depth==4 || src.depth==8;
    if(backend==Backend::Video1)return !(src.width&3) && !(src.height&3);
    return builtin(backend);
}

inline bool compression_destination(Backend backend,const avi::FrameLayout& src,const avi::FrameLayout& dst){
    if(src.width!=dst.width || src.height!=dst.height)return false;
    if(backend==Backend::RunLength)
        return !dst.top_down && dst.depth==src.depth && dst.palette==src.palette &&
               dst.compression==(src.depth==8?1U:2U);
    if(backend==Backend::Video1)return dst.depth==16 && !dst.top_down && accepts(backend,dst);
    return accepts(backend,dst) && (dst.raw() || !dst.top_down);
}

inline std::uint32_t compression_bound(Backend backend,const avi::FrameLayout& dst) {
    if(backend==Backend::Dib)return dst.image_size();
    if(backend==Backend::RunLength)return (2*dst.width+2)*dst.height+2;
    if(backend==Backend::Video1)return (dst.width*dst.height/16)*18+2;
    // ImageIO 可以加入標頭、色彩資訊及容器中繼資料。此上限供 GET_SIZE 及輸出緩衝區配置。
    return 5*dst.width*dst.height+1024*1024;
}

inline avi::Bytes compression_header(Backend backend,avi::View input,const avi::FrameLayout& src) {
    if(!compression_source(backend,src))return {};
    auto output=avi::frame_output(input,src,{});
    if(output.empty())return {};
    if(backend==Backend::RunLength){
        avi::put(output,8,src.height);
        avi::put(output,16,src.depth==8?1:2);
    }else if(backend==Backend::Video1){
        output.assign(40,0);
        avi::put(output,0,40);
        avi::put(output,4,src.width);
        avi::put(output,8,src.height);
        avi::put(output,12,16U<<16|1);
        avi::put(output,16,video1_tag);
    }else if(backend==Backend::Jpeg || backend==Backend::Png){
        output.assign(40,0);
        avi::put(output,0,40);
        avi::put(output,4,src.width);
        avi::put(output,8,src.height);
        avi::put(output,12,24U<<16|1);
        avi::put(output,16,backend==Backend::Jpeg?fourcc('M','J','P','G'):fourcc('P','N','G',' '));
    }
    const auto dest=avi::frame_layout(output);
    if(!dest)return {};
    avi::put(output,20,compression_bound(backend,*dest));
    return output;
}

inline avi::Bytes encode_run_length(const avi::FrameLayout& src,avi::View input,avi::View previous={}) {
    avi::Bytes result;
    const bool four=src.depth==4;
    std::vector<std::uint8_t> pixels(src.width);
    const auto index=[four](avi::View row,unsigned x){
        return four?static_cast<std::uint8_t>((row[x/2]>>((x&1)?0:4))&15):row[x];
    };
    for(unsigned y=0;y<src.height;++y){
        const auto offset=std::size_t(src.top_down?src.height-y-1:y)*src.stride;
        const auto row=input.subspan(offset,src.stride);
        for(unsigned x=0;x<src.width;++x)pixels[x]=index(row,x);
        unsigned first_changed=0,last_changed=src.width;
        if(!previous.empty()){
            const auto prior=previous.subspan(offset,src.stride);
            while(first_changed<last_changed && pixels[first_changed]==index(prior,first_changed))++first_changed;
            while(last_changed>first_changed && pixels[last_changed-1]==index(prior,last_changed-1))--last_changed;
            for(unsigned skipped=0;first_changed<last_changed && skipped<first_changed;){
                const auto count=std::min(first_changed-skipped,255U);
                result.insert(result.end(),{0,2,static_cast<std::uint8_t>(count),0});
                skipped+=count;
            }
        }
        const auto run_length=[&](unsigned x){
            unsigned end=x+1;
            while(end<last_changed && end-x<255 &&
                  pixels[end]==pixels[x+(four?((end-x)&1):0)])++end;
            return end-x;
        };
        const auto write_run=[&](unsigned x,unsigned count){
            result.push_back(static_cast<std::uint8_t>(count));
            const auto first=pixels[x],second=count>1?pixels[x+1]:first;
            result.push_back(four?static_cast<std::uint8_t>((first<<4)|second):first);
        };
        for(unsigned x=first_changed;x<last_changed;){
            const auto run=run_length(x);
            if(run>=3){
                write_run(x,run);
                x+=run;
                continue;
            }
            unsigned end=x+run;
            while(end<last_changed && end-x<255){
                const auto next=run_length(end);
                if(next>=3 || end-x+next>255)break;
                end+=next;
            }
            const auto count=end-x;
            if(count<3){
                if(four || count==run)write_run(x,count);
                else{write_run(x,1);write_run(x+1,1);}
            }else{
                result.push_back(0);
                result.push_back(static_cast<std::uint8_t>(count));
                const auto start=result.size();
                if(four){
                    for(unsigned i=x;i<end;i+=2)
                        result.push_back(static_cast<std::uint8_t>((pixels[i]<<4)|(i+1<end?pixels[i+1]:0)));
                }else result.insert(result.end(),pixels.begin()+x,pixels.begin()+end);
                if((result.size()-start)&1)result.push_back(0);
            }
            x=end;
        }
        result.insert(result.end(),{0,0}); // EOL
    }
    result.insert(result.end(),{0,1}); // EOF
    return result;
}

inline avi::Bytes encode_image(Backend backend,avi::View header,const avi::FrameLayout& src,avi::View pixels,
                              const avi::FrameLayout& dst,unsigned quality,unsigned target) {
    if(backend==Backend::Dib){
        avi::Bytes output(dst.image_size());
        if(!avi::frame_pixels(header,src,pixels,dst,output,0,0,dst.width,dst.height))return {};
        return output;
    }
    if(backend==Backend::RunLength)return encode_run_length(src,pixels);
    const DibRowCodec codec(src.depth,src.masks,src.palette);
    std::vector<unsigned> row(src.width);
    avi::Bytes rgba(std::size_t(src.width)*src.height*4);
    for(unsigned y=0;y<src.height;++y){
        const auto offset=std::size_t(src.top_down?y:src.height-y-1)*src.stride;
        if(!codec.decode(pixels.subspan(offset,src.stride),row))return {};
        for(unsigned x=0;x<src.width;++x){
            const auto color=row[x];
            const auto at=(std::size_t(y)*src.width+x)*4;
            rgba[at]=static_cast<std::uint8_t>(color);
            rgba[at+1]=static_cast<std::uint8_t>(color>>8);
            rgba[at+2]=static_cast<std::uint8_t>(color>>16);
            rgba[at+3]=src.masks[3]?static_cast<std::uint8_t>(color>>24):255;
        }
    }
    GraphicsResourceDesc desc{};
    desc.dimension=3;
    desc.format=28;
    desc.width=src.width;
    desc.height=src.height;
    desc.samples=1;
    const bool jpeg=backend==Backend::Jpeg;
    const auto encode=[&](double value){
        return texture_image::encode(desc,jpeg?1:3,[&](unsigned){return rgba;},{value});
    };
    try {
        const auto requested=std::min(quality,10000U)/10000.0;
        auto encoded=encode(requested);
        if(!jpeg || !target || encoded.size()<=target || requested==0)return encoded;
        // dwFrameSize 是目標長度；達不到時保留可解碼、實際最小的影格。
        auto smallest=encode(0);
        if(smallest.size()>target)return smallest.size()<encoded.size()?std::move(smallest):std::move(encoded);
        double low=0,high=requested;
        for(unsigned step=0;step<5;++step){
            const auto middle=(low+high)/2;
            auto attempt=encode(middle);
            if(attempt.size()>target)high=middle;
            else{low=middle;smallest=std::move(attempt);}
        }
        return smallest;
    }catch(const texture_image::Error&){return {};}
}
}
