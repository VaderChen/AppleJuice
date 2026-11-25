#pragma once
#include "x64arm/memory.hpp"
#include <algorithm>
#include <cstring>

namespace x64arm {
// 以客體碼元計數。逐段取得可讀映射，未對齊的 UTF-16 碼元也可跨越映射邊界。
inline std::uint64_t crt_bounded_length(const GuestMemory& memory,std::uint64_t text,std::uint64_t limit,unsigned width){
    std::uint64_t count=0;
    while(count<limit){
        const auto units=std::min<std::uint64_t>(limit-count,4096/width);
        const GuestAddress at{text+count*width};
        const auto bytes=memory.readable_prefix(at,units*width);
        if(width==1){
            const auto* zero=static_cast<const std::uint8_t*>(std::memchr(bytes.data(),0,bytes.size()));
            if(zero)return count+static_cast<std::size_t>(zero-bytes.data());
            count+=bytes.size();
        }else if(bytes.size()<2){
            if(!memory.load(at,16))return count;
            ++count;
        }else{
            const auto pairs=bytes.size()/2;
            for(std::size_t i=0;i<pairs;++i)
                if(!(bytes[2*i]|bytes[2*i+1]))return count+i;
            count+=pairs;
        }
    }
    return count;
}
}
