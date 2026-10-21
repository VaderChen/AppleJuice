#pragma once
#include "x64arm/registry.hpp"
#include <string_view>

namespace x64arm::http_proxy {
struct Error {std::uint32_t code;};
struct Configuration {std::string proxy,bypass;};
inline std::u16string key(bool x86){return registry_view_path(u"\x0002\\software\\microsoft\\windows\\currentversion\\internet settings\\connections",registry_view(0,x86));}
inline constexpr std::u16string_view value_name=u"winhttpsettings";

// WinHTTP 的機器設定與 IE 的 HKCU ProxyServer 分開儲存。兩個字串皆為
// 長度前綴的 ASCII；讀取時逐段驗證，不能信任登錄檔提供的長度。
inline Configuration read(const RegistryState& registry,bool x86){
    const auto found=registry.keys.find(key(x86));if(found==registry.keys.end())return {};
    const auto value=found->second.find(std::u16string(value_name));if(value==found->second.end())return {};
    const auto& bytes=value->second.bytes;if(value->second.type!=3 || bytes.size()<20)throw Error{13};
    std::size_t cursor=0;
    const auto number=[&]{if(bytes.size()-cursor<4)throw Error{13};std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=std::uint32_t{bytes[cursor++]}<<(8*i);return n;};
    if(number()!=0x18)throw Error{13};(void)number();const auto flags=number();
    const auto string=[&]{const auto length=number();if(length>bytes.size()-cursor)throw Error{13};std::string text;text.reserve(length);
        for(std::uint32_t i=0;i<length;++i){const auto c=bytes[cursor++];if(!c || c>127)throw Error{13};text.push_back(static_cast<char>(c));}return text;};
    Configuration result{string(),string()};if(!(flags&2))return {};return result;
}
inline std::vector<std::uint8_t> encode(const Configuration& value,bool named){
    std::vector<std::uint8_t> bytes;bytes.reserve(20+value.proxy.size()+value.bypass.size());
    const auto number=[&](std::uint32_t n){for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(n>>(8*i)));};
    number(0x18);number(0);number(named?2U:1U);
    for(const auto* text:{&value.proxy,&value.bypass}){number(static_cast<std::uint32_t>(text->size()));bytes.insert(bytes.end(),text->begin(),text->end());}return bytes;
}
}
