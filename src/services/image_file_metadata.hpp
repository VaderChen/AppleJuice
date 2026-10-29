#pragma once
#include "x64arm/readonly_files.hpp"
#include "x64arm/security_descriptor.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace x64arm::image_file {
[[noreturn]] inline void invalid(){throw std::invalid_argument("執行映像中繼資料範圍或格式不合法");}

// 查詢只借用已授權描述元。4 KiB 視窗存活於本次呼叫，不加入內容快取，
// 不改變檔案位置；overlay、檔案偏移與 SizeOfImage 都不決定配置量。
class Reader {
public:
    static constexpr std::size_t window_capacity=4096;
    explicit Reader(const HostFile& file):file_(file){
        const auto info=file_.information();if(info.error)throw SecurityDescriptorError{info.error};before_=info.metadata;
    }
    std::uint64_t size()const{return before_.size;}
    bool fits(std::uint64_t at,std::uint64_t count)const{return at<=size() && count<=size()-at;}
    void check(std::uint64_t at,std::uint64_t count)const{if(!fits(at,count))invalid();}
    void read(std::uint64_t at,std::span<std::uint8_t> output){
        check(at,output.size());
        if(output.size()>window_capacity){exact(at,output);return;}
        while(!output.empty()){
            if(at<window_at_ || at-window_at_>=window_size_){
                window_at_=at&~std::uint64_t{window_capacity-1};
                window_size_=static_cast<std::size_t>(std::min<std::uint64_t>(window_capacity,size()-window_at_));
                exact(window_at_,std::span{window_}.first(window_size_));
            }
            const auto offset=static_cast<std::size_t>(at-window_at_),count=std::min(output.size(),window_size_-offset);
            std::copy_n(window_.begin()+static_cast<std::ptrdiff_t>(offset),count,output.begin());at+=count;output=output.subspan(count);
        }
    }
    std::uint64_t integer(std::uint64_t at,unsigned count){
        if(count>8)invalid();std::array<std::uint8_t,8> bytes{};read(at,std::span{bytes}.first(count));
        std::uint64_t result=0;for(unsigned n=0;n<count;++n)result|=std::uint64_t{bytes[n]}<<(n*8);return result;
    }
    void finish()const{
        const auto info=file_.information();if(info.error)throw SecurityDescriptorError{info.error};const auto& after=info.metadata;
        if(before_.device!=after.device || before_.id!=after.id || before_.size!=after.size ||
           before_.modified!=after.modified || before_.changed!=after.changed)throw SecurityDescriptorError{30};
    }
    std::uint64_t read_bytes()const{return read_bytes_;}
    std::uint64_t read_calls()const{return read_calls_;}
private:
    void exact(std::uint64_t at,std::span<std::uint8_t> output){
        while(!output.empty()){
            const auto result=file_.read(output,at);++read_calls_;read_bytes_+=result.bytes;
            if(result.error)throw SecurityDescriptorError{result.error};
            if(!result.bytes || result.bytes>output.size())throw SecurityDescriptorError{30};
            at+=result.bytes;output=output.subspan(static_cast<std::size_t>(result.bytes));
        }
    }
    const HostFile& file_;FileMetadata before_;
    std::array<std::uint8_t,window_capacity> window_{};
    std::uint64_t window_at_{UINT64_MAX},read_bytes_{},read_calls_{};std::size_t window_size_{};
};

