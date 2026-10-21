#pragma once
#include "x64arm/audio.hpp"
#include "x64arm/windows_guid.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

// 宿主 Core Audio 裝置對應的 Windows 音訊端點。MMDevice、WinMM 與 DirectSound 共用同一份順序、
// 端點 ID 與裝置 GUID：GUID 由 Core Audio UID 推導（跨程序、重新啟動皆穩定），DirectSound 裝置 GUID
// 即 PKEY_AudioEndpoint_GUID；預設輸出裝置排在第一個，WinMM 裝置編號依此順序。
namespace x64arm::audio_endpoint {
struct Endpoint {std::u16string id,guid_text;WindowsGuid guid{};audio::DeviceInfo device;};
inline std::u16string guid_from_uid(std::string_view uid){
    std::array<std::uint8_t,16> bytes{};
    for(unsigned half=0;half<2;++half){std::uint64_t hash=half?0x84222325cbf29ce4ULL:1469598103934665603ULL;
        for(const auto c:uid){hash^=static_cast<std::uint8_t>(c);hash*=1099511628211ULL;}
        for(unsigned i=0;i<8;++i)bytes[half*8+i]=static_cast<std::uint8_t>(hash>>(8*i));}
    bytes[6]=static_cast<std::uint8_t>((bytes[6]&0x0f)|0x50);bytes[8]=static_cast<std::uint8_t>((bytes[8]&0x3f)|0x80);
    constexpr char16_t digits[]=u"0123456789abcdef";std::u16string text=u"{";
    for(unsigned i=0;i<16;++i){if(i==4 || i==6 || i==8 || i==10)text+=u'-';text+=digits[bytes[i]>>4];text+=digits[bytes[i]&15];}
    return text+u"}";
}
inline std::vector<Endpoint> render(){
    std::vector<Endpoint> result;
    for(auto& host:audio::devices())if(host.output_channels){
        Endpoint item;item.guid_text=guid_from_uid(host.uid);item.id=u"{0.0.0.00000000}."+item.guid_text;
        item.guid=parse_windows_guid(item.guid_text,true).value_or(WindowsGuid{});item.device=std::move(host);result.push_back(std::move(item));
    }
    std::stable_partition(result.begin(),result.end(),[](const auto& item){return item.device.default_output;});
    return result;
}
inline std::optional<Endpoint> render_by_guid(const WindowsGuid& guid){
    for(auto& item:render())if(item.guid==guid)return std::move(item);return std::nullopt;
}
// EndpointFormFactor：依 Core Audio 傳輸類型對應。
inline std::uint32_t form_factor(std::uint32_t transport){
    switch(transport){
    case 0x626c746e:return 1;                       // 'bltn' 內建 → Speakers
    case 0x68646d69:case 0x64707274:return 9;       // 'hdmi'、'dprt' → DigitalAudioDisplayDevice
    case 0x626c7565:case 0x626c6561:return 3;       // 'blue'、'blea' → Headphones
    case 0x75736220:case 0x66777265:return 1;       // 'usb '、'fwre' → Speakers
    default:return 10;                              // UnknownFormFactor
    }
}
}
