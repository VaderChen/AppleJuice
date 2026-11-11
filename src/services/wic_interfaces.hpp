#pragma once
#include "x64arm/memory.hpp"
#include <array>
#include <cstdint>
#include <string_view>
namespace x64arm::wic {
using Guid=std::array<std::uint8_t,16>;
// 由「xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx」產生 Windows GUID 的記憶體排列（前三段小端序）。
consteval Guid guid(std::string_view text){
    const auto hex=[](char c)->unsigned{return c>='0' && c<='9'?unsigned(c-'0'):c>='a' && c<='f'?unsigned(c-'a'+10):unsigned(c-'A'+10);};
    std::array<unsigned,16> raw{};unsigned n=0;
    for(std::size_t i=0;i+1<text.size() && n<16;){if(text[i]=='-'){++i;continue;}raw[n++]=hex(text[i])*16+hex(text[i+1]);i+=2;}
    Guid out{};const unsigned order[16]{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    for(unsigned i=0;i<16;++i)out[i]=static_cast<std::uint8_t>(raw[order[i]]);
    return out;
}
inline constexpr Guid clsid_factory=guid("cacaf262-9370-4615-a13b-9f5539da4c0a");
inline constexpr Guid clsid_factory2=guid("317d06e8-5f24-433d-bdf7-79ce68d8abc2");
inline bool is_class(const GuestMemory& memory,std::uint64_t p){if(!p)return false;Guid g{};memory.read(GuestAddress{p},g);return g==clsid_factory || g==clsid_factory2;}
}
