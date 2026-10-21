#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <limits>

namespace x64arm {
namespace {
using Bytes=std::vector<std::uint8_t>;
struct Error {std::uint32_t code;};
void require(bool value,std::uint32_t code=13){if(!value)throw Error{code};}
std::uint64_t get(std::span<const std::uint8_t> bytes,std::size_t offset,unsigned width){require(offset<=bytes.size() && width<=bytes.size()-offset);std::uint64_t value=0;for(unsigned i=0;i<width;++i)value|=std::uint64_t(bytes[offset+i])<<(8*i);return value;}
void put(Bytes& bytes,std::size_t offset,std::uint64_t value,unsigned width=4){require(offset<=bytes.size() && width<=bytes.size()-offset);for(unsigned i=0;i<width;++i)bytes[offset+i]=static_cast<std::uint8_t>(value>>(8*i));}
struct Certificate {std::size_t offset{},length{},padded{};std::uint16_t type{};};
struct Image {
    std::size_t checksum{},directory{},table{},length{},sections{},debug{};
    unsigned section_count{},debug_count{};
    std::vector<Certificate> certificates;
};
Image parse(std::span<const std::uint8_t> bytes){
    require(bytes.size()>=64 && get(bytes,0,2)==0x5a4d,193);const auto nt=static_cast<std::size_t>(get(bytes,60,4));require(nt<=bytes.size() && bytes.size()-nt>=24 && get(bytes,nt,4)==0x4550,193);
    const auto opt=nt+24,optional_size=static_cast<std::size_t>(get(bytes,nt+20,2));const auto magic=get(bytes,opt,2);require((magic==0x10b || magic==0x20b) && opt<=bytes.size() && optional_size<=bytes.size()-opt,193);const bool small=magic==0x10b;const auto directories=opt+(small?96:112),number=opt+(small?92:108);require(optional_size>=(small?136:152) && get(bytes,number,4)>=5,193);
    Image image;image.checksum=opt+64;image.directory=directories+32;image.table=static_cast<std::size_t>(get(bytes,image.directory,4));image.length=static_cast<std::size_t>(get(bytes,image.directory+4,4));image.sections=opt+optional_size;image.section_count=static_cast<unsigned>(get(bytes,nt+6,2));require(image.section_count<=96 && image.sections<=bytes.size() && image.section_count*40ULL<=bytes.size()-image.sections,193);
    require((image.table==0)==(image.length==0));if(image.table){require(!(image.table&7) && image.table>=get(bytes,opt+60,4) && image.table<=bytes.size() && image.length<=bytes.size()-image.table);for(unsigned i=0;i<image.section_count;++i){const auto section=image.sections+i*40;const auto raw=get(bytes,section+20,4),length=get(bytes,section+16,4);require(!length || raw+length<=image.table || image.table+image.length<=raw);}
        const auto end=image.table+image.length;for(auto at=image.table;at<end;){require(end-at>=8);const auto size=static_cast<std::size_t>(get(bytes,at,4)),padded=(size+7)&~std::size_t{7};require(size>=8 && padded<=end-at && image.certificates.size()<4096);image.certificates.push_back({at,size,padded,static_cast<std::uint16_t>(get(bytes,at+6,2))});at+=padded;}
    }
    if(get(bytes,number,4)>6 && directories+56<=opt+optional_size){const auto rva=get(bytes,directories+48,4),length=get(bytes,directories+52,4);if(rva && length){require(!(length%28));for(unsigned i=0;i<image.section_count;++i){const auto section=image.sections+i*40;const auto va=get(bytes,section+12,4),size=get(bytes,section+16,4);if(rva>=va && rva-va<size && length<=size-(rva-va)){image.debug=static_cast<std::size_t>(get(bytes,section+20,4)+(rva-va));image.debug_count=static_cast<unsigned>(length/28);require(image.debug<=bytes.size() && length<=bytes.size()-image.debug);break;}}}}
    return image;
}
// 插入／移除憑證表時保留其後 overlay，並調整 PE 中其餘檔案 offset。
void relocate(Bytes& bytes,const Image& image,std::size_t begin,std::size_t removed,std::size_t inserted){
    const auto change=[&](std::size_t field){const auto old=get(bytes,field,4);if(!old || old<begin)return;require(old>=begin+removed);const auto next=old-removed+inserted;require(next<=UINT32_MAX);put(bytes,field,next);};
    const auto nt=static_cast<std::size_t>(get(bytes,60,4));change(nt+12);
    for(unsigned i=0;i<image.section_count;++i)for(const auto field:{20U,24U,28U})change(image.sections+i*40+field);
    auto debug=image.debug;if(debug>=begin+removed)debug=debug-removed+inserted;
    for(unsigned i=0;i<image.debug_count;++i)change(debug+i*28+24);
}
void checksum(Bytes& bytes,const Image& image){
    put(bytes,image.checksum,0);std::uint64_t value=0;for(std::size_t at=0;at<bytes.size();at+=2){value+=bytes[at];if(at+1<bytes.size())value+=std::uint64_t(bytes[at+1])<<8;value=(value&0xffff)+(value>>16);}value=(value&0xffff)+(value>>16);put(bytes,image.checksum,(value&0xffff)+bytes.size());
}
}
std::uint32_t WindowsServices::image_certificate_call(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol){
    try {
        const auto found=files_.find(arg(0));require(found!=files_.end(),6);auto& file=found->second;require(!file.directory_loaded && !(file.metadata.attributes&0x10) && !file.pipe && !file.serial && !file.hid,6);
        const bool writing=symbol=="ImageAddCertificate" || symbol=="ImageRemoveCertificate";require((file.access&0x80000001U)!=0,5);if(writing)require(file.native && (file.access&0x40000002U)!=0 && filesystem_.writable(),5);
        Bytes original;if(file.native){if(!runtime_.network.external_io)throw HostCallFault("compare 不允許原生映像檔 I/O");const auto info=file.native->information();require(!info.error,info.error);require(info.metadata.size<=256ULL*1024*1024,8);original.resize(static_cast<std::size_t>(info.metadata.size));const auto io=file.native->read(original,0);require(!io.error,io.error);require(io.bytes==original.size(),38);}else{require(bool(file.bytes),6);original=*file.bytes;}
        const auto image=parse(original);const auto count=static_cast<std::uint32_t>(image.certificates.size());
        if(symbol=="ImageEnumerateCertificates"){
            require(arg(2)!=0,87);memory.check(GuestAddress{arg(2)},4,Access::Write);std::vector<std::uint32_t> indices;const auto filter=static_cast<std::uint16_t>(arg(1));for(std::uint32_t i=0;i<count;++i)if(filter==255 || image.certificates[i].type==filter)indices.push_back(i);const auto capacity=static_cast<std::uint32_t>(arg(4));const auto written=std::min<std::size_t>(capacity,indices.size());if(arg(3))memory.check(GuestAddress{arg(3)},written*4,Access::Write);memory.store(GuestAddress{arg(2)},32,indices.size());if(arg(3))for(std::size_t i=0;i<written;++i)memory.store(GuestAddress{arg(3)+i*4},32,indices[i]);return 1;
        }
        if(symbol=="ImageGetCertificateData" || symbol=="ImageGetCertificateHeader"){
            const auto index=static_cast<std::uint32_t>(arg(1));require(index<count,87);const auto& cert=image.certificates[index];const bool header=symbol=="ImageGetCertificateHeader";
            if(header){require(arg(2)!=0,87);memory.write(GuestAddress{arg(2)},std::span(original).subspan(cert.offset,8));return 1;}
            require(arg(3)!=0,87);const auto capacity=memory.load(GuestAddress{arg(3)},32);memory.check(GuestAddress{arg(3)},4,Access::Write);if(arg(2) && capacity>=cert.length)memory.check(GuestAddress{arg(2)},cert.length,Access::Write);memory.store(GuestAddress{arg(3)},32,cert.length);require(arg(2) && capacity>=cert.length,122);memory.write(GuestAddress{arg(2)},std::span(original).subspan(cert.offset,cert.length));return 1;
        }
        auto bytes=original;std::uint32_t index=0;
        if(symbol=="ImageAddCertificate"){
            require(arg(1) && arg(2),87);memory.check(GuestAddress{arg(2)},4,Access::Write);const auto length=memory.load(GuestAddress{arg(1)},32);require(length>=8 && length<=output_limit,87);Bytes cert(static_cast<std::size_t>((length+7)&~UINT64_C(7)));memory.read(GuestAddress{arg(1)},std::span(cert).first(static_cast<std::size_t>(length)));const auto revision=get(cert,4,2);require(revision==0x100 || revision==0x200,87);index=count;
            const auto at=image.table?image.table+image.length:(bytes.size()+7)&~std::size_t{7};if(!image.table)bytes.resize(at);bytes.insert(bytes.begin()+static_cast<std::ptrdiff_t>(at),cert.begin(),cert.end());relocate(bytes,image,at,0,cert.size());put(bytes,image.directory,image.table?image.table:at);put(bytes,image.directory+4,image.length+cert.size());
        }else if(symbol=="ImageRemoveCertificate"){
            index=static_cast<std::uint32_t>(arg(1));require(index<count,87);const auto& cert=image.certificates[index];bytes.erase(bytes.begin()+static_cast<std::ptrdiff_t>(cert.offset),bytes.begin()+static_cast<std::ptrdiff_t>(cert.offset+cert.padded));relocate(bytes,image,cert.offset,cert.padded,0);const auto remaining=image.length-cert.padded;put(bytes,image.directory,remaining?image.table:0);put(bytes,image.directory+4,remaining);
        }else throw HostCallFault("未實作此 ImageHlp 憑證 API");
        (void)parse(bytes);checksum(bytes,image);const auto access=file.native->validate_io(0,std::max(bytes.size(),original.size()),true);require(!access,access);
        const auto io=file.native->write(bytes,0);auto error=io.error?io.error:io.bytes!=bytes.size()?29U:file.native->truncate(bytes.size());if(error){const auto restore=file.native->write(original,0);if(!restore.error && restore.bytes==original.size())(void)file.native->truncate(original.size());throw Error{error};}
        file.metadata=file.native->information().metadata;if(symbol=="ImageAddCertificate")memory.store(GuestAddress{arg(2)},32,index);return 1;
    }catch(const Error& error){last_error_=error.code;return 0;}catch(const MemoryFault&){last_error_=87;return 0;}catch(const std::bad_alloc&){last_error_=8;return 0;}
}
}
