#pragma once
#include "x64arm/memory.hpp"
#include "x64arm/unicode.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace x64arm {
// STRING／ANSI_STRING／UNICODE_STRING 的共用讀寫：{USHORT Length; USHORT MaximumLength; PTR Buffer}，
// Buffer 在 x86 位於偏移 4、x64 位於偏移 8。ntdll 的 Rtl 字串、載入器與地區設定服務共用，不各自重寫。
struct GuestCountedString {std::uint16_t length{},capacity{};std::uint64_t buffer{};};
inline GuestCountedString read_counted_header(const GuestMemory& memory,std::uint64_t address,bool x86){
    return {static_cast<std::uint16_t>(memory.load(GuestAddress{address},16)),static_cast<std::uint16_t>(memory.load(GuestAddress{address+2},16)),
            memory.load(GuestAddress{address+(x86?4U:8U)},x86?32:64)};
}
inline std::u16string read_counted_wide(const GuestMemory& memory,const GuestCountedString& value){
    return read_utf16_counted(memory,GuestAddress{value.buffer},value.length/2U);
}
inline std::string read_counted_narrow(const GuestMemory& memory,const GuestCountedString& value){
    std::string text(value.length,'\0');
    if(value.length)memory.read(GuestAddress{value.buffer},std::span(reinterpret_cast<std::uint8_t*>(text.data()),text.size()));
    return text;
}
inline std::vector<std::uint8_t> utf16_payload(std::u16string_view text){auto bytes=utf16_bytes(text);bytes.resize(text.size()*2);return bytes;}
// 寫入內容（不含 NUL 的 data）：allocate 時以 allocator(位元組數) 配置程序 heap 區塊（RtlFree*String 可釋放），
// 否則受 MaximumLength 限制、超過回 overflow；terminator 為 NUL 寬度（0、1、2），空間不足時省略 NUL。
template<class Allocator>
std::uint32_t store_counted_string(GuestMemory& memory,std::uint64_t address,bool x86,std::vector<std::uint8_t> data,unsigned terminator,bool allocate,
                                   std::uint32_t overflow,Allocator&& allocator){
    constexpr std::uint32_t invalid_parameter=0xc000000dU,no_memory=0xc0000017U;
    if(data.size()+terminator>0xffffU)return invalid_parameter;
    const auto length=static_cast<std::uint16_t>(data.size());data.resize(data.size()+terminator);
    const unsigned unit=x86?4U:8U;
    if(allocate){
        const std::uint64_t buffer=allocator(std::max<std::size_t>(data.size(),1));if(!buffer)return no_memory;
        if(!data.empty())memory.write(GuestAddress{buffer},data);
        memory.store(GuestAddress{address},16,length);memory.store(GuestAddress{address+2},16,data.size());memory.store(GuestAddress{address+unit},x86?32:64,buffer);
        return 0;
    }
    const auto target=read_counted_header(memory,address,x86);if(length>target.capacity)return overflow;
    if(data.size()>target.capacity)data.resize(length);
    if(!data.empty())memory.write(GuestAddress{target.buffer},data);
    memory.store(GuestAddress{address},16,length);return 0;
}
}
