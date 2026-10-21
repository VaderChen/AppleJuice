#include "etw_file_internal.hpp"

namespace x64arm::etw {
using namespace detail;
Bytes serialize_record(const Record& record){
    const auto base=base_size(record.kind);std::size_t header_size=base;
    if(system_kind(record.kind)){
        if(record.extra_header.size()!=extra_size(record.version))throw Error(Failure::InvalidParameter,"ETL System 額外 header 大小不符");
        header_size+=record.extra_header.size();
    }else if(!record.extra_header.empty())throw Error(Failure::InvalidParameter,"ETL 非 System 事件不可帶額外 header");
    if(record.kind!=Kind::Modern && !record.extensions.empty())throw Error(Failure::InvalidParameter,"ETL 此事件不支援延伸項目");
    if(record.extensions.size()>256)throw Error(Failure::TooLarge,"ETL 延伸項目過多");
    for(const auto& item:record.extensions){
        if(item.data.size()>65520)throw Error(Failure::TooLarge,"ETL 延伸項目過大");header_size+=align8(8+item.data.size());
    }
    if(header_size>UINT16_MAX || record.data.size()>UINT16_MAX-header_size)throw Error(Failure::TooLarge,"ETL 事件超過 16-bit 大小上限");
    const auto size=header_size+record.data.size();Bytes bytes(align8(size));
    const auto type=[&]()->unsigned {
        switch(record.kind){case Kind::Modern:return record.x86?0x12U:0x13U;case Kind::Classic:return record.x86?0x0aU:0x14U;
            case Kind::System:return record.x86?1U:2U;case Kind::Compact:return record.x86?3U:4U;case Kind::PerfInfo:return record.x86?0x10U:0x11U;}
        throw Error(Failure::InvalidParameter,"ETL 事件種類無效");
    }();
    put(bytes,0,2,system_kind(record.kind)?record.version:size);put(bytes,2,1,type);put(bytes,3,1,0xc0);
    if(record.kind==Kind::PerfInfo)put(bytes,8,8,record.timestamp);
    else {put(bytes,8,4,record.thread);put(bytes,12,4,record.process);put(bytes,16,8,record.timestamp);}
    if(system_kind(record.kind)){
        put(bytes,4,2,size);put(bytes,6,1,record.descriptor.opcode);put(bytes,7,1,record.group);
        if(record.kind==Kind::System)put(bytes,24,8,record.processor_time);copy(bytes,base,record.extra_header);
    }else {
        copy(bytes,24,record.provider);
        if(record.kind==Kind::Classic){put(bytes,4,1,record.descriptor.opcode);put(bytes,5,1,record.descriptor.level);put(bytes,6,2,record.version);put(bytes,40,8,record.processor_time);}
        else {
            put(bytes,4,2,(record.flags&~1U)|(record.extensions.empty()?0U:1U));put(bytes,6,2,record.property);
            const auto& descriptor=record.descriptor;put(bytes,40,2,descriptor.id);put(bytes,42,1,descriptor.version);put(bytes,43,1,descriptor.channel);
            put(bytes,44,1,descriptor.level);put(bytes,45,1,descriptor.opcode);put(bytes,46,2,descriptor.task);put(bytes,48,8,descriptor.keyword);
            put(bytes,56,8,record.processor_time);copy(bytes,64,record.activity);
            auto at=base;
            for(std::size_t i=0;i<record.extensions.size();++i){const auto& item=record.extensions[i];const auto extent=align8(8+item.data.size());
                put(bytes,at,2,extent);put(bytes,at+2,2,item.type);put(bytes,at+4,2,(item.flags&~1U)|(i+1<record.extensions.size()?1U:0U));
                put(bytes,at+6,2,item.data.size());copy(bytes,at+8,item.data);at+=extent;}
        }
    }
    copy(bytes,header_size,record.data);return bytes;
}
namespace detail {
Record header_record(const Header& header){
    Record record;record.kind=Kind::System;record.x86=header.x86;record.version=2;record.timestamp=header.start_timestamp;
    const std::size_t base=header.x86?240:248;auto& bytes=record.data;bytes.resize(base+32);
    put(bytes,0,4,header.buffer_size);put(bytes,4,4,header.version);put(bytes,8,4,header.build);put(bytes,12,4,header.processors);put(bytes,16,8,header.end_time);
    put(bytes,24,4,header.timer_resolution);put(bytes,28,4,header.maximum_file_size);put(bytes,32,4,header.mode);put(bytes,36,4,header.buffers_written);
    put(bytes,40,4,header.start_buffers);put(bytes,44,4,header.pointer_size);put(bytes,48,4,header.events_lost);put(bytes,52,4,header.cpu_mhz);
    copy(bytes,header.x86?64:72,header.timezone);put(bytes,base,8,header.boot_time);put(bytes,base+8,8,header.frequency);put(bytes,base+16,8,header.start_time);
    put(bytes,base+24,4,header.clock);put(bytes,base+28,4,header.buffers_lost);append_string(bytes,header.session_name);append_string(bytes,header.file_name);return record;
}
}
Bytes serialize(const Header& input,std::span<const Record> records,const Limits& limits){
    const auto buffer_size=static_cast<std::size_t>(input.buffer_size);
    if(buffer_size<1024 || buffer_size%1024 || buffer_size>limits.buffer_bytes)throw Error(Failure::InvalidParameter,"ETL buffer 大小無效");
    if(input.pointer_size!=4 && input.pointer_size!=8)throw Error(Failure::InvalidParameter,"ETL pointer size 無效");
    const auto valid_name=[](const auto& name,std::size_t maximum){return name.size()<=maximum && name.find(char16_t{})==name.npos;};
    if(!valid_name(input.session_name,1024) || !valid_name(input.file_name,32767))throw Error(Failure::InvalidParameter,"ETL 名稱長度或 NUL 無效");
    if(records.size()>=limits.records)throw Error(Failure::TooLarge,"ETL 事件數超過上限");
    Header header=input;header.buffers_written=0;const auto first=serialize_record(header_record(header));
    if(first.size()>buffer_size-buffer_header_size)throw Error(Failure::TooLarge,"ETL buffer 無法容納 logfile header");
    Bytes bytes;std::size_t count=0,begin=0,at=0;std::uint16_t processor=0,logger=0;
    const auto finish=[&]{if(!count)return;put(bytes,begin+4,4,at);put(bytes,begin+8,4,at);put(bytes,begin+48,4,at);};
    const auto next=[&](std::uint16_t type,std::uint16_t cpu,std::uint16_t id){
        finish();if(count>=limits.buffers || buffer_size>limits.file_bytes || bytes.size()>limits.file_bytes-buffer_size)throw Error(Failure::TooLarge,"ETL buffer 或檔案容量耗盡");
        begin=bytes.size();bytes.resize(begin+buffer_size,0xff);std::fill_n(bytes.begin()+static_cast<std::ptrdiff_t>(begin),buffer_header_size,0);
        at=buffer_header_size;processor=cpu;logger=id;put(bytes,begin,4,buffer_size);put(bytes,begin+24,8,count++);
        put(bytes,begin+40,2,cpu);put(bytes,begin+42,2,id);put(bytes,begin+44,4,3);put(bytes,begin+54,2,type);
    };
    next(4,0,0);copy(bytes,begin+at,first);at+=first.size();
    for(const auto& record:records){const auto data=serialize_record(record);
        if(data.size()>buffer_size-buffer_header_size)throw Error(Failure::TooLarge,"ETL buffer 無法容納事件");
        if(count==1 || processor!=record.processor || logger!=record.logger || data.size()>buffer_size-at)next(0,record.processor,record.logger);
        copy(bytes,begin+at,data);at+=data.size();put(bytes,begin+16,8,record.timestamp);
    }
    finish();header.buffers_written=static_cast<std::uint32_t>(count);
    const auto completed=serialize_record(header_record(header));copy(bytes,buffer_header_size,completed);return bytes;
}
}
