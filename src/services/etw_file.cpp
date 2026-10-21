#include "etw_file_internal.hpp"

namespace x64arm::etw {
using namespace detail;
Record parse_record(std::span<const std::uint8_t> bytes,std::uint16_t processor,std::uint16_t logger){
    require(bytes.size()>=4,"ETL 事件標頭截斷");
    if(bytes[3]!=0xc0)throw Error(Failure::Unsupported,"ETL 事件 marker 尚未支援");
    Record record;const auto type=bytes[2];
    switch(type){
        case 0x12:case 0x13:record.kind=Kind::Modern;record.x86=type==0x12;break;
        case 0x0a:case 0x14:record.kind=Kind::Classic;record.x86=type==0x0a;break;
        case 1:case 2:record.kind=Kind::System;record.x86=type==1;break;
        case 3:case 4:record.kind=Kind::Compact;record.x86=type==3;break;
        case 0x10:case 0x11:record.kind=Kind::PerfInfo;record.x86=type==0x10;break;
        default:throw Error(Failure::Unsupported,"ETL 事件標頭種類尚未支援");
    }
    record.processor=processor;record.logger=logger;
    const auto size=static_cast<std::size_t>(get(bytes,system_kind(record.kind)?4:0,2));
    auto header_size=base_size(record.kind);
    if(system_kind(record.kind)){
        record.version=static_cast<std::uint16_t>(get(bytes,0,2));header_size+=extra_size(record.version);
    }
    require(size>=header_size && size<=bytes.size(),"ETL 事件大小無效");bytes=bytes.first(size);
    if(record.kind==Kind::PerfInfo)record.timestamp=get(bytes,8,8);
    else {record.thread=static_cast<std::uint32_t>(get(bytes,8,4));record.process=static_cast<std::uint32_t>(get(bytes,12,4));record.timestamp=get(bytes,16,8);}
    if(system_kind(record.kind)){
        record.descriptor.opcode=bytes[6];record.group=bytes[7];
        if(record.kind==Kind::System)record.processor_time=get(bytes,24,8);
        record.extra_header=range(bytes,base_size(record.kind),header_size);
    }else {
        std::copy_n(bytes.begin()+24,16,record.provider.begin());
        if(record.kind==Kind::Classic){record.descriptor.opcode=bytes[4];record.descriptor.level=bytes[5];record.version=static_cast<std::uint16_t>(get(bytes,6,2));record.processor_time=get(bytes,40,8);}
        else {
            record.flags=static_cast<std::uint16_t>(get(bytes,4,2));record.property=static_cast<std::uint16_t>(get(bytes,6,2));
            auto& descriptor=record.descriptor;descriptor.id=static_cast<std::uint16_t>(get(bytes,40,2));
            descriptor.version=bytes[42];descriptor.channel=bytes[43];descriptor.level=bytes[44];descriptor.opcode=bytes[45];
            descriptor.task=static_cast<std::uint16_t>(get(bytes,46,2));descriptor.keyword=get(bytes,48,8);
            record.processor_time=get(bytes,56,8);std::copy_n(bytes.begin()+64,16,record.activity.begin());
            if(record.flags&1){
                bool more;
                do {
                    require(record.extensions.size()<256,"ETL 延伸項目過多");
                    const auto extent=static_cast<std::size_t>(get(bytes,header_size,2));
                    const auto count=static_cast<std::size_t>(get(bytes,header_size+6,2));
                    require(extent>=8 && extent%8==0 && extent<=bytes.size()-header_size && count<=extent-8,"ETL 延伸項目大小無效");
                    Extension extension;extension.type=static_cast<std::uint16_t>(get(bytes,header_size+2,2));
                    extension.flags=static_cast<std::uint16_t>(get(bytes,header_size+4,2));more=(extension.flags&1)!=0;
                    extension.data=range(bytes,header_size+8,header_size+8+count);record.extensions.push_back(std::move(extension));header_size+=extent;
                }while(more);
            }
        }
    }
    record.data=range(bytes,header_size,size);return record;
}
namespace detail {
Header parse_header(const Record& record){
    require(record.kind==Kind::System && record.group==0 && record.descriptor.opcode==0,"ETL 第一個事件不是 logfile header");
    if((record.version&0xff)>2)throw Error(Failure::Unsupported,"ETL logfile header 版本尚未支援");
    Header header;header.x86=record.x86;header.start_timestamp=record.timestamp;const auto& bytes=record.data;
    const std::size_t base=record.x86?240:248,total=base+32;require(bytes.size()>=total,"ETL logfile header 截斷");
    const auto u32=[&](std::size_t at){return static_cast<std::uint32_t>(get(bytes,at,4));};
    header.buffer_size=u32(0);header.version=u32(4);header.build=u32(8);header.processors=u32(12);header.end_time=get(bytes,16,8);
    header.timer_resolution=u32(24);header.maximum_file_size=u32(28);header.mode=u32(32);header.buffers_written=u32(36);
    header.start_buffers=u32(40);header.pointer_size=u32(44);header.events_lost=u32(48);header.cpu_mhz=u32(52);
    require(header.pointer_size==4 || header.pointer_size==8,"ETL pointer size 無效");
    std::copy_n(bytes.begin()+(record.x86?64:72),172,header.timezone.begin());
    header.boot_time=get(bytes,base,8);header.frequency=get(bytes,base+8,8);header.start_time=get(bytes,base+16,8);
    header.clock=u32(base+24);header.buffers_lost=u32(base+28);
    auto at=total;header.session_name=string(bytes,at,1024);header.file_name=string(bytes,at,32767);
    return header;
}
}
File parse(std::span<const std::uint8_t> bytes,const Limits& limits){
    if(bytes.size()>limits.file_bytes)throw Error(Failure::TooLarge,"ETL 超過檔案容量上限");
    File file;std::size_t offset=0,count=0;
    while(offset<bytes.size()){
        if(file.buffers.size()>=limits.buffers)throw Error(Failure::TooLarge,"ETL buffer 過多");
        const auto tail=bytes.subspan(offset);require(tail.size()>=buffer_header_size,"ETL buffer 標頭截斷");
        Buffer buffer;buffer.size=static_cast<std::uint32_t>(get(tail,0,4));
        require(buffer.size>=1024 && buffer.size%1024==0 && buffer.size<=tail.size(),"ETL buffer 大小或檔案長度無效");
        if(buffer.size>limits.buffer_bytes)throw Error(Failure::TooLarge,"ETL buffer 超過容量上限");
        buffer.saved=static_cast<std::uint32_t>(get(tail,4,4));buffer.current=static_cast<std::uint32_t>(get(tail,8,4));
        buffer.timestamp=get(tail,16,8);buffer.sequence=get(tail,24,8);
        buffer.processor=static_cast<std::uint16_t>(get(tail,40,2));buffer.logger=static_cast<std::uint16_t>(get(tail,42,2));
        buffer.state=static_cast<std::uint32_t>(get(tail,44,4));buffer.filled=static_cast<std::uint32_t>(get(tail,48,4));
        buffer.flags=static_cast<std::uint16_t>(get(tail,52,2));buffer.type=static_cast<std::uint16_t>(get(tail,54,2));
        if(buffer.flags&0x40)throw Error(Failure::Unsupported,"ETL XPRESS 壓縮 buffer 尚未支援");
        require(buffer.filled>=buffer_header_size && buffer.filled<=buffer.size,"ETL buffer 有效範圍無效");
        const auto data=tail.first(buffer.filled);auto at=buffer_header_size;
        while(at<data.size()){
            if(data.size()-at>=4 && get(data,at,4)==UINT32_MAX){
                require(std::all_of(data.begin()+static_cast<std::ptrdiff_t>(at),data.end(),[](auto value){return value==0xff;}),"ETL 結尾標記後仍有資料");break;
            }
            if(count>=limits.records)throw Error(Failure::TooLarge,"ETL 事件數超過上限");
            auto record=parse_record(data.subspan(at),buffer.processor,buffer.logger);
            const auto size=static_cast<std::size_t>(get(data,at+(system_kind(record.kind)?4:0),2));
            const auto aligned=align8(size);require(aligned<=data.size()-at,"ETL 事件對齊截斷");at+=aligned;++count;
            if(file.buffers.empty() && buffer.records.empty())file.header=parse_header(record);
            buffer.records.push_back(std::move(record));
        }
        file.buffers.push_back(std::move(buffer));offset+=file.buffers.back().size;
    }
    require(!file.buffers.empty() && !file.buffers.front().records.empty(),"ETL 缺少 logfile header");
    require(file.header.buffer_size==file.buffers.front().size,"ETL logfile 與第一個 buffer 大小不一致");
    // 未結束的 live 檔案可能仍為零；非零計數超出完整檔案時必須拒絕。
    require(!file.header.buffers_written || file.header.buffers_written<=file.buffers.size(),"ETL 已寫 buffer 數超出檔案範圍");
    return file;
}
std::uint64_t filetime(const Header& header,std::uint64_t raw){
    if(header.clock==2)return raw;
    std::uint64_t denominator=0,numerator=0;
    if(header.clock==1){denominator=header.frequency;numerator=10000000;}
    else if(header.clock==3){denominator=header.cpu_mhz;numerator=10;}
    else throw Error(Failure::Unsupported,"ETL 時鐘種類尚未支援");
    require(denominator!=0,"ETL 時鐘頻率為零");
    const auto delta=static_cast<__int128>(raw)-header.start_timestamp;
    const auto value=static_cast<__int128>(header.start_time)+delta*numerator/denominator;
    require(value>=0 && value<=UINT64_MAX,"ETL 時間轉換溢位");return static_cast<std::uint64_t>(value);
}
}
