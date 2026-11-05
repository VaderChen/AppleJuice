#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace x64arm::gdiplus_codec {
using Guid=std::array<std::uint8_t,16>;
constexpr Guid guid(std::uint32_t a,std::uint16_t b,std::uint16_t c,std::array<std::uint8_t,8> tail){
    Guid result{};for(unsigned i=0;i<4;++i)result[i]=static_cast<std::uint8_t>(a>>(i*8));
    for(unsigned i=0;i<2;++i){result[4+i]=static_cast<std::uint8_t>(b>>(i*8));result[6+i]=static_cast<std::uint8_t>(c>>(i*8));}
    for(unsigned i=0;i<8;++i)result[8+i]=tail[i];return result;
}
constexpr auto quality=guid(0x1d5be4b5,0xfa4a,0x452d,{0x9c,0xdd,0x5d,0xb3,0x51,0x05,0xe7,0xeb});
constexpr auto compression=guid(0xe09d739d,0xccd4,0x44ee,{0x8e,0xba,0x3f,0xbf,0x8b,0xe4,0xfc,0x58});
struct Codec {
    std::uint32_t index,format_id,format;
    std::u16string_view name,extension,mime;
    std::string_view signatures;std::uint32_t signature_size;
    constexpr Guid clsid()const{return guid(0x557cf400+index,0x1a04,0x11d3,{0x9a,0x73,0,0,0xf8,0x1e,0xf3,0x2e});}
    constexpr Guid image_format()const{return guid(format_id,0x0728,0x11d3,{0x9d,0x7b,0,0,0xf8,0x1e,0xf3,0x2e});}
};
// 列出的格式皆由共用像素編解碼器承接；不列出尚無解碼器的 EMF／WMF。
inline constexpr std::array codecs{
    Codec{0,0xb96b3cab,0,u"BMP",u"*.BMP;*.DIB;*.RLE",u"image/bmp",{"BM",2},2},
    Codec{1,0xb96b3cae,1,u"JPEG",u"*.JPG;*.JPEG;*.JPE;*.JFIF",u"image/jpeg",{"\xff\xd8\xff",3},3},
    Codec{2,0xb96b3cb0,11,u"GIF",u"*.GIF",u"image/gif",{"GIF87aGIF89a",12},6},
    Codec{5,0xb96b3cb1,10,u"TIFF",u"*.TIF;*.TIFF",u"image/tiff",{"II\x2a\0MM\0\x2a",8},4},
    Codec{6,0xb96b3caf,3,u"PNG",u"*.PNG",u"image/png",{"\x89PNG\r\n\x1a\n",8},8}};
inline const Codec* find(const Guid& clsid){for(const auto& codec:codecs)if(codec.clsid()==clsid)return &codec;return nullptr;}
inline std::vector<std::uint8_t> information(bool small,std::uint64_t base){
    const unsigned unit=small?4:8,size=small?76:104;
    std::vector<std::uint8_t> bytes(size*codecs.size());
    const auto put=[&](std::size_t at,std::uint64_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(i*8));};
    const auto text=[&](std::size_t slot,std::u16string_view value){
        if(bytes.size()%2)bytes.push_back(0);put(slot,base+bytes.size(),unit);
        for(const auto ch:value){bytes.push_back(ch&255);bytes.push_back(ch>>8);}bytes.push_back(0);bytes.push_back(0);
    };
    for(unsigned i=0;i<codecs.size();++i){
        const auto& codec=codecs[i];const auto at=i*size,flags=at+32+5*unit;
        for(unsigned n=0;n<16;++n){bytes[at+n]=codec.clsid()[n];bytes[at+16+n]=codec.image_format()[n];}
        text(at+32,codec.name);text(at+32+2*unit,codec.name);text(at+32+3*unit,codec.extension);text(at+32+4*unit,codec.mime);
        put(flags,0x10007,4);put(flags+4,1,4);put(flags+8,codec.signatures.size()/codec.signature_size,4);put(flags+12,codec.signature_size,4);
        put(flags+16,base+bytes.size(),unit);bytes.insert(bytes.end(),codec.signatures.begin(),codec.signatures.end());
        put(flags+16+unit,base+bytes.size(),unit);bytes.insert(bytes.end(),codec.signatures.size(),255);
    }
    return bytes;
}
}
