#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace x64arm {
using WindowsGuid = std::array<std::uint8_t,16>;
// GUID 前三個欄位採 Windows little-endian；字串及 UUID 比較使用欄位順序。
inline constexpr std::array<unsigned,16> windows_guid_order{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
inline std::u16string windows_guid_text(const WindowsGuid& value){
    constexpr char16_t hex[]=u"0123456789ABCDEF";
    std::u16string text=u"{";
    for(const auto index:windows_guid_order){
        if(text.size()==9 || text.size()==14 || text.size()==19 || text.size()==24)text+=u'-';
        text+=hex[value[index]>>4];text+=hex[value[index]&15];
    }
    text+=u'}';return text;
}
inline std::optional<WindowsGuid> parse_windows_guid(std::u16string_view text,bool braces){
    if(braces){
        if(text.size()!=38 || text.front()!=u'{' || text.back()!=u'}')return {};
        text=text.substr(1,36);
    }
    if(text.size()!=36)return {};
    const auto hex=[](char16_t c)->int{
        if(c>=u'0' && c<=u'9')return c-u'0';
        if(c>=u'A' && c<=u'F')return c-u'A'+10;
        if(c>=u'a' && c<=u'f')return c-u'a'+10;
        return -1;
    };
    WindowsGuid value{};unsigned at=0;
    for(const auto index:windows_guid_order){
        if(at==8 || at==13 || at==18 || at==23)if(text[at++]!=u'-')return {};
        const auto high=hex(text[at++]),low=hex(text[at++]);
        if(high<0 || low<0)return {};
        value[index]=static_cast<std::uint8_t>(high*16+low);
    }
    return value;
}
}
