#pragma once

#include "avi_frame_image.hpp"
#include "x64arm/unicode.hpp"

namespace x64arm::vfw {
using avi::fourcc;
inline constexpr std::uint32_t video_type=fourcc('v','i','d','c'), version=0x040003b6, interface_version=0x0104;
inline constexpr std::uint32_t icinfo_size=568;
inline constexpr std::u16string_view drivers32=u"software\\microsoft\\windows nt\\currentversion\\drivers32";
inline constexpr int ok=0, unsupported=-1, bad_format=-2, no_memory=-3, bad_flags=-5,
    bad_parameter=-6, bad_size=-7, bad_handle=-8;

enum class Backend { Dib, RunLength, Jpeg, Png, Video1, FileDriver, Callback, InstallableDriver };
struct Driver {
    std::uint32_t type{},handler{};
    Backend backend{};
    std::u16string name,description,file;
    std::uint64_t procedure{},driver{};
};
struct DrawChannel {
    std::uint64_t surface{},dc{},owner_window{},ticks{};
    avi::Bytes format;
    std::array<std::uint32_t,4> dest{},source{};
    std::uint32_t flags{},rate{},scale{};
    std::int32_t sample{},to{};
    bool running{},play_range{},pending{};
};
struct Codec {
    Driver driver;
    std::uint64_t instance{};
    bool owns_driver{},busy{},decompressing{},compressing{},standalone{};
    std::uint32_t quality{7500};
    avi::Bytes rle_encoded_format,rle_decoded_format,rle_input,rle_output;
    avi::Bytes video1_source,video1_target;
    std::unique_ptr<media::Video1Decoder> video1_decoder;
    std::unique_ptr<media::Video1Encoder> video1_encoder;
    DrawChannel drawing;
};

inline constexpr unsigned compress_format=0x4004, compress_size=0x4005, compress_query=0x4006,
    compress_begin=0x4007, compress_frame=0x4008, compress_end=0x4009, default_key_rate=0x402a,
    default_quality=0x501e,get_quality=0x501f,set_quality=0x5020;
inline constexpr unsigned get_state=0x5000,set_state=0x5001,codec_about=0x500b,codec_configure=0x500a,
    decompress_palette=0x401e;
inline constexpr unsigned decompress_ex_begin=0x403c,decompress_ex_query=0x403d,decompress_ex_frame=0x403e,
    decompress_ex_end=0x403f;
inline constexpr std::uint32_t mrle=fourcc('M','R','L','E'),rle4=fourcc('R','L','E','4'),rle8=fourcc('R','L','E','8');
inline constexpr std::uint32_t video1_tag=fourcc('M','S','V','C');
inline constexpr unsigned rle_default_quality=8500,rle_key_rate=15;
inline constexpr unsigned draw_begin_message=0x400f, draw_palette_message=0x4010, draw_update_message=0x4011,
    draw_start_message=0x4012, draw_stop_message=0x4013, draw_end_message=0x4015,
    draw_query_message=0x401f, draw_get_time_message=0x4020, draw_frame_message=0x4021,
    draw_window_message=0x4022, draw_set_time_message=0x4023, draw_realize_message=0x4024,
    draw_flush_message=0x4025, draw_render_message=0x4026, draw_start_play_message=0x4027,
    draw_stop_play_message=0x4028, draw_buffers_message=0x4029, draw_suggest_message=0x4032,
    draw_change_palette_message=0x4033;
inline constexpr unsigned renderer_query=1, renderer_fullscreen=2, renderer_hdc=4, renderer_animate=8,
    renderer_continue=0x10, renderer_render=0x80, renderer_hurry=0x80000000U, renderer_update=0x40000000U,
    renderer_preroll=0x20000000U, renderer_nullframe=0x10000000U, renderer_non_key=0x08000000U;
inline constexpr unsigned draw_begin_size(unsigned unit){return unit==4?60:80;}
inline constexpr unsigned draw_frame_size(unsigned unit){return unit==4?20:32;}
inline constexpr unsigned compress_previous(unsigned unit){return unit==4?40:72;}
inline constexpr unsigned compress_wire_size(unsigned unit){return compress_previous(unit)+2*unit;}
inline bool builtin(Backend value) {
    return value==Backend::Dib || value==Backend::RunLength || value==Backend::Jpeg || value==Backend::Png ||
        value==Backend::Video1;
}
inline void put_pointer(avi::Bytes& bytes,unsigned offset,unsigned unit,std::uint64_t value){
    avi::put(bytes,offset,static_cast<std::uint32_t>(value));
    if(unit==8)avi::put(bytes,offset+4,static_cast<std::uint32_t>(value>>32));
}
inline std::int32_t draw_sample(const DrawChannel& draw,std::uint64_t now){
    if(!draw.running || now<=draw.ticks || !draw.rate || !draw.scale)return draw.sample;
    const auto frames=static_cast<long double>(now-draw.ticks)*draw.rate/(10000000.0L*draw.scale);
    const auto maximum=draw.play_range?draw.to:INT32_MAX;
    return static_cast<std::int32_t>(std::min<long double>(maximum,static_cast<long double>(draw.sample)+frames));
}

inline std::uint64_t deliver_error(int value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}
inline std::uint32_t folded(std::uint32_t value) {
    std::uint32_t result=0;
    for(unsigned i=0;i<4;++i){
        auto c=(value>>(8*i))&255U;
        if(c>='A' && c<='Z')c+='a'-'A';
        result|=c<<(i*8);
    }
    return result;
}
inline bool matches(std::uint32_t a,std::uint32_t b) {return folded(a)==folded(b);}
inline bool rle_handler(std::uint32_t handler){
    return matches(handler,mrle) || matches(handler,rle4) || matches(handler,rle8) ||
        matches(handler,fourcc('R','L','E',' '));
}
inline bool rle_depth(std::uint32_t handler,unsigned depth){
    if(matches(handler,rle4))return depth==4;
    if(matches(handler,rle8))return depth==8;
    return depth==4 || depth==8;
}
inline std::u16string fcc_text(std::uint32_t fcc) {
    std::u16string out;
    for(unsigned i=0;i<4;++i)out.push_back(static_cast<char16_t>((fcc>>(i*8))&255U));
    return out;
}
inline std::uint32_t read_fourcc(std::u16string_view text) {
    if(text.size()!=4)return 0;
    std::uint32_t answer=0;
    for(unsigned i=0;i<4;++i){
        if(text[i]<0x20 || text[i]>0x7e || text[i]==u'\\')return 0;
        answer|=static_cast<std::uint32_t>(text[i])<<(8*i);
    }
    return answer;
}
inline std::u16string driver_name(std::uint32_t type,std::uint32_t handler) {
    return fcc_text(folded(type))+u"."+fcc_text(folded(handler));
}
inline bool codec_mode(std::uint32_t value) {
    return (value>=1 && value<=5) || value==8;
}
inline Driver rle_driver(){
    return {video_type,mrle,Backend::RunLength,u"RLE",u"Run-length encoded bitmap",u"msrle32.dll"};
}
inline Driver video1_driver(){
    return {video_type,video1_tag,Backend::Video1,u"Video 1",u"Microsoft Video 1",u"msvidc32.dll"};
}
inline bool driver_matches_handler(const Driver& driver,std::uint32_t handler){
    return matches(driver.handler,handler) || (driver.backend==Backend::Video1 && media::video1_fourcc(handler));
}
inline std::vector<Driver> builtins() {
    return {
        {video_type,fourcc('D','I','B',' '),Backend::Dib,u"DIB",u"Uncompressed RGB and bitfields",{}},
        rle_driver(),
        {video_type,fourcc('M','J','P','G'),Backend::Jpeg,u"MJPEG",u"Motion JPEG",{}},
        {video_type,fourcc('P','N','G',' '),Backend::Png,u"PNG",u"PNG frame codec",{}},
        video1_driver(),
    };
}
inline avi::Bytes pack_info(const Driver& driver) {
    avi::Bytes bytes(icinfo_size);
    avi::put(bytes,0,icinfo_size);
    avi::put(bytes,4,driver.type);
    avi::put(bytes,8,driver.handler);
    if(builtin(driver.backend)){
        // VIDCF_DRAW、VIDCF_QUALITY、VIDCF_TEMPORAL。
        const auto options=driver.backend==Backend::RunLength?5U:driver.backend==Backend::Video1?4U:
            driver.backend==Backend::Jpeg?1U:0U;
        avi::put(bytes,12,0x10|options);
    }
    avi::put(bytes,16,version);
    avi::put(bytes,20,interface_version);
    const auto text=[&](unsigned offset,const std::u16string& value,unsigned capacity){
        const auto data=utf16_bytes(value.substr(0,capacity-1));
        std::copy(data.begin(),data.end(),bytes.begin()+offset);
    };
    text(24,driver.name,16);
    text(56,driver.description,128);
    text(312,driver.file,128);
    return bytes;
}
inline bool accepts(Backend backend,const avi::FrameLayout& format) {
    switch(backend){
        case Backend::Dib:return format.raw();
        case Backend::RunLength:return format.compression==1 || format.compression==2;
        case Backend::Jpeg:return format.compression==4 || matches(format.compression,fourcc('M','J','P','G')) ||
            matches(format.compression,fourcc('J','P','E','G'));
        case Backend::Png:return format.compression==5 || matches(format.compression,fourcc('P','N','G',' '));
        case Backend::Video1:return media::video1_fourcc(format.compression);
        case Backend::FileDriver:case Backend::Callback:case Backend::InstallableDriver:return false;
    }
    return false;
}
}
