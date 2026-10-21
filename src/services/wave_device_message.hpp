#pragma once
#include "x64arm/unicode.hpp"
#include "x64arm/memory.hpp"
#include <algorithm>
#include <span>
#include <string>
#include <string_view>

namespace x64arm::wave {
// 相容層的預設 CoreAudio 裝置身分；WinMM 與 MMDevice 使用相同的播放端點 ID。
inline constexpr std::u16string_view output_id=u"{0.0.0.00000000}.{x64arm-core-audio-output}";
inline constexpr std::u16string_view input_id=u"{0.0.1.00000000}.{x64arm-core-audio-input}";
// ids 為依 WinMM 裝置編號排列的端點 ID；第 0 個是系統預設裝置。
inline std::uint32_t device_message(GuestMemory& memory,std::uint64_t device,std::uint32_t message,
                                  std::uint64_t first,std::uint64_t second,std::span<const std::u16string> ids,bool opened,bool x86){
    // 系統攔截查詢接受 device ID，不接受已開啟的 HWAVE handle。
    if(opened)return 8;
    const bool mapper=device==UINT32_MAX || device==UINT64_MAX;const bool available=!ids.empty();
    if(!mapper && device>=std::max<std::size_t>(ids.size(),1))return 2;
    const std::u16string_view id=mapper || ids.empty()?std::u16string_view{}:std::u16string_view{ids[static_cast<std::size_t>(device)]};
    if(message==0x2015 || message==0x2017){
        if(!mapper)return 8;
        if(!first || !second)return 11;
        memory.check(GuestAddress{first},4,Access::Write);memory.check(GuestAddress{second},4,Access::Write);
        memory.store(GuestAddress{first},32,available?0:UINT32_MAX);memory.store(GuestAddress{second},32,1);return 0;
    }
    if(mapper)return 2;
    if(!available)return 2;
    if(message==0x805)return 0; // 此裝置確實可由 waveIn/OutOpen 的 mapper 開啟。
    if(message==0x812){
        if(!first || second)return 11;
        memory.store(GuestAddress{first},x86?32:64,(id.size()+1)*2);return 0;
    }
    if(message==0x811){
        const auto bytes=utf16_bytes(id);
        if(!first || second<bytes.size())return 11;
        memory.write(GuestAddress{first},bytes);return 0;
    }
    // CoreAudio 相容裝置沒有 Windows kernel device-interface，依契約回空身分。
    if(message==0x80d){if(!first || second)return 11;memory.store(GuestAddress{first},32,0);return 0;}
    if(message==0x80c){if(!first || second<2)return 11;memory.store(GuestAddress{first},16,0);return 0;}
    return 8;
}
inline std::uint32_t device_message(GuestMemory& memory,std::uint64_t device,std::uint32_t message,
                                  std::uint64_t first,std::uint64_t second,bool capture,bool available,bool opened,bool x86){
    const std::u16string id(capture?input_id:output_id);
    return device_message(memory,device,message,first,second,available?std::span<const std::u16string>(&id,1):std::span<const std::u16string>{},opened,x86);
}
}
