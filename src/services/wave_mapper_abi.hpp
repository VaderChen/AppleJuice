#pragma once

#include "x64arm/windows_abi.hpp"
#include <array>
#include <optional>
#include <span>

namespace x64arm::wave {
// Wave driver 的訊息與 WinMM 使用同一套裝置、標頭、佇列及通知。
struct MapperCall : WindowsCall {
    using WindowsCall::WindowsCall;
    std::optional<std::array<std::uint64_t,6>> parameters;
    std::uint64_t client_handle{};
    std::uint64_t operator()(unsigned index)const{
        return parameters?parameters->at(index):WindowsCall::operator()(index);
    }

    std::optional<std::uint32_t> translate(GuestMemory& memory,std::string_view& name,bool input,std::uint8_t bits){
        if(name!="widMessage" && name!="wodMessage")return {};
        const auto device=static_cast<std::uint32_t>((*this)(0));
        const auto message=static_cast<std::uint32_t>((*this)(1));
        const auto instance=(*this)(2),first=(*this)(3),second=(*this)(4);
        const unsigned base=input?50U:3U;
        if(message>=100 && message<=104)return 0;
        if(message==base)return 1;
        if(device!=0 && device!=UINT32_MAX && device!=UINT16_MAX)return 2;
        if(message==base+2){
            if(!first)return 11;
            const unsigned unit=bits/8;
            const auto field=[&](unsigned index){return memory.load(GuestAddress{first+index*unit},bits);};
            const auto flags=static_cast<std::uint32_t>(second);
            const auto requested=flags&4U?memory.load(GuestAddress{first+4*unit},32):UINT32_MAX;
            client_handle=field(0);
            name=input?"waveInOpen":"waveOutOpen";
            parameters=std::array<std::uint64_t,6>{instance,requested,field(1),field(2),field(3),flags};
            return {};
        }
        if(message==base+1){
            name=input?"waveInGetDevCapsW":"waveOutGetDevCapsW";
            parameters=std::array<std::uint64_t,6>{instance?instance:UINT32_MAX,first,second};
            return {};
        }
        if(message==0x2000U){
            if(first>2)return 8;
            name=input?"waveInMapperStatus":"waveOutMapperStatus";
            parameters=std::array<std::uint64_t,6>{instance,first,second};
            return {};
        }
        if(message==0x2015U || message==0x2017U){
            name=input?"waveInMessage":"waveOutMessage";
            parameters=std::array<std::uint64_t,6>{UINT32_MAX,message,first,second};
            return {};
        }
        static constexpr std::array<std::string_view,8> capture{
            "waveInClose","waveInPrepareHeader","waveInUnprepareHeader","waveInAddBuffer",
            "waveInStart","waveInStop","waveInReset","waveInGetPosition"
        };
        static constexpr std::array<std::string_view,15> playback{
            "waveOutClose","waveOutPrepareHeader","waveOutUnprepareHeader","waveOutWrite",
            "waveOutPause","waveOutRestart","waveOutReset","waveOutGetPosition","waveOutGetPitch",
            "waveOutSetPitch","waveOutGetVolume","waveOutSetVolume","waveOutGetPlaybackRate","waveOutSetPlaybackRate",
            "waveOutBreakLoop"
        };
        const std::span<const std::string_view> commands=input?std::span<const std::string_view>(capture):
            std::span<const std::string_view>(playback);
        const auto first_command=base+3;
        if(message<first_command || message-first_command>=commands.size())return 8;
        name=commands[message-first_command];
        parameters=std::array<std::uint64_t,6>{instance,first,second};
        return {};
    }
};

inline void write_pcm_format(GuestMemory& memory,std::uint64_t output,unsigned channels,unsigned rate,
                             unsigned bits,bool floating){
    const auto align=channels*bits/8;
    memory.check(GuestAddress{output},18,Access::Write);
    memory.store(GuestAddress{output},16,floating?3:1);
    memory.store(GuestAddress{output+2},16,channels);
    memory.store(GuestAddress{output+4},32,rate);
    memory.store(GuestAddress{output+8},32,rate*align);
    memory.store(GuestAddress{output+12},16,align);
    memory.store(GuestAddress{output+14},16,bits);
    memory.store(GuestAddress{output+16},16,0);
}
}