struct PeHeader {std::uint64_t at{};std::uint16_t machine{},sections{},optional_size{},flags{},magic{},subsystem{};};
inline PeHeader pe_header(Reader& file,std::uint64_t at){
    file.check(at,24);if(at<64 || file.integer(at,4)!=0x4550)invalid();
    PeHeader result{at,static_cast<std::uint16_t>(file.integer(at+4,2)),static_cast<std::uint16_t>(file.integer(at+6,2)),
        static_cast<std::uint16_t>(file.integer(at+20,2)),static_cast<std::uint16_t>(file.integer(at+22,2))};
    if(result.optional_size<70)invalid();file.check(at+24,result.optional_size);
    result.magic=static_cast<std::uint16_t>(file.integer(at+24,2));result.subsystem=static_cast<std::uint16_t>(file.integer(at+24+68,2));return result;
}
inline std::optional<std::uint32_t> binary_type(Reader& file,std::string_view path){
    std::optional<std::uint32_t> result;
    if(file.fits(0,64) && file.integer(0,2)==0x5a4d){
        const auto at=file.integer(60,4);
        if(at>=64 && file.fits(at,4) && file.integer(at,4)==0x4550){
            const auto header=pe_header(file,at);
            if((header.flags&2U) && !(header.flags&0x2000U)){
                if(header.magic==0x10b)result=header.subsystem==7?4U:0U;else if(header.magic==0x20b)result=6;
            }
        }else if(at>=64 && file.fits(at,2) && file.integer(at,2)==0x454e){
            if(file.fits(at,64) && !(file.integer(at+12,2)&0x8000U)){
                const auto target=file.integer(at+54,1);if(target==2 || target==4)result=2;else if(target==1)result=5;
            }
        }else if(at>=64 && file.fits(at,2) && (file.integer(at,2)==0x454c || file.integer(at,2)==0x584c)){
            // LE／LX 沿用既有不支援結果。
        }else result=1;
    }else if(file.size()){
        const auto dot=path.find_last_of('.');auto extension=dot==path.npos?std::string{}:std::string(path.substr(dot));
        for(auto& c:extension)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
        if(extension==".com")result=1;else if(extension==".pif")result=3;
    }
    file.finish();return result;
}

// 只建立固定大小的 RVA→檔案範圍描述。執行時的匯入、重定位、TLS、
// 指令與完整資源驗證仍交給既有 SDK 載入器，不建立第二份映像。
class PeMetadata {
public:
    static constexpr std::size_t section_capacity=96;
    explicit PeMetadata(Reader& file):file_(file){
        file.check(0,64);if(file.integer(0,2)!=0x5a4d)invalid();header_=pe_header(file,file.integer(60,4));
        const bool small=header_.machine==0x14c;
        if((!small && header_.machine!=0x8664) || header_.magic!=(small?0x10b:0x20b) ||
           !header_.sections || header_.sections>section_capacity || !(header_.flags&2U) || (header_.flags&0x1000U))invalid();
        const auto opt=header_.at+24;const unsigned directory_offset=small?96U:112U;
        if(header_.optional_size<directory_offset)invalid();
        const auto alignment=file.integer(opt+32,4),file_alignment=file.integer(opt+36,4);
        image_size_=file.integer(opt+56,4);headers_=file.integer(opt+60,4);
        const auto power2=[](std::uint64_t value){return value && !(value&(value-1));};
        if(!power2(alignment) || alignment<4096 || !power2(file_alignment) || file_alignment<512 ||
           file_alignment>65536 || file_alignment>alignment || !image_size_ || image_size_%alignment ||
           !headers_ || headers_>image_size_ || headers_%file_alignment)invalid();
        const auto directories=file.integer(opt+directory_offset-4,4),table=opt+header_.optional_size;
        if(directories>16 || directory_offset+directories*8>header_.optional_size || table>headers_ ||
           std::uint64_t{header_.sections}*40>headers_-table)invalid();
        file.check(0,headers_);file.check(table,std::uint64_t{header_.sections}*40);
        for(unsigned n=0;n<header_.sections;++n){
            const auto at=table+n*40,virtual_size=file.integer(at+8,4),rva=file.integer(at+12,4),raw_size=file.integer(at+16,4),raw=file.integer(at+20,4);
            const auto extent=virtual_size?virtual_size:raw_size;if(!extent)continue;
            const auto rounded=(extent+4095)&~UINT64_C(4095),source_size=raw?raw_size:0;
            if(rva%alignment || rva<headers_ || rva>=image_size_ || rounded>image_size_-rva ||
               (source_size && raw%file_alignment))invalid();
            if(source_size)file.check(raw,source_size);
            for(std::size_t i=0;i<count_;++i){const auto& other=sections_[i];
                if((rva<other.rva+other.size && other.rva<rva+rounded) ||
                   (source_size && other.raw_size && raw<other.raw+other.raw_size && other.raw<raw+source_size))invalid();
            }
            sections_[count_++]={rva,rounded,raw,source_size};
        }
        if(directories>2){resource_=file.integer(opt+directory_offset+16,4);resource_size_=file.integer(opt+directory_offset+20,4);}
        if((resource_==0)!=(resource_size_==0))invalid();if(resource_)locate(resource_,resource_size_);
    }
    const PeHeader& header()const{return header_;}
    struct ResourceLocation {std::uint64_t rva{},size{};};
    std::optional<ResourceLocation> resource_location(std::uint16_t type,std::uint16_t id,std::size_t maximum,std::uint16_t language=0){
        if(!resource_)return {};
        std::optional<Entry> selected;
        visit(0,[&](Entry entry){if(entry.name==type || named_type(entry.name,type)){selected=entry;return false;}return true;});
        if(!selected)return {};
        const auto names=child(*selected);if(!names)invalid();selected.reset();
        visit(names,[&](Entry entry){if(entry.name==id){selected=entry;return false;}return true;});
        if(!selected)return {};
        const auto languages=child(*selected);if(!languages || languages==names)invalid();selected.reset();unsigned best=4;
        visit(languages,[&](Entry entry){
            const unsigned rank=entry.name==0?0U:entry.name==language?1U:entry.name==(language&0x3ffU)?2U:3U;
            if(rank<best){best=rank;selected=entry;}return best!=0;
        });
        if(!selected)return {};if(selected->target&0x80000000U)invalid();
        const auto at=selected->target;directory_check(at,16);
        const auto rva=integer(at,4),size=integer(std::uint64_t{at}+4,4);
        if(integer(std::uint64_t{at}+12,4))invalid();
        locate(rva,size);if(size>maximum)throw SecurityDescriptorError{14001};
        return ResourceLocation{rva,size};
    }
    std::optional<std::vector<std::uint8_t>> resource(std::uint16_t type,std::uint16_t id,std::size_t maximum,std::uint16_t language=0){
        const auto location=resource_location(type,id,maximum,language);if(!location)return {};
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(location->size));read_rva(location->rva,payload);return payload;
    }
