#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm::acm {
inline constexpr std::uint64_t builtin_driver_base=0x6c000001;
enum CodecIndex : unsigned { pcm_codec,ms_adpcm_codec,mpeg_codec,g711_codec,ima_codec,gsm_codec };
inline constexpr std::array<std::string_view,6> builtin_codec_modules{
    "msacm32.dll","msadp32.acm","l3codeca.acm","msg711.acm","imaadp32.acm","msgsm32.acm"
};
inline constexpr unsigned driver_name=1,driver_function=3,driver_notification=4,driver_type_mask=7,driver_global=8;
inline constexpr unsigned status_done=0x10000,status_prepared=0x20000;
inline constexpr unsigned acm_driver_base=0x6000,driver_details=acm_driver_base+10,driver_about=acm_driver_base+11,
    format_tag_details=acm_driver_base+25,format_details=acm_driver_base+26,format_suggest=acm_driver_base+27,
    filter_tag_details=acm_driver_base+50,filter_details=acm_driver_base+51,
    stream_open=acm_driver_base+76,stream_close=acm_driver_base+77,stream_size=acm_driver_base+78,
    stream_convert=acm_driver_base+79,stream_reset=acm_driver_base+80,
    stream_prepare=acm_driver_base+81,stream_unprepare=acm_driver_base+82;

struct RegisteredDriver {
    struct Tag {
        struct Standard {std::vector<std::uint8_t> bytes;std::u16string label;unsigned index;};
        std::u16string label;
        std::uint32_t tag{},size{},standard_count{},index{};
        std::vector<Standard> standards;
    };
    std::uint64_t token{},procedure{},module{},window{};
    unsigned message{},support{};
    std::u16string alias;
    std::vector<std::uint8_t> details;
    std::vector<Tag> formats,filters;
    unsigned open_count{},registrations{1};
    bool enabled{true},global{},loaded{},busy{};
};

struct DriverInstance {
    std::shared_ptr<RegisteredDriver> driver;
    std::uint64_t endpoint{};
    bool busy{};
};
struct DriverStream {
    struct Header {std::uint64_t source,destination;unsigned source_length,destination_length;};
    std::shared_ptr<DriverInstance> instance;
    std::map<std::uint64_t,Header> headers;
    std::uint64_t allocation{},driver_handle{};
    bool busy{},owns_driver{};
};

struct StreamInstanceAbi {
    unsigned pointer;
    unsigned source()const{return pointer;}
    unsigned destination()const{return 2*pointer;}
    unsigned filter()const{return 3*pointer;}
    unsigned callback()const{return 4*pointer;}
    unsigned context()const{return 5*pointer;}
    unsigned flags()const{return 6*pointer;}
    unsigned private_data()const{return flags()+8;}
    unsigned handle()const{return private_data()+pointer;}
    unsigned size()const{return handle()+pointer;}
};
struct StreamHeaderAbi {
    unsigned pointer;
    unsigned source()const{return 8+pointer;}
    unsigned source_length()const{return source()+pointer;}
    unsigned destination()const{return source_length()+8+pointer;}
    unsigned destination_length()const{return destination()+pointer;}
    unsigned convert_flags()const{return destination_length()+8+pointer;}
    unsigned size()const{return pointer==4?84U:124U;}
};

inline constexpr unsigned local_support=0x40000000U,disabled_support=0x80000000U;
inline unsigned registered_support(const RegisteredDriver& driver){
    return driver.support|(driver.global?0:local_support)|(driver.enabled?0:disabled_support);
}
}
