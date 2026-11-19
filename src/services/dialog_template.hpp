#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace x64arm {
struct DialogTemplate {
    std::vector<std::uint8_t> bytes;std::uint16_t count{};
    void word(std::uint32_t value){bytes.push_back(static_cast<std::uint8_t>(value));bytes.push_back(static_cast<std::uint8_t>(value>>8));}
    void dword(std::uint32_t value){word(value&0xffffU);word(value>>16);}
    void text(const std::u16string& value){for(const auto c:value)word(c);word(0);}
    void align(){while(bytes.size()%4)bytes.push_back(0);}
    void item(std::uint32_t style,std::int32_t x,std::int32_t y,std::int32_t cx,std::int32_t cy,std::int32_t id,std::uint16_t atom,const std::u16string& cls,const std::u16string& title){
        align();dword(0x50000000U|style);dword(0);word(static_cast<std::uint16_t>(x));word(static_cast<std::uint16_t>(y));
        word(static_cast<std::uint16_t>(cx));word(static_cast<std::uint16_t>(cy));word(static_cast<std::uint16_t>(id));
        if(atom){word(0xffff);word(atom);}else text(cls);
        text(title);word(0);++count;
    }
};
}
