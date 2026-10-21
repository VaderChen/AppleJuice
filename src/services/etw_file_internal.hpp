#pragma once
#include "x64arm/etw_file.hpp"
#include <algorithm>
#include <limits>

namespace x64arm::etw::detail {
inline constexpr std::size_t buffer_header_size=72;
inline std::size_t align8(std::size_t value){return (value+7)&~std::size_t{7};}
inline void require(bool condition,const char* message){if(!condition)throw Error(Failure::InvalidFormat,message);}
inline std::uint64_t get(std::span<const std::uint8_t> bytes,std::size_t at,unsigned width){
    require(at<=bytes.size() && width<=bytes.size()-at,"ETL 欄位超出資料範圍");
    std::uint64_t value=0;for(unsigned i=0;i<width;++i)value|=static_cast<std::uint64_t>(bytes[at+i])<<(8*i);return value;
}
inline void put(Bytes& bytes,std::size_t at,unsigned width,std::uint64_t value){
    for(unsigned i=0;i<width;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));
}
inline void copy(Bytes& bytes,std::size_t at,std::span<const std::uint8_t> value){
    std::copy(value.begin(),value.end(),bytes.begin()+static_cast<std::ptrdiff_t>(at));
}
inline Bytes range(std::span<const std::uint8_t> bytes,std::size_t begin,std::size_t end){
    require(begin<=end && end<=bytes.size(),"ETL 子範圍無效");
    return {bytes.begin()+static_cast<std::ptrdiff_t>(begin),bytes.begin()+static_cast<std::ptrdiff_t>(end)};
}
inline bool system_kind(Kind kind){return kind==Kind::System || kind==Kind::Compact || kind==Kind::PerfInfo;}
inline std::size_t base_size(Kind kind){
    switch(kind){case Kind::Modern:return 80;case Kind::Classic:return 48;case Kind::System:return 32;case Kind::Compact:return 24;case Kind::PerfInfo:return 16;}
    throw Error(Failure::InvalidParameter,"ETL 事件種類無效");
}
inline std::size_t extra_size(std::uint16_t version){return ((version&0x8000)?8U:0U)+8U*((version>>8)&7U);}
inline std::u16string string(std::span<const std::uint8_t> bytes,std::size_t& at,std::size_t maximum){
    std::u16string value;
    for(;;){const auto c=static_cast<char16_t>(get(bytes,at,2));at+=2;if(!c)return value;
        require(value.size()<maximum,"ETL 名稱超過上限");value.push_back(c);}
}
inline void append_string(Bytes& bytes,const std::u16string& value){
    for(auto c:value){bytes.push_back(static_cast<std::uint8_t>(c));bytes.push_back(static_cast<std::uint8_t>(c>>8));}bytes.insert(bytes.end(),2,0);
}
Header parse_header(const Record&);
Record header_record(const Header&);
}
