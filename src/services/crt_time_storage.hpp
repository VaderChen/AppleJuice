#pragma once
#include "windows_locale_data.hpp"
#include "x64arm/memory.hpp"
#include "x64arm/unicode.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace x64arm::crt_time {
struct Storage {
    std::vector<std::uint8_t> bytes;
    std::array<std::size_t,87> pointer_cells{};
    unsigned pointer_bytes{};
    void put(std::size_t at,std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));}
    void relocate(std::uint64_t base){
        for(const auto at:pointer_cells){std::uint64_t offset=0;for(unsigned i=0;i<pointer_bytes;++i)offset|=std::uint64_t{bytes[at+i]}<<(8*i);
            if(offset>UINT64_MAX-base || (pointer_bytes==4 && base+offset>UINT32_MAX))throw std::overflow_error("CRT 時間快照位址溢位");put(at,base+offset,pointer_bytes);
        }
    }
};
inline Storage encode(const CrtTimeData& data,std::uint32_t page,unsigned pointer_bytes){
    Storage result;result.pointer_bytes=pointer_bytes;result.bytes.resize(87*pointer_bytes+8);result.put(43*pointer_bytes,data.calendar,4);result.put(43*pointer_bytes+4,1,4);
    const auto append=[&](std::span<const std::uint8_t> value,unsigned alignment){while(result.bytes.size()%alignment)result.bytes.push_back(0);const auto at=result.bytes.size();result.bytes.insert(result.bytes.end(),value.begin(),value.end());return at;};
    for(unsigned i=0;i<43;++i){
        // 與 CRT 的 WideCharToMultiByte 相同：碼頁無法表示的字元以預設字元 '?' 取代，而非失敗。
        auto narrow=windows_crt_encode(data.fields[i],page);
        if(!narrow){
            narrow.emplace();
            for(const auto ch:data.fields[i]){const auto single=windows_crt_encode(std::u16string(1,ch),page);if(single)narrow->insert(narrow->end(),single->begin(),single->end());else narrow->push_back('?');}
        }
        narrow->push_back(0);
        result.pointer_cells[i]=i*pointer_bytes;result.put(i*pointer_bytes,append(*narrow,1),pointer_bytes);
    }
    for(unsigned i=0;i<44;++i){const auto at=43*pointer_bytes+8+i*pointer_bytes;result.pointer_cells[43+i]=at;const auto wide=utf16_bytes(data.fields[i]);result.put(at,append(wide,2),pointer_bytes);}
    return result;
}
inline CrtTimeData read(const GuestMemory& memory,std::uint64_t pointer,unsigned pointer_bytes){
    memory.check(GuestAddress{pointer},87*pointer_bytes+8,Access::Read);CrtTimeData data;data.calendar=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+43*pointer_bytes},32));
    for(unsigned i=0;i<44;++i){const auto text=memory.load(GuestAddress{pointer+43*pointer_bytes+8+i*pointer_bytes},static_cast<std::uint8_t>(pointer_bytes*8));data.fields[i]=read_utf16(memory,GuestAddress{text},4096);}
    // _Gettnames 是深複製；它不等同於 CRT 內部的靜態 C 時間物件。
    return data;
}
inline std::optional<std::u16string> decode(std::string_view text,std::uint32_t page){
    std::u16string result;const auto bytes=std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());
    for(std::size_t at=0;at<bytes.size();){const auto decoded=windows_crt_decode(bytes.subspan(at),page);if(decoded.status!=CrtDecodedCharacter::Status::Complete || !decoded.bytes)return {};
        if(decoded.character<=0xffff)result.push_back(static_cast<char16_t>(decoded.character));else{const auto value=decoded.character-0x10000;result.push_back(static_cast<char16_t>(0xd800+(value>>10)));result.push_back(static_cast<char16_t>(0xdc00+(value&1023)));}at+=decoded.bytes;
    }return result;
}
}
