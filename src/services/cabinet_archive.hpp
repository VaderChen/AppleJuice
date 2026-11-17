#pragma once
#include <zlib.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Microsoft Cabinet（MSCF）讀取：CFHEADER、CFFOLDER、CFFILE 與 CFDATA。SetupAPI 的壓縮資訊、解壓與 SetupIterateCabinet 共用。
// 支援未壓縮（0）與 MSZIP（1：每個 CFDATA 以 "CK" 開頭的 raw deflate，前一區塊的輸出是下一區塊的字典）；
// Quantum（2）與 LZX（3）尚未實作，回報 ERROR_NOT_SUPPORTED。跨卷（續接上一／下一卷）的檔案只回報，不跨卷解壓。
namespace x64arm::cabinet {
struct File {std::string name;std::uint32_t size{},offset{};std::uint16_t folder{},date{},time{},attributes{};};
struct Folder {std::uint32_t start{};std::uint16_t blocks{},compression{};};
struct Archive {
    std::uint32_t size{};std::uint16_t set_id{},index{},flags{};std::uint8_t folder_reserve{},data_reserve{};
    std::string previous_cabinet,previous_disk,next_cabinet,next_disk;
    std::vector<Folder> folders;std::vector<File> files;
};
struct Data {std::vector<std::uint8_t> bytes;std::uint32_t error{};};
constexpr std::uint32_t invalid_data=13,not_supported=50;

inline bool is_cabinet(std::span<const std::uint8_t> bytes){return bytes.size()>=36 && bytes[0]=='M' && bytes[1]=='S' && bytes[2]=='C' && bytes[3]=='F';}

inline std::optional<Archive> parse(std::span<const std::uint8_t> bytes){
    if(!is_cabinet(bytes))return std::nullopt;
    const auto u16=[&](std::size_t at)->std::optional<std::uint16_t>{if(at+2>bytes.size())return std::nullopt;return static_cast<std::uint16_t>(bytes[at]|(bytes[at+1]<<8));};
    const auto u32=[&](std::size_t at)->std::optional<std::uint32_t>{if(at+4>bytes.size())return std::nullopt;return bytes[at]|(std::uint32_t{bytes[at+1]}<<8)|(std::uint32_t{bytes[at+2]}<<16)|(std::uint32_t{bytes[at+3]}<<24);};
    const auto text=[&](std::size_t& at)->std::optional<std::string>{std::string value;while(at<bytes.size() && bytes[at])value.push_back(static_cast<char>(bytes[at++]));if(at>=bytes.size())return std::nullopt;++at;return value;};
    Archive archive;const auto size=u32(8),files_at=u32(16);const auto folder_count=u16(26),file_count=u16(28),flags=u16(30),set=u16(32),index=u16(34);
    if(!size || !files_at || !folder_count || !file_count || !flags || !set || !index || bytes[25]!=1)return std::nullopt;
    archive.size=*size;archive.flags=*flags;archive.set_id=*set;archive.index=*index;std::size_t at=36;
    if(archive.flags&4U){
        const auto header_reserve=u16(36);if(!header_reserve || at+4>bytes.size())return std::nullopt;archive.folder_reserve=bytes[38];archive.data_reserve=bytes[39];at=40+std::size_t{*header_reserve};
    }
    if(archive.flags&1U){auto cabinet=text(at);auto disk=cabinet?text(at):std::nullopt;if(!disk)return std::nullopt;archive.previous_cabinet=*cabinet;archive.previous_disk=*disk;}
    if(archive.flags&2U){auto cabinet=text(at);auto disk=cabinet?text(at):std::nullopt;if(!disk)return std::nullopt;archive.next_cabinet=*cabinet;archive.next_disk=*disk;}
    for(unsigned i=0;i<*folder_count;++i){
        const auto start=u32(at);const auto blocks=u16(at+4),compression=u16(at+6);if(!start || !blocks || !compression)return std::nullopt;
        archive.folders.push_back({*start,*blocks,*compression});at+=8+archive.folder_reserve;
    }
    at=*files_at;
    for(unsigned i=0;i<*file_count;++i){
        const auto file_size=u32(at),offset=u32(at+4);const auto folder=u16(at+8),date=u16(at+10),time=u16(at+12),attributes=u16(at+14);
        if(!file_size || !offset || !folder || !date || !time || !attributes)return std::nullopt;
        std::size_t name_at=at+16;auto name=text(name_at);if(!name)return std::nullopt;
        archive.files.push_back({std::move(*name),*file_size,*offset,*folder,*date,*time,*attributes});at=name_at;
    }
    return archive;
}

// 解出整個資料夾（依 CFDATA 順序串接）；檔案內容為其中 [offset, offset+size)。
inline Data folder_data(std::span<const std::uint8_t> bytes,const Archive& archive,std::size_t index,std::size_t limit=512U*1024U*1024U){
    Data result;if(index>=archive.folders.size()){result.error=invalid_data;return result;}
    const auto& folder=archive.folders[index];const auto method=folder.compression&0x0fU;
    if(method==2 || method==3){result.error=not_supported;return result;}
    if(method>3){result.error=invalid_data;return result;}
    std::size_t at=folder.start;std::vector<std::uint8_t> window;
    for(unsigned block=0;block<folder.blocks;++block){
        if(at+8>bytes.size()){result.error=invalid_data;return result;}
        const std::size_t packed=bytes[at+4]|(std::size_t{bytes[at+5]}<<8),unpacked=bytes[at+6]|(std::size_t{bytes[at+7]}<<8);at+=8+archive.data_reserve;
        if(at+packed>bytes.size() || result.bytes.size()+unpacked>limit){result.error=invalid_data;return result;}
        const auto input=bytes.subspan(at,packed);at+=packed;
        if(method==0){if(packed!=unpacked){result.error=invalid_data;return result;}result.bytes.insert(result.bytes.end(),input.begin(),input.end());continue;}
        if(packed<2 || input[0]!='C' || input[1]!='K' || unpacked>32768){result.error=invalid_data;return result;}
        std::vector<std::uint8_t> output(unpacked);z_stream stream{};
        if(inflateInit2(&stream,-MAX_WBITS)!=Z_OK){result.error=invalid_data;return result;}
        if(!window.empty() && inflateSetDictionary(&stream,window.data(),static_cast<uInt>(window.size()))!=Z_OK){inflateEnd(&stream);result.error=invalid_data;return result;}
        stream.next_in=const_cast<Bytef*>(input.data()+2);stream.avail_in=static_cast<uInt>(packed-2);stream.next_out=output.data();stream.avail_out=static_cast<uInt>(output.size());
        const auto status=inflate(&stream,Z_FINISH);const auto produced=output.size()-stream.avail_out;inflateEnd(&stream);
        if((status!=Z_STREAM_END && status!=Z_OK && status!=Z_BUF_ERROR) || produced!=unpacked){result.error=invalid_data;return result;}
        window.assign(output.begin(),output.end());if(window.size()>32768)window.erase(window.begin(),window.end()-32768);
        result.bytes.insert(result.bytes.end(),output.begin(),output.end());
    }
    return result;
}

// 單一檔案的內容；續接自其他卷（folder 0xfffd／0xffff）或續接到下一卷（0xfffe）的檔案不在本卷完整，回報 ERROR_INVALID_DATA。
inline Data file_data(std::span<const std::uint8_t> bytes,const Archive& archive,const File& file){
    Data result;if(file.folder>=0xfffdU){result.error=invalid_data;return result;}
    auto folder=folder_data(bytes,archive,file.folder);if(folder.error)return folder;
    if(std::size_t{file.offset}+file.size>folder.bytes.size()){result.error=invalid_data;return result;}
    result.bytes.assign(folder.bytes.begin()+file.offset,folder.bytes.begin()+file.offset+file.size);return result;
}
}
