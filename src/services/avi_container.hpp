#pragma once
#include "x64arm/riff_chunk.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <vector>

namespace x64arm::avi {
using Bytes=std::vector<std::uint8_t>;
using View=std::span<const std::uint8_t>;
constexpr std::uint32_t fourcc(char a,char b,char c,char d){
    return std::uint32_t(std::uint8_t(a))|(std::uint32_t(std::uint8_t(b))<<8)|
           (std::uint32_t(std::uint8_t(c))<<16)|(std::uint32_t(std::uint8_t(d))<<24);
}
constexpr auto video=fourcc('v','i','d','s'),audio=fourcc('a','u','d','s');
inline std::uint32_t word(View data,std::size_t at){return riff::word(data.subspan(at));}
inline void put(std::span<std::uint8_t> data,std::size_t at,std::uint32_t value){
    for(unsigned i=0;i<4;++i)data[at+i]=static_cast<std::uint8_t>(value>>(8*i));
}
inline void append_word(Bytes& data,std::uint32_t value){const auto pos=data.size();data.resize(pos+4);put(data,pos,value);}
struct Extra {std::uint32_t id;Bytes bytes;};
struct Stream {
    std::array<std::uint8_t,56> header{};
    Bytes format,name;
    std::vector<Extra> extra;
};
struct Packet {unsigned stream;std::uint32_t id,flags;Bytes bytes;std::uint32_t record{};};
struct Movie {
    std::array<std::uint8_t,56> header{};
    std::vector<Stream> streams;
    std::vector<Packet> packets;
    std::vector<Extra> extra;
};
inline void chunk(Bytes& out,std::uint32_t id,View content){
    if(content.size()>UINT32_MAX || out.size()+8+content.size()+(content.size()&1U)>UINT32_MAX)
        throw std::length_error("AVI RIFF 大小超出 DWORD");
    append_word(out,id);append_word(out,static_cast<std::uint32_t>(content.size()));
    out.insert(out.end(),content.begin(),content.end());if(content.size()&1U)out.push_back(0);
}
inline Bytes encode(const Movie& movie){
    Bytes hdrl;append_word(hdrl,fourcc('h','d','r','l'));
    auto main=movie.header;
    put(main,24,static_cast<std::uint32_t>(movie.streams.size()));
    put(main,12,word(main,12)|0x10U);
    chunk(hdrl,fourcc('a','v','i','h'),main);
    for(const auto& stream:movie.streams){
        Bytes list;append_word(list,fourcc('s','t','r','l'));
        chunk(list,fourcc('s','t','r','h'),stream.header);
        chunk(list,fourcc('s','t','r','f'),stream.format);
        if(!stream.name.empty())chunk(list,fourcc('s','t','r','n'),stream.name);
        for(const auto& e:stream.extra)chunk(list,e.id,e.bytes);
        chunk(hdrl,riff::list_id,list);
    }
    Bytes body,movi,index;append_word(body,fourcc('A','V','I',' '));
    chunk(body,riff::list_id,hdrl);
    for(const auto& e:movie.extra)chunk(body,e.id,e.bytes);
    append_word(movi,fourcc('m','o','v','i'));
    Bytes record;
    std::uint32_t current=0;
    auto close_record=[&]{
        if(current)chunk(movi,riff::list_id,record);
        record.clear();
    };
    for(const auto& packet:movie.packets){
        if(packet.record!=current){
            close_record();current=packet.record;
            if(current)append_word(record,fourcc('r','e','c',' '));
        }
        const auto offset=movi.size()+(current?8+record.size():0);
        append_word(index,packet.id);append_word(index,packet.flags);
        append_word(index,static_cast<std::uint32_t>(offset));
        append_word(index,static_cast<std::uint32_t>(packet.bytes.size()));
        chunk(current?record:movi,packet.id,packet.bytes);
    }
    close_record();
    chunk(body,riff::list_id,movi);chunk(body,fourcc('i','d','x','1'),index);
    Bytes result;chunk(result,riff::riff_id,body);return result;
}
template<typename Visit>
void chunks(View data,std::size_t begin,std::size_t end,Visit visit){
    for(auto pos=begin;pos<end;){
        const auto c=riff::chunk(data.subspan(pos),pos,end);
        visit(c.id,data.subspan(static_cast<std::size_t>(c.data),c.size),pos);
        pos=static_cast<std::size_t>(c.next);
    }
}
inline Movie decode(View data){
    const auto root=riff::chunk(data,0,data.size());
    if(root.id!=riff::riff_id || root.size<4 || word(data,8)!=fourcc('A','V','I',' '))
        throw std::invalid_argument("不是 RIFF AVI");
    Movie movie;bool has_header=false,has_movie=false;
    unsigned record_number=0;
    View index;std::size_t movi_origin=0;
    std::map<std::size_t,std::size_t> packet_offsets;
    auto copy=[](View v){return Bytes(v.begin(),v.end());};
    chunks(data,12,8ULL+root.size,[&](std::uint32_t id,View bytes,std::size_t pos){
        if(id==riff::list_id && word(bytes,0)==fourcc('h','d','r','l')){
            chunks(bytes,4,bytes.size(),[&](std::uint32_t tag,View h,std::size_t){
                if(tag==fourcc('a','v','i','h')){
                    if(has_header || h.size()<movie.header.size())throw std::invalid_argument("AVI 主標頭長度或數目不符");
                    std::copy_n(h.begin(),movie.header.size(),movie.header.begin());has_header=true;
                }else if(tag==riff::list_id && word(h,0)==fourcc('s','t','r','l')){
                    if(movie.streams.size()>=100)throw std::invalid_argument("AVI 串流數超出 chunk 編號");
                    auto& stream=movie.streams.emplace_back();bool header=false;
                    chunks(h,4,h.size(),[&](std::uint32_t field,View v,std::size_t){
                        if(field==fourcc('s','t','r','h')){
                            if(header || v.size()<48)throw std::invalid_argument("AVI 串流標頭不完整");
                            std::copy_n(v.begin(),std::min(v.size(),stream.header.size()),stream.header.begin());header=true;
                        }else if(field==fourcc('s','t','r','f'))stream.format=copy(v);
                        else if(field==fourcc('s','t','r','n'))stream.name=copy(v);
                        else stream.extra.push_back({field,copy(v)});
                    });
                    if(!header)throw std::invalid_argument("AVI 缺少串流標頭");
                }
            });
        }else if(id==riff::list_id && word(bytes,0)==fourcc('m','o','v','i')){
            if(has_movie)throw std::invalid_argument("重複的 AVI movi");
            has_movie=true;movi_origin=pos+8;
            const auto packet=[&](std::uint32_t code,View v,std::size_t at,unsigned record){
                const auto a=code&255U,b=(code>>8)&255U;
                if(a<'0'||a>'9'||b<'0'||b>'9')return;
                const auto number=(a-'0')*10+(b-'0');
                if(number>=movie.streams.size())throw std::invalid_argument("AVI 封包指向未知串流");
                packet_offsets.emplace(at,movie.packets.size());
                const auto suffix=code>>16;
                const auto key=suffix==0x6264U || suffix==0x6277U;
                movie.packets.push_back({number,code,key?0x10U:0U,copy(v),record});
            };
            chunks(bytes,4,bytes.size(),[&](std::uint32_t code,View v,std::size_t at){
                if(code==riff::list_id && word(v,0)==fourcc('r','e','c',' ')){
                    ++record_number;
                    chunks(v,4,v.size(),[&](std::uint32_t sub,View content,std::size_t offset){packet(sub,content,at+8+offset,record_number);});
                }else packet(code,v,at,0);
            });
        }else if(id==fourcc('i','d','x','1'))index=bytes;
        else if(id!=fourcc('J','U','N','K'))movie.extra.push_back({id,copy(bytes)});
    });
    if(!has_header || !has_movie || word(movie.header,24)!=movie.streams.size())
        throw std::invalid_argument("AVI 容器或串流清單不完整");
    if(index.size()%16)throw std::invalid_argument("AVI 索引長度無效");
    for(std::size_t at=0;at<index.size();at+=16){
        const auto offset=word(index,at+8);
        const auto match=[&](std::size_t pos){
            const auto it=packet_offsets.find(pos);if(it==packet_offsets.end())return false;
            auto& p=movie.packets[it->second];
            if(p.id!=word(index,at) || p.bytes.size()!=word(index,at+12))return false;
            p.flags=word(index,at+4);return true;
        };
        if(!match(offset) && offset>=movi_origin)match(offset-movi_origin);
    }
    return movie;
}
}
