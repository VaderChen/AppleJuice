#pragma once
#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace x64arm::detail {
struct ChildPresentationOptions {
    bool spatial{},interpolation{};
    unsigned upscale{1};
    std::string profile;
    std::array<std::string,4> environment()const{
        return {"X64ARM_AUDIO_ENVIRONMENT="+std::to_string(spatial),
                "X64ARM_PRESENT_UPSCALE="+std::to_string(upscale),
                "X64ARM_PRESENT_INTERPOLATION="+std::to_string(interpolation),
                "X64ARM_PRESENTATION_PROFILE="+profile};
    }
};

// 與 Launcher 的 AJPO v1 原子快照共用契約；不更動父程序環境。
// 已登錄的子 EXE 優先，未登錄者沿用啟動器目前設定，孫程序繼續使用同一快照。
inline std::optional<ChildPresentationOptions> child_presentation_options(const std::filesystem::path& source){
    const auto* file=std::getenv("X64ARM_PRESENTATION_OPTIONS");
    if(!file || !*file)return std::nullopt;
    std::ifstream stream(file,std::ios::binary|std::ios::ate);
    if(!stream)return std::nullopt;
    const auto length=stream.tellg();
    if(length<12 || length>4*1024*1024)return std::nullopt;
    std::vector<unsigned char> data(static_cast<std::size_t>(length));
    stream.seekg(0);
    if(!stream.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size())))return std::nullopt;
    if(data[0]!='A' || data[1]!='J' || data[2]!='P' || data[3]!='O' || data[4]!=1 || data[5]>1 || data[6] || data[7])return std::nullopt;
    const auto count=std::uint32_t{data[8]}|(std::uint32_t{data[9]}<<8)|(std::uint32_t{data[10]}<<16)|(std::uint32_t{data[11]}<<24);
    if(count>4096)return std::nullopt;
    const auto* inherited=std::getenv("X64ARM_PRESENTATION_PROFILE");
    const std::string_view inherited_profile=inherited?inherited:"";
    ChildPresentationOptions result;
    result.spatial=data[5]!=0;
    if(const auto* text=std::getenv("X64ARM_PRESENT_UPSCALE"))result.upscale=std::string_view(text)=="2"?2U:1U;
    if(const auto* text=std::getenv("X64ARM_PRESENT_INTERPOLATION"))result.interpolation=std::string_view(text)=="1";
    result.profile=inherited_profile;
    std::error_code error;
    auto target=std::filesystem::weakly_canonical(source,error);
    if(error)target=source.lexically_normal();
    unsigned selected=0;
    std::size_t at=12;
    for(std::uint32_t record=0;record<count;++record){
        if(data.size()-at<20)return std::nullopt;
        const auto size=std::size_t{data[at]}|(std::size_t{data[at+1]}<<8);
        const auto upscale=data[at+2],interpolation=data[at+3];
        if(!size || size>32768 || size>data.size()-at-20 || (upscale!=1 && upscale!=2) || interpolation>1)return std::nullopt;
        std::string profile;
        constexpr char hex[]="0123456789abcdef";
        for(std::size_t i=0;i<16;++i){const auto value=data[at+4+i];profile+=hex[value>>4];profile+=hex[value&15];}
        const std::string path(reinterpret_cast<const char*>(data.data()+at+20),size);
        if(path.find('\0')!=path.npos || !std::filesystem::path(path).is_absolute())return std::nullopt;
        const unsigned priority=std::filesystem::path(path)==target?2U:profile==inherited_profile?1U:0U;
        if(priority>selected){result.upscale=upscale;result.interpolation=interpolation!=0;result.profile=std::move(profile);selected=priority;}
        at+=20+size;
    }
    if(at!=data.size())return std::nullopt;
    return result;
}
}
