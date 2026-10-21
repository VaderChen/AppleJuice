#pragma once
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm {
// BuildCommDCB 的 ANSI／Unicode 與 timeout 版本共用交易式解析。
inline bool serial_definition(std::string text,std::array<std::uint8_t,28>& dcb,std::optional<bool>& timeout){
    for(auto& c:text){if(static_cast<unsigned char>(c)>127)return false;c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));}
    const auto trim=[](std::string_view s){while(!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))s.remove_prefix(1);while(!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))s.remove_suffix(1);return s;};
    const auto integer=[](std::string_view s,std::uint32_t& result){const auto parsed=std::from_chars(s.data(),s.data()+s.size(),result);return !s.empty() && parsed.ec==std::errc{} && parsed.ptr==s.data()+s.size();};
    auto input=trim(text);
    if(input.starts_with("com")){
        input.remove_prefix(3);const auto end=input.find_first_not_of("0123456789");const auto digits=input.substr(0,end);std::uint32_t port=0;if(!integer(digits,port) || !port || port>4096)return false;
        input.remove_prefix(digits.size());if(!input.empty() && input.front()==':')input.remove_prefix(1);else if(!input.empty() && !std::isspace(static_cast<unsigned char>(input.front())))return false;input=trim(input);
    }
    auto next=dcb;std::uint32_t flags;std::memcpy(&flags,next.data()+8,4);
    flags=(flags&~(0xcU|0x30U|0x300U|0x3000U))|0x10U|0x1000U;
    bool baud110=false,stop_set=false;std::optional<bool> next_timeout;
    const auto bits=[&](unsigned mask,unsigned value){flags=(flags&~mask)|value;};
    const auto setting=[&](std::string_view key,std::string_view value)->bool{
        if(key=="baud"){
            std::uint32_t baud=0;if(!integer(value,baud) || !baud)return false;
            switch(baud){case 11:baud=110;break;case 15:baud=150;break;case 30:baud=300;break;case 60:baud=600;break;case 12:baud=1200;break;case 24:baud=2400;break;case 48:baud=4800;break;case 96:baud=9600;break;case 19:baud=19200;break;}
            std::memcpy(next.data()+4,&baud,4);baud110=baud==110;return true;
        }
        if(key=="parity"){const auto index=std::string_view("noems").find(value);if(value.size()!=1 || index==std::string_view::npos)return false;next[19]=static_cast<std::uint8_t>(index);bits(2,index?2:0);return true;}
        if(key=="data"){std::uint32_t data=0;if(!integer(value,data) || data<5 || data>8)return false;next[18]=static_cast<std::uint8_t>(data);return true;}
        if(key=="stop"){if(value!="1" && value!="1.5" && value!="2")return false;next[20]=value=="1"?0:value=="1.5"?1:2;stop_set=true;return true;}
        if(key=="dtr" || key=="rts"){
            const unsigned control=value=="off"?0:value=="on"?1:value=="hs"?2:value=="tg" && key=="rts"?3:4;
            if(control==4)return false;const unsigned shift=key=="dtr"?4:12;bits(3U<<shift,control<<shift);return true;
        }
        if(value!="on" && value!="off")return false;const bool enabled=value=="on";
        if(key=="to")next_timeout=enabled;
        else if(key=="xon")bits(0x300,enabled?0x300:0);
        else if(key=="odsr")bits(8,enabled?8:0);
        else if(key=="octs")bits(4,enabled?4:0);
        else if(key=="idsr")bits(0x40,enabled?0x40:0);
        else return false;return true;
    };
    if(input.find('=')!=std::string_view::npos){
        if(input.find(',')!=std::string_view::npos)return false;
        while(!input.empty()){
            const auto end=input.find_first_of("= \t\r\n");if(end==std::string_view::npos)return false;
            const auto key=input.substr(0,end);input=trim(input.substr(end));if(input.empty() || input.front()!='=')return false;input=trim(input.substr(1));
            const auto separator=input.find_first_of(" \t\r\n");const auto value=input.substr(0,separator);if(!setting(key,value))return false;
            input=separator==std::string_view::npos?std::string_view{}:trim(input.substr(separator));
        }
    }else if(!input.empty()){
        const std::array<std::string_view,4> keys{"baud","parity","data","stop"};unsigned column=0;
        while(true){const auto separator=input.find(',');const auto value=trim(input.substr(0,separator));
            if(column<4){if(!value.empty() && !setting(keys[column],value))return false;}
            else if(column==4){if(value=="x")bits(0x300,0x300);else if(value=="p"){bits(0xc,0xc);bits(0x30,0x20);bits(0x3000,0x2000);}else if(!value.empty())return false;}
            else return false;
            ++column;if(separator==std::string_view::npos)break;input=input.substr(separator+1);
        }
    }
    if(baud110 && !stop_set)next[20]=2;
    const std::uint32_t length=28;std::memcpy(next.data(),&length,4);std::memcpy(next.data()+8,&flags,4);
    dcb=next;timeout=next_timeout;return true;
}
}