private:
    struct Section{std::uint64_t rva{},size{},raw{},raw_size{};};
    struct Entry{std::uint32_t name{},target{};};
    Section locate(std::uint64_t rva,std::uint64_t size)const{
        if(rva>image_size_ || size>image_size_-rva)invalid();
        if(rva<=headers_ && size<=headers_-rva)return {0,headers_,0,headers_};
        for(std::size_t i=0;i<count_;++i){const auto& section=sections_[i];
            if(rva>=section.rva && rva-section.rva<=section.size && size<=section.size-(rva-section.rva))return section;
        }
        invalid();
    }
    void read_rva(std::uint64_t rva,std::span<std::uint8_t> output){
        const auto section=locate(rva,output.size());const auto offset=rva-section.rva;
        std::fill(output.begin(),output.end(),0);
        if(offset<section.raw_size){const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(output.size(),section.raw_size-offset));file_.read(section.raw+offset,output.first(count));}
    }
    void directory_check(std::uint64_t at,std::uint64_t size)const{
        if(at>resource_size_ || size>resource_size_-at)invalid();locate(resource_+at,size);
    }
    std::uint64_t integer(std::uint64_t at,unsigned count){
        directory_check(at,count);std::array<std::uint8_t,8> bytes{};read_rva(resource_+at,std::span{bytes}.first(count));
        std::uint64_t result=0;for(unsigned n=0;n<count;++n)result|=std::uint64_t{bytes[n]}<<(n*8);return result;
    }
    template<class Visitor>void visit(std::uint32_t at,Visitor visitor){
        directory_check(at,16);const auto count=integer(std::uint64_t{at}+12,2)+integer(std::uint64_t{at}+14,2);
        directory_check(std::uint64_t{at}+16,count*8);
        for(std::uint64_t n=0;n<count;++n){const auto entry=std::uint64_t{at}+16+n*8;
            if(!visitor(Entry{static_cast<std::uint32_t>(integer(entry,4)),static_cast<std::uint32_t>(integer(entry+4,4))}))break;
        }
    }
    bool named_type(std::uint32_t name,std::uint16_t type){
        if(!(name&0x80000000U))return false;
        const auto text="#"+std::to_string(type);const auto at=std::uint64_t{name&0x7fffffffU};
        const auto count=integer(at,2);directory_check(at+2,count*2);if(count!=text.size())return false;
        for(std::size_t n=0;n<text.size();++n)if(integer(at+2+n*2,2)!=static_cast<unsigned char>(text[n]))return false;return true;
    }
    static std::uint32_t child(Entry entry){if(!(entry.target&0x80000000U))invalid();return entry.target&0x7fffffffU;}
    Reader& file_;PeHeader header_;std::array<Section,section_capacity> sections_{};std::size_t count_{};
    std::uint64_t headers_{},image_size_{},resource_{},resource_size_{};
};
}
