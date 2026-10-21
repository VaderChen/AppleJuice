#pragma once
#include "socket_address.hpp"
#include <optional>
#include <string_view>

namespace x64arm::socket_detail {
// 數值轉換不查詢 DNS；port 接受 Windows 的十／八／十六進位形式。
inline bool address_number(std::string_view text,std::uint32_t limit,std::uint32_t& value,bool radix=false){
    if(text.empty())return false;
    unsigned base=10;
    if(radix && text.size()>1 && text.front()=='0'){
        base=8;
        if(text[1]=='x' || text[1]=='X'){base=16;text.remove_prefix(2);if(text.empty())return false;}
    }
    value=0;
    for(const auto c:text){
        const unsigned digit=c>='0' && c<='9'?static_cast<unsigned>(c-'0'):c>='a' && c<='f'?static_cast<unsigned>(c-'a'+10):c>='A' && c<='F'?static_cast<unsigned>(c-'A'+10):base;
        if(digit>=base || digit>limit || value>(limit-digit)/base)return false;
        value=value*base+digit;
    }
    return true;
}
inline std::optional<Address> parse_address_text(unsigned family,std::string_view text){
    if((family!=2 && family!=23) || text.empty() || text.size()>128)return std::nullopt;
    // inet_aton 會接受尾端空白，Windows 的完整位址轉換必須消耗全部字串。
    if(std::any_of(text.begin(),text.end(),[](unsigned char c){return c<=32 || c>=127;}))return std::nullopt;
    std::string_view host=text,port_text;
    bool has_port=false;
    if(family==2){
        if(const auto colon=text.find(':');colon!=text.npos){host=text.substr(0,colon);port_text=text.substr(colon+1);has_port=true;}
    }else if(text.front()=='['){
        const auto close=text.find(']');if(close==text.npos)return std::nullopt;
        host=text.substr(1,close-1);
        const auto suffix=text.substr(close+1);
        if(!suffix.empty()){if(suffix.front()!=':')return std::nullopt;port_text=suffix.substr(1);has_port=true;}
    }
    std::uint32_t port=0,scope=0;
    if(has_port && (!address_number(port_text,65535,port,true) || !port))return std::nullopt;
    if(family==23){
        if(const auto percent=host.find('%');percent!=host.npos){
            if(!address_number(host.substr(percent+1),UINT32_MAX,scope))return std::nullopt;
            host=host.substr(0,percent);
        }
    }
    std::array<char,129> literal{};std::copy(host.begin(),host.end(),literal.begin());
    std::array<std::uint8_t,28> bytes{};bytes[0]=static_cast<std::uint8_t>(family);
    bytes[2]=static_cast<std::uint8_t>(port>>8);bytes[3]=static_cast<std::uint8_t>(port);
    if(family==2){
        in_addr address{};if(!::inet_aton(literal.data(),&address))return std::nullopt;
        std::memcpy(bytes.data()+4,&address,4);
    }else{
        if(::inet_pton(AF_INET6,literal.data(),bytes.data()+8)!=1)return std::nullopt;
        for(unsigned i=0;i<4;++i)bytes[24+i]=static_cast<std::uint8_t>(scope>>(i*8));
    }
    return native_address(std::span(bytes).first(family==2?16:28));
}
}
