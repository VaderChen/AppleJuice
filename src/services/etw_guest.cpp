#include "x64arm/etw_guest.hpp"
#include "etw_file_internal.hpp"
#include <algorithm>

namespace x64arm::etw::guest {
namespace {
void put(Bytes& bytes,std::size_t at,unsigned n,std::uint64_t value){detail::put(bytes,at,n,value);}
void pointer(Bytes& bytes,std::size_t at,unsigned unit,std::uint64_t value){if(unit==4 && value>UINT32_MAX)throw Fault{87};put(bytes,at,unit,value);}
void copy(Bytes& bytes,std::size_t at,std::span<const std::uint8_t> source){detail::copy(bytes,at,source);}
Guid provider(const Record& record){
    if(record.kind==Kind::Modern || record.kind==Kind::Classic)return record.provider;
    // Logfile header 的 MOF class 為 EventTraceGuid；其他 kernel group 另依其 schema 映射。
    if(record.kind==Kind::System && !record.group && !record.descriptor.opcode)return {0x00,0xd9,0xfd,0x68,0x3e,0x4a,0xd1,0x11,0x84,0xf4,0x00,0x00,0xf8,0x04,0x64,0xe3};
    throw Fault{50};
}
void legacy(Bytes& bytes,const Record& record,std::uint64_t timestamp){
    put(bytes,0,2,48);put(bytes,4,1,record.descriptor.opcode);put(bytes,5,1,record.descriptor.level);
    put(bytes,6,2,record.kind==Kind::Modern?record.descriptor.version:record.version);
    put(bytes,8,4,record.thread);put(bytes,12,4,record.process);put(bytes,16,8,timestamp);copy(bytes,24,provider(record));put(bytes,40,8,record.processor_time);
}
}
Descriptor descriptor(const GuestMemory& memory,std::uint64_t address){
    if(!address)throw Fault{87};memory.check(GuestAddress{address},16,Access::Read);Descriptor result;
    result.id=static_cast<std::uint16_t>(memory.load(GuestAddress{address},16));result.version=static_cast<std::uint8_t>(memory.load(GuestAddress{address+2},8));
    result.channel=static_cast<std::uint8_t>(memory.load(GuestAddress{address+3},8));result.level=static_cast<std::uint8_t>(memory.load(GuestAddress{address+4},8));
    result.opcode=static_cast<std::uint8_t>(memory.load(GuestAddress{address+5},8));result.task=static_cast<std::uint16_t>(memory.load(GuestAddress{address+6},16));
    result.keyword=memory.load(GuestAddress{address+8},64);return result;
}
void data(const GuestMemory& memory,Record& record,std::uint32_t count,std::uint64_t descriptors,bool typed){
    if(count>128 || (count && !descriptors))throw Fault{87};if(count)memory.check(GuestAddress{descriptors},std::size_t{count}*16,Access::Read);
    std::size_t total=0;std::vector<Bytes> blocks;blocks.reserve(count);std::vector<unsigned> types;
    for(std::uint32_t i=0;i<count;++i){const auto at=descriptors+std::uint64_t{i}*16;const auto ptr=memory.load(GuestAddress{at},64);
        const auto size=memory.load(GuestAddress{at+8},32);const auto type=typed?static_cast<unsigned>(memory.load(GuestAddress{at+12},8)):0U;
        if(type>2)throw Fault{50};if(size>65535-total)throw Fault{534};total+=static_cast<std::size_t>(size);
        Bytes block(static_cast<std::size_t>(size));if(size){if(!ptr)throw Fault{87};memory.read(GuestAddress{ptr},block);}blocks.push_back(std::move(block));types.push_back(type);
    }
    // 先完成驗證再提交；一般 block 直接串接，不插入宿主對齊或 padding。
    auto pending=record;for(std::size_t i=0;i<blocks.size();++i){if(types[i])pending.extensions.push_back({static_cast<std::uint16_t>(types[i]==1?11:12),0,std::move(blocks[i])});
        else pending.data.insert(pending.data.end(),blocks[i].begin(),blocks[i].end());}
    try{(void)serialize_record(pending);}catch(const Error& error){throw Fault{error.failure==Failure::TooLarge?534U:87U};}record=std::move(pending);
}
Record classic(const GuestMemory& memory,std::uint64_t address,bool x86){
    if(!address)throw Fault{87};memory.check(GuestAddress{address},48,Access::Read);
    const auto size=memory.load(GuestAddress{address},16);const auto flags=memory.load(GuestAddress{address+44},32);
    if(size<48)throw Fault{87};if(!(flags&0x20000U) || (flags&~UINT64_C(0x1a0200)))throw Fault{186};
    memory.check(GuestAddress{address},static_cast<std::size_t>(size),Access::Read);
    Record result;result.kind=Kind::Classic;result.x86=x86;result.descriptor.opcode=static_cast<std::uint8_t>(memory.load(GuestAddress{address+4},8));
    result.descriptor.level=static_cast<std::uint8_t>(memory.load(GuestAddress{address+5},8));result.version=static_cast<std::uint16_t>(memory.load(GuestAddress{address+6},16));
    result.timestamp=memory.load(GuestAddress{address+16},64);
    const auto guid=(flags&0x80000U)?memory.load(GuestAddress{address+24},64):address+24;if(!guid)throw Fault{87};memory.read(GuestAddress{guid},result.provider);
    if(flags&0x100000U){if((size-48)%16 || (size-48)/16>16)throw Fault{87};data(memory,result,static_cast<std::uint32_t>((size-48)/16),address+48,false);}
    else {result.data.resize(static_cast<std::size_t>(size-48));memory.read(GuestAddress{address+48},result.data);}
    return result;
}
Bytes logfile_header(const Header& header,bool x86,std::uint64_t logger_name,std::uint64_t file_name){
    auto adjusted=header;adjusted.x86=x86;auto bytes=detail::header_record(adjusted).data;bytes.resize(x86?272:280);
    pointer(bytes,56,x86?4:8,logger_name);pointer(bytes,x86?60:64,x86?4:8,file_name);return bytes;
}
std::size_t event_size(const Record& record,bool x86,bool modern){
    (void)provider(record);try{(void)serialize_record(record);}catch(const Error& error){throw Fault{error.failure==Failure::TooLarge?534U:87U};}
    std::size_t result=modern?layout(x86).event_record:layout(x86).event_trace;
    if(modern){if(record.extensions.size()>256)throw Fault{534};result+=record.extensions.size()*16;for(const auto& item:record.extensions)result+=detail::align8(item.data.size());}
    if(record.data.size()>65535)throw Fault{534};return result+record.data.size();
}
Bytes event(const Record& record,bool x86,bool modern,std::uint64_t arena,std::uint64_t timestamp,std::uint64_t context){
    const auto shape=layout(x86);Bytes bytes(event_size(record,x86,modern));
    if(!modern){legacy(bytes,record,timestamp);pointer(bytes,72,shape.unit,record.data.empty()?0:arena+shape.event_trace);
        put(bytes,72+shape.unit,4,record.data.size());put(bytes,76+shape.unit,2,record.processor);put(bytes,78+shape.unit,2,record.logger);
        copy(bytes,shape.event_trace,record.data);return bytes;}
    put(bytes,0,2,80);put(bytes,2,2,0);
    const auto flags=record.kind==Kind::Modern?record.flags:static_cast<std::uint16_t>(0x100U|(record.flags&0x12U));put(bytes,4,2,(flags&~0x60U)|(record.x86?0x20U:0x40U)|(record.extensions.empty()?0U:1U));
    put(bytes,6,2,record.property);put(bytes,8,4,record.thread);put(bytes,12,4,record.process);put(bytes,16,8,timestamp);copy(bytes,24,provider(record));
    const auto d=record.kind==Kind::Modern?record.descriptor:Descriptor{0,0,static_cast<std::uint8_t>(record.version),0,record.descriptor.level,record.descriptor.opcode,0};put(bytes,40,2,d.id);put(bytes,42,1,d.version);put(bytes,43,1,d.channel);put(bytes,44,1,d.level);
    put(bytes,45,1,d.opcode);put(bytes,46,2,d.task);put(bytes,48,8,d.keyword);put(bytes,56,8,record.processor_time);copy(bytes,64,record.activity);
    put(bytes,80,2,record.processor);put(bytes,82,2,record.logger);put(bytes,84,2,record.extensions.size());put(bytes,86,2,record.data.size());
    auto at=shape.event_record+record.extensions.size()*16;
    pointer(bytes,88,shape.unit,record.extensions.empty()?0:arena+shape.event_record);
    for(std::size_t i=0;i<record.extensions.size();++i){const auto& item=record.extensions[i];const auto ext=shape.event_record+i*16;
        put(bytes,ext+2,2,item.type);put(bytes,ext+4,2,item.flags&~1U);put(bytes,ext+6,2,item.data.size());put(bytes,ext+8,8,arena+at);
        copy(bytes,at,item.data);at+=detail::align8(item.data.size());}
    pointer(bytes,88+shape.unit,shape.unit,record.data.empty()?0:arena+at);pointer(bytes,88+shape.unit*2,shape.unit,context);copy(bytes,at,record.data);return bytes;
}
}
