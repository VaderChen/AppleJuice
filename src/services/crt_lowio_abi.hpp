#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
namespace x64arm::crt_lowio {
// 系統 MSVCRT 的 lowio ABI；VC8+／UCRT 的不同結構不使用這份版面。
struct Layout {unsigned pointer{},stride{},flags{},lookahead{},extended{},critical{};};
inline constexpr Layout layout(bool x86){return x86?Layout{4,36,4,5,8,12}:Layout{8,56,8,9,12,16};}
inline constexpr unsigned records_per_block=32,descriptor_limit=8192,block_count=descriptor_limit/records_per_block;
inline constexpr std::uint8_t opened=1,eof=2,read_newline=4,pipe=8,noinherit=16,append=32,tty=64,text=128;
inline constexpr std::uint32_t utf8=1,utf16=2,critical_initialized=4,unicode=8;
enum class TextMode : std::uint8_t { Ansi, Utf8, Utf16 };
inline constexpr TextMode text_mode(std::uint32_t extended){return extended&utf8?TextMode::Utf8:extended&utf16?TextMode::Utf16:TextMode::Ansi;}
inline constexpr std::uint32_t mode_value(bool translated,TextMode mode){return !translated?0x8000U:mode==TextMode::Ansi?0x4000U:0x10000U;}
inline constexpr bool valid_mode(std::uint32_t mode){return mode==0x4000 || mode==0x8000 || mode==0x10000 || mode==0x20000 || mode==0x40000;}
inline constexpr std::uint32_t with_mode(std::uint32_t extended,std::uint32_t mode){return (extended&~(utf8|utf16))|(mode==0x40000?utf8:mode==0x10000 || mode==0x20000?utf16:0U);}
struct InheritedDescriptor {std::uint32_t descriptor{};std::uint64_t handle{};std::uint8_t flags{};};
// STARTUPINFO 的保留區塊先放 DWORD count，再放逐 fd flags 與未對齊的 HANDLE 陣列。
inline std::optional<std::vector<InheritedDescriptor>> inherited_descriptors(std::span<const std::uint8_t> bytes,unsigned pointer){
    if((pointer!=4 && pointer!=8) || bytes.size()<4)return std::nullopt;
    std::uint32_t count=0;for(unsigned i=0;i<4;++i)count|=std::uint32_t(bytes[i])<<(8*i);
    if(count>descriptor_limit || bytes.size()!=4ULL+std::uint64_t(count)*(pointer+1))return std::nullopt;
    std::vector<InheritedDescriptor> result;
    for(std::uint32_t fd=0;fd<count;++fd){std::uint64_t handle=0;const auto at=4+count+std::uint64_t(fd)*pointer;
        for(unsigned i=0;i<pointer;++i)handle|=std::uint64_t(bytes[static_cast<std::size_t>(at+i)])<<(8*i);
        const auto flags=bytes[4+fd];const auto invalid=pointer==4?UINT64_C(0xffffffff):UINT64_MAX;
        if((flags&(opened|noinherit))==opened && handle!=invalid && handle!=(invalid-1))result.push_back({fd,handle,flags});
    }
    return result;
}
}
