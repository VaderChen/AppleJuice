#pragma once
#include "windows_wmf.hpp"
#include <cmath>

namespace x64arm::gdiplus_meta {
// MetafileHeader 使用固定寬度欄位，x86 與 x64 都只複製 ENHMETAHEADER3 的前 88 bytes。
struct Header {
    std::uint32_t type{},size{},version{},flags{};
    float dpi_x{},dpi_y{};
    std::int32_t x{},y{},width{},height{};
    std::array<std::uint8_t,88> native{};
    std::uint32_t plus_size{},logical_x{},logical_y{};
};
static_assert(sizeof(Header)==140 && offsetof(Header,native)==40 && offsetof(Header,plus_size)==128);

struct Image {
    Header header;
    std::array<float,4> bounds{};
    std::uint64_t emf{},wmf{};
    bool own_emf{},own_wmf{};
    std::uint8_t format{0xac}; // ImageFormatEMF 或 ImageFormatWMF。
};

inline std::int32_t rounded(float coordinate){
    const double value=std::floor(static_cast<double>(coordinate)+0.5);
    if(!std::isfinite(value) || value<INT32_MIN || value>INT32_MAX)throw std::invalid_argument("中繼檔邊界超出範圍");
    return static_cast<std::int32_t>(value);
}

inline Image emf_image(const std::vector<std::uint8_t>& data){
    const auto records=emf_validate(data);
    Image result{};
    auto& h=result.header;
    h.type=3;h.size=emf_get(data,48);h.version=emf_get(data,44);
    std::copy_n(data.begin(),h.native.size(),h.native.begin());
    const auto integer=[&](std::size_t at){return static_cast<std::int32_t>(emf_get(data,at));};
    if(integer(80)<=0 || integer(84)<=0)throw std::invalid_argument("EMF 裝置尺寸無效");
    h.dpi_x=static_cast<float>(integer(72)*25.4/integer(80));
    h.dpi_y=static_cast<float>(integer(76)*25.4/integer(84));
    const double left=integer(24),top=integer(28);
    result.bounds={static_cast<float>(left*h.dpi_x/2540),static_cast<float>(top*h.dpi_y/2540),
                   static_cast<float>((integer(32)-left)*h.dpi_x/2540),static_cast<float>((integer(36)-top)*h.dpi_y/2540)};
    h.x=rounded(result.bounds[0]);h.y=rounded(result.bounds[1]);
    h.width=rounded(result.bounds[2]);h.height=rounded(result.bounds[3]);
    if(records.size()>1){
        const auto [at,length]=records[1];
        const auto comment=std::span(data).subspan(at,length);
        if(emf_get(comment,0)==70 && length>=16 && emf_get(comment,12)==0x2b464d45U){
            const auto payload=emf_get(comment,8);
            if(payload<32 || payload>length-12 || wmf_word(comment,16)!=0x4001 ||
               emf_get(comment,20)<28 || emf_get(comment,20)>payload-4 || emf_get(comment,24)!=16)
                throw std::invalid_argument("EMF+ 標頭記錄無效");
            h.type=4+(wmf_word(comment,18)&1);h.plus_size=emf_get(comment,20);
            h.version=emf_get(comment,28);h.flags=emf_get(comment,32);
            h.logical_x=emf_get(comment,36);h.logical_y=emf_get(comment,40);
        }
    }
    return result;
}

inline void set_wmf_header(Image& image,std::span<const std::uint8_t> data,std::span<const std::uint8_t> placeable){
    auto& h=image.header;
    h.type=1;h.size=static_cast<std::uint32_t>(windows_metafile_size(data));h.version=wmf_word(data,4);
    h.native.fill(0);std::copy_n(data.begin(),18,h.native.begin());
    h.flags=h.plus_size=h.logical_x=h.logical_y=0;
    if(!placeable.empty()){
        const auto coord=[&](unsigned at){return static_cast<std::int16_t>(wmf_word(placeable,at));};
        const auto inch=wmf_word(placeable,14);
        if(!inch)throw std::invalid_argument("WMF 缺少每英吋單位數");
        h.type=2;h.dpi_x=h.dpi_y=static_cast<float>(inch);
        h.width=coord(10)-coord(6);h.height=coord(12)-coord(8);
        image.bounds={coord(6)/h.dpi_x,coord(8)/h.dpi_y,static_cast<float>(h.width),static_cast<float>(h.height)};
    }
}
}
