#pragma once
#include "avi_frame_image.hpp"

namespace x64arm::avi {
// CF_DIB 是緊密排列的 DIB；CF_WAVE 則是包含 fmt 與 data 的 RIFF。
inline bool append_clipboard_stream(Movie& movie,unsigned format,View bytes){
    Stream stream;
    View header,samples;
    unsigned size=0,length=0,suffix=0;
    if(format==8 || format==17){
        const auto layout=frame_layout(bytes);
        if(!layout)return false;
        header=bytes.first(frame_header_size(bytes));
        const unsigned image=layout->raw()?layout->image_size():word(bytes,20);
        if(!image || image>bytes.size()-header.size())return false;
        samples=bytes.subspan(header.size(),image);
        put(stream.header,0,video);put(stream.header,4,layout->compression);
        put(stream.header,20,1);put(stream.header,24,1);
        put(stream.header,52,layout->width|(layout->height<<16));
        length=1;suffix=layout->raw()?0x6264:0x6364;
    }else if(format==12 || format==11){
        const auto root=riff::chunk(bytes,0,bytes.size());
        if(root.id!=riff::riff_id || root.size<4 || word(bytes,8)!=fourcc('W','A','V','E'))return false;
        bool has_data=false;
        chunks(bytes,12,8ULL+root.size,[&](unsigned tag,View chunk,std::size_t){
            if(tag==fourcc('f','m','t',' '))header=chunk;
            if(tag==fourcc('d','a','t','a')){samples=chunk;has_data=true;}
        });
        if(header.size()<16 || !has_data)return false;
        size=word(header,12)&0xffff;
        if(!size || !(word(header,0)>>16) || !word(header,8) || samples.size()%size)return false;
        length=static_cast<unsigned>(samples.size()/size);
        put(stream.header,0,audio);put(stream.header,20,size);put(stream.header,24,word(header,8));
        put(stream.header,44,size);suffix=0x6277;
    }else return false;
    put(stream.header,32,length);put(stream.header,36,static_cast<unsigned>(samples.size()));
    stream.format.assign(header.begin(),header.end());
    const auto index=static_cast<unsigned>(movie.streams.size());
    movie.streams.push_back(std::move(stream));
    movie.packets.push_back({index,('0'+index/10)|(('0'+index%10)<<8)|(suffix<<16),0x10,Bytes(samples.begin(),samples.end())});
    return true;
}
inline Bytes clipboard_wave(const Movie& movie,unsigned index){
    Bytes content,data,out;
    append_word(content,fourcc('W','A','V','E'));
    chunk(content,fourcc('f','m','t',' '),movie.streams[index].format);
    for(const auto& p:movie.packets)if(p.stream==index)data.insert(data.end(),p.bytes.begin(),p.bytes.end());
    chunk(content,fourcc('d','a','t','a'),data);chunk(out,riff::riff_id,content);return out;
}
}
