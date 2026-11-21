#pragma once

#include <ucptrie_impl.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace x64arm::icu_trie {
inline constexpr std::size_t value_size(int width){
    switch(width){
    case UCPTRIE_VALUE_BITS_16:return 2;
    case UCPTRIE_VALUE_BITS_32:return 4;
    case UCPTRIE_VALUE_BITS_8:return 1;
    default:return 0;
    }
}

inline bool valid_binary_indices(std::span<const std::uint16_t> index,std::size_t data_length,int type,std::int32_t high_start){
    const bool fast=type==UCPTRIE_TYPE_FAST;
    const auto fast_length=fast?UCPTRIE_BMP_INDEX_LENGTH:UCPTRIE_SMALL_INDEX_LENGTH;
    const auto fast_limit=fast_length<<UCPTRIE_FAST_SHIFT;
    // 末尾的 high／error 值可以與 128 個 ASCII 元素重疊。
    if(data_length<128 || index.size()<static_cast<std::size_t>(fast_length) || high_start>0x110000)return false;
    for(auto block:index.first(static_cast<std::size_t>(fast_length)))
        if(block+std::size_t{UCPTRIE_FAST_DATA_BLOCK_LENGTH}>data_length)return false;
    const auto i1_base=fast?fast_length-UCPTRIE_OMITTED_BMP_INDEX_1_LENGTH:fast_length;
    for(auto code=fast_limit;code<high_start;code+=UCPTRIE_CP_PER_INDEX_2_ENTRY){
        const auto i1=static_cast<std::size_t>(i1_base+(code>>UCPTRIE_SHIFT_1));
        if(i1>=index.size())return false;
        const auto i2=static_cast<std::size_t>(index[i1])+((code>>UCPTRIE_SHIFT_2)&UCPTRIE_INDEX_2_MASK);
        if(i2>=index.size())return false;
        const auto block=index[i2];
        const auto first=static_cast<std::size_t>(block&0x7fffU);
        const bool wide=(block&0x8000U)!=0;
        const std::size_t words=UCPTRIE_INDEX_3_BLOCK_LENGTH+(wide?UCPTRIE_INDEX_3_BLOCK_LENGTH/8:0);
        if(first+words>index.size())return false;
        for(unsigned i=0;i<UCPTRIE_INDEX_3_BLOCK_LENGTH;++i){
            auto offset=static_cast<std::uint32_t>(index[first+i]);
            if(wide){
                const auto group=first+(i/8)*9;
                offset=((static_cast<std::uint32_t>(index[group])<<(2+2*(i%8)))&0x30000U)|index[group+1+i%8];
            }
            if(offset+UCPTRIE_SMALL_DATA_BLOCK_LENGTH>data_length)return false;
        }
    }
    return true;
}


inline std::size_t binary_size(const UCPTrieHeader& header,UCPTrieType type,UCPTrieValueWidth width){
    const int stored_type=(header.options>>6)&3;
    const int stored_width=header.options&UCPTRIE_OPTIONS_VALUE_BITS_MASK;
    const auto unit=value_size(stored_width);
    if(header.signature!=UCPTRIE_SIG || stored_type>UCPTRIE_TYPE_SMALL || !unit ||
       (header.options&UCPTRIE_OPTIONS_RESERVED_MASK) || (type>=0 && type!=stored_type) ||
       (width>=0 && width!=stored_width) || (unit==4 && (header.indexLength&1)))return 0;
    const auto data_length=static_cast<std::size_t>(((header.options&UCPTRIE_OPTIONS_DATA_LENGTH_MASK)<<4)|header.dataLength);
    return sizeof(header)+static_cast<std::size_t>(header.indexLength)*2+data_length*unit;
}

inline bool valid_binary(std::span<const std::uint8_t> bytes,UCPTrieType type,UCPTrieValueWidth width){
    if(bytes.size()<sizeof(UCPTrieHeader) || (reinterpret_cast<std::uintptr_t>(bytes.data())&3))return false;
    UCPTrieHeader header{};
    std::memcpy(&header,bytes.data(),sizeof(header));
    const auto size=binary_size(header,type,width);
    if(!size || size>bytes.size())return false;
    const int stored_type=(header.options>>6)&3;
    const auto data_length=static_cast<std::size_t>(((header.options&UCPTRIE_OPTIONS_DATA_LENGTH_MASK)<<4)|header.dataLength);
    return valid_binary_indices({reinterpret_cast<const std::uint16_t*>(bytes.data()+sizeof(header)),header.indexLength},
            data_length,stored_type,static_cast<std::int32_t>(header.shiftedHighStart)<<UCPTRIE_SHIFT_2);
}
}
