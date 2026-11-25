#pragma once

#include <unicode/utypes.h>
#include <utrie2.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace x64arm::icu_selector {

inline constexpr std::size_t header_probe=32;
inline constexpr std::size_t index_size=64;
// ICU 以有號 16 位元值計算列舉長度，且在計數後才排除最後一組的填補位元。
inline constexpr std::int32_t max_encodings=(INT16_MAX/32)*32;

using Bytes=std::span<const std::uint8_t>;

inline std::uint32_t word(Bytes data,std::size_t offset,unsigned width,bool big_endian){
    std::uint32_t value{};
    for(unsigned i=0;i<width;++i){
        const auto index=big_endian?offset+i:offset+width-1-i;
        value=(value<<8)|data[index];
    }
    return value;
}

inline std::size_t header_size(Bytes header,std::size_t capacity,UErrorCode& error){
    if(header.size()<header_probe){error=U_INDEX_OUTOFBOUNDS_ERROR;return 0;}
    if(header[2]!=0xda || header[3]!=0x27 || std::memcmp(header.data()+12,"CSel",4)!=0 ||
       header[8]>1 || header[9]>1 || header[10]!=2){error=U_INVALID_FORMAT_ERROR;return 0;}
    if(header[16]!=1){error=U_UNSUPPORTED_ERROR;return 0;}
    const auto size=word(header,0,2,header[8]!=0),info=word(header,4,2,header[8]!=0);
    if(size<24 || size%4 || info<20 || info>size-4){error=U_INVALID_FORMAT_ERROR;return 0;}
    if(size+index_size>capacity){error=U_INDEX_OUTOFBOUNDS_ERROR;return 0;}
    return size;
}

struct Layout {
    std::size_t header{},trie{},rows{},encodings{},names{},size{};
    bool big_endian{};
};

inline Layout layout(Bytes indexes,std::size_t header,std::size_t capacity,bool big_endian,UErrorCode& error){
    if(indexes.size()<index_size){error=U_INDEX_OUTOFBOUNDS_ERROR;return {};}
    const auto at=[&](std::size_t index){return word(indexes,4*index,4,big_endian);};
    const auto trie=at(0),rows=at(1),encodings=at(2),names=at(3),size=at(15);
    const auto computed=index_size+std::uint64_t{trie}+4*std::uint64_t{rows}+names;
    if(size!=computed || trie<16 || trie%4 || !encodings || encodings>max_encodings ||
       rows<(encodings+31)/32 || names<encodings || names%4){error=U_INVALID_FORMAT_ERROR;return {};}
    if(header>capacity || size>capacity-header){error=U_INDEX_OUTOFBOUNDS_ERROR;return {};}
    return {header,trie,rows,encodings,names,header+size,big_endian};
}

inline bool valid_trie(Bytes trie,std::size_t max_row,bool big_endian){
    const auto field=[&](std::size_t offset){return word(trie,offset,2,big_endian);};
    if(trie.size()<16 || word(trie,0,4,big_endian)!=0x54726932 || field(4)!=0)return false;
    const auto indexes=std::size_t{field(6)},data_count=std::size_t{field(8)}<<UTRIE2_INDEX_SHIFT;
    const auto high_start=field(14)<<UTRIE2_SHIFT_1;
    const auto data_null=field(12);
    if(indexes<UTRIE2_INDEX_1_OFFSET || data_count<UTRIE2_DATA_START_OFFSET ||
       trie.size()!=16+2*(indexes+data_count) || high_start>0x110000 ||
       data_null<indexes || data_null>=indexes+data_count)return false;
    const auto at=[&](std::size_t index){return field(16+2*index);};
    const auto block=[&](std::size_t start,std::size_t count){
        return start>=indexes && start+count<=indexes+data_count;
    };
    const auto block32=[&](std::size_t index){
        return index<indexes && block(std::size_t{at(index)}<<UTRIE2_INDEX_SHIFT,UTRIE2_DATA_BLOCK_LENGTH);
    };
    for(std::size_t n=0;n<UTRIE2_INDEX_2_BMP_LENGTH;++n)if(!block32(n))return false;
    for(std::size_t n=UTRIE2_UTF8_2B_INDEX_2_OFFSET;n<UTRIE2_INDEX_1_OFFSET;++n)
        if(!block(at(n),64))return false;
    for(std::uint32_t code=0x10000;code<high_start;code+=UTRIE2_DATA_BLOCK_LENGTH){
        const auto index1=UTRIE2_INDEX_1_OFFSET-UTRIE2_OMITTED_BMP_INDEX_1_LENGTH+(code>>UTRIE2_SHIFT_1);
        if(index1>=indexes || !block32(at(index1)+((code>>UTRIE2_SHIFT_2)&UTRIE2_INDEX_2_MASK)))return false;
    }
    for(std::size_t n=indexes;n<indexes+data_count;++n)if(at(n)>max_row)return false;
    return true;
}

inline bool valid_payload(Bytes bytes,const Layout& item){
    if(!item.size || bytes.size()!=item.size)return false;
    const auto after_indexes=item.header+index_size;
    const auto columns=(item.encodings+31)/32;
    if(!valid_trie(bytes.subspan(after_indexes,item.trie),item.rows-columns,item.big_endian))return false;
    auto names=bytes.last(item.names);
    for(std::size_t n=0;n<item.encodings;++n){
        if(names.empty())return false;
        const auto* end=static_cast<const std::uint8_t*>(std::memchr(names.data(),0,names.size()));
        if(!end)return false;
        names=names.subspan(static_cast<std::size_t>(end-names.data())+1);
    }
    return true;
}

}
