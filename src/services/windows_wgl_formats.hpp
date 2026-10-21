#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace x64arm {
// 傳統 PFD 與 ARB 查詢使用相同的實際 drawable 格式。
inline std::array<std::uint8_t,40> wgl_pixel_format(){
    std::array<std::uint8_t,40> p{};
    p[0]=40;p[2]=1;p[4]=0x25;p[9]=32;
    p[10]=p[12]=p[14]=p[16]=8;p[11]=16;p[13]=8;p[17]=24;
    const auto* backend=std::getenv("X64ARM_GL_BACKEND");
    p[23]=backend && std::string_view(backend)=="metal"?32:24;p[24]=8;
    return p;
}
struct WglFormatAttribute {int value{};bool minimum{},ignored{},main_plane_only{};};
inline std::optional<WglFormatAttribute> wgl_format_attribute(unsigned attribute){
    const auto p=wgl_pixel_format();
    switch(attribute){
    case 0x2000:return WglFormatAttribute{1,false,true,true};
    case 0x2001:return WglFormatAttribute{1,false,false,true};
    case 0x2002:case 0x2004:case 0x2005:case 0x2006:return WglFormatAttribute{0,false,false,true};
    case 0x2003:return WglFormatAttribute{0x2027}; // 完整硬體加速。
    case 0x2007:return WglFormatAttribute{0x202a,false,false,true}; // 不保證不同後端交換後的後緩衝內容。
    case 0x2008:case 0x2009:return WglFormatAttribute{0,true,false,true};
    case 0x200a:case 0x2037:case 0x2038:case 0x2039:case 0x203a:case 0x203b:return WglFormatAttribute{0,false,true};
    case 0x200c:case 0x200d:case 0x200e:return WglFormatAttribute{1,false,true}; // 無 overlay，選擇時忽略。
    case 0x200f:return WglFormatAttribute{0};
    case 0x2010:case 0x2011:return WglFormatAttribute{1};
    case 0x2012:return WglFormatAttribute{0};
    case 0x2013:return WglFormatAttribute{0x202b};
    case 0x2014:return WglFormatAttribute{p[10]+p[12]+p[14],true}; // 不包含 alpha。
    case 0x2015:return WglFormatAttribute{p[10],true};case 0x2016:return WglFormatAttribute{p[11],false,true};
    case 0x2017:return WglFormatAttribute{p[12],true};case 0x2018:return WglFormatAttribute{p[13],false,true};
    case 0x2019:return WglFormatAttribute{p[14],true};case 0x201a:return WglFormatAttribute{p[15],false,true};
    case 0x201b:return WglFormatAttribute{p[16],true};case 0x201c:return WglFormatAttribute{p[17],false,true};
    case 0x201d:case 0x201e:case 0x201f:case 0x2020:case 0x2021:case 0x2024:return WglFormatAttribute{0,true};
    case 0x2022:return WglFormatAttribute{p[23],true};case 0x2023:return WglFormatAttribute{p[24],true};
    default:return {};
    }
}
}
