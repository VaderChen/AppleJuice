#include "x64arm/minidump.hpp"
#include "x64arm/windows_unwind.hpp"
#include "x64arm/cpu_features.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
struct Writer {
    DumpBytes bytes;
    std::uint32_t reserve(std::size_t count,unsigned alignment=4){
        const auto at=(bytes.size()+alignment-1)&~static_cast<std::size_t>(alignment-1);
        if(at>minidump_maximum_bytes || count>minidump_maximum_bytes-at)throw std::length_error("minidump 超過容量上限");
        bytes.resize(at+count);return static_cast<std::uint32_t>(at);
    }
    void put(std::size_t at,unsigned size,std::uint64_t value){for(unsigned i=0;i<size;++i)bytes.at(at+i)=static_cast<std::uint8_t>(value>>(8*i));}
    std::uint32_t append(std::span<const std::uint8_t> data,unsigned alignment=4){const auto at=reserve(data.size(),alignment);std::copy(data.begin(),data.end(),bytes.begin()+at);return at;}
    std::uint32_t string(std::u16string_view value){const auto data=utf16_bytes(std::u16string(value));const auto at=reserve(4+data.size());put(at,4,data.size()-2);std::copy(data.begin(),data.end(),bytes.begin()+at+4);return at;}
    void location(std::size_t at,std::span<const std::uint8_t> data,unsigned alignment=4){if(data.empty())return;const auto rva=append(data,alignment);put(at,4,data.size());put(at+4,4,rva);}
};
}
DumpBytes minidump_context(const GuestState& state,bool x86){
    GuestMemory scratch;scratch.set_execution_mode(x86?ExecutionMode::X86:ExecutionMode::X64);scratch.map(GuestAddress{0x10000},4096,read_write);
    write_context(scratch,0x10000,state);DumpBytes result(x86?716:1232);scratch.read(GuestAddress{0x10000},result);return result;
}
DumpBytes encode_minidump(const DumpSnapshot& snapshot){
    Writer out;
    struct Stream {std::uint32_t type,size,rva;};std::vector<Stream> streams;
    const auto expected=5U+static_cast<unsigned>(snapshot.user_streams.size())+(snapshot.exception?1U:0U)+((snapshot.flags&0x800)?1U:0U)+((snapshot.flags&0x1000)?1U:0U)+((snapshot.flags&0x400)?0U:1U);
    out.reserve(32+expected*12);out.put(0,4,0x504d444d);out.put(4,4,0xa793);out.put(12,4,32);out.put(20,4,snapshot.timestamp);out.put(24,8,snapshot.flags);
    const auto stream=[&](std::uint32_t type,std::size_t size){const auto at=out.reserve(size);streams.push_back({type,static_cast<std::uint32_t>(size),at});return at;};
    const auto system=stream(7,56);out.put(system,2,snapshot.x86?0:9);out.put(system+2,2,6);out.put(system+6,1,snapshot.processors);out.put(system+7,1,1);
    out.put(system+8,4,10);out.put(system+16,4,19045);out.put(system+20,4,2);const auto csd=out.string(u"");out.put(system+24,4,csd);
    const auto cpuid=virtual_cpuid(0,0,snapshot.processors),features=virtual_cpuid(1,0,snapshot.processors);
    if(snapshot.x86){out.put(system+32,4,cpuid[1]);out.put(system+36,4,cpuid[3]);out.put(system+40,4,cpuid[2]);out.put(system+44,4,features[0]);out.put(system+48,4,features[3]);}
    else {std::uint64_t mask=0;for(unsigned index=0;index<64;++index)if(virtual_processor_feature(index,false))mask|=UINT64_C(1)<<index;out.put(system+32,8,mask);}
    const auto misc=stream(15,24);out.put(misc,4,24);out.put(misc+4,4,1);out.put(misc+8,4,snapshot.pid);
    std::vector<const DumpThread*> threads;for(const auto& thread:snapshot.threads)if(thread.flags&1)threads.push_back(&thread);
    const auto thread_list=stream(3,4+threads.size()*48);out.put(thread_list,4,threads.size());
    for(std::size_t i=0;i<threads.size();++i){const auto& thread=*threads[i];const auto at=thread_list+4+i*48;
        out.put(at,4,thread.id);out.put(at+4,4,thread.suspend);out.put(at+8,4,thread.priority_class);out.put(at+12,4,thread.priority);out.put(at+16,8,thread.teb);
        if((thread.flags&2) && !thread.stack.empty()){out.put(at+24,8,thread.stack_begin);out.location(at+32,thread.stack);}
        if(thread.flags&4)out.location(at+40,thread.context,16);
    }
    std::vector<const DumpModule*> modules;for(const auto& module:snapshot.modules)if(module.flags&1)modules.push_back(&module);
    const auto module_list=stream(4,4+modules.size()*108);out.put(module_list,4,modules.size());
    for(std::size_t i=0;i<modules.size();++i){const auto& module=*modules[i];const auto at=module_list+4+i*108;
        out.put(at,8,module.image.base);out.put(at+8,4,module.image.size);out.put(at+12,4,module.checksum);out.put(at+16,4,module.timestamp);
        auto name=module.path;if(snapshot.flags&0x80)name=name.substr(name.find_last_of(u"\\/")+1);const auto rva=out.string(name);out.put(at+20,4,rva);
        for(unsigned n=0;n<13;++n)out.put(at+24+n*4,4,module.version[n]);if(module.flags&8)out.location(at+76,module.cv);if(module.flags&4)out.location(at+84,module.misc);
    }
    if(snapshot.exception){const auto at=stream(6,168);out.put(at,4,snapshot.exception->first);if(snapshot.exception->second.size()!=152)throw std::invalid_argument("minidump 例外資料大小錯誤");
        std::copy(snapshot.exception->second.begin(),snapshot.exception->second.end(),out.bytes.begin()+at+8);out.location(at+160,snapshot.exception_context,16);
    }
    if(snapshot.flags&0x800){const auto at=stream(16,16+snapshot.information.size()*48);out.put(at,4,16);out.put(at+4,4,48);out.put(at+8,8,snapshot.information.size());
        for(std::size_t i=0;i<snapshot.information.size();++i){const auto& info=snapshot.information[i];const auto item=at+16+i*48;out.put(item,8,info.base);out.put(item+8,8,info.allocation);out.put(item+16,4,info.initial);out.put(item+24,8,info.size);out.put(item+32,4,info.state);out.put(item+36,4,info.protection);out.put(item+40,4,info.type);}
    }
    if(snapshot.flags&0x1000){std::vector<const DumpThread*> information;for(const auto* thread:threads)if(thread->flags&0x40)information.push_back(thread);
        const auto at=stream(17,12+information.size()*64);out.put(at,4,12);out.put(at+4,4,64);out.put(at+8,4,information.size());
        for(std::size_t i=0;i<information.size();++i){const auto& thread=*information[i];const auto item=at+12+i*64;out.put(item,4,thread.id);out.put(item+4,4,8U|(thread.writing?2U:0U));out.put(item+12,4,thread.exit_code);out.put(item+48,8,thread.start);out.put(item+56,8,thread.affinity);}
    }
    if(!(snapshot.flags&0x400)){const auto names=stream(24,4+threads.size()*12);out.put(names,4,threads.size());for(std::size_t i=0;i<threads.size();++i){out.put(names+4+i*12,4,threads[i]->id);const auto rva=out.string(threads[i]->name);out.put(names+8+i*12,8,rva);}}
    for(const auto& [type,data]:snapshot.user_streams){const auto rva=out.append(data);streams.push_back({type,static_cast<std::uint32_t>(data.size()),rva});}
    const auto memory_count=snapshot.memory.size();
    if(snapshot.flags&2){const auto at=stream(9,16+memory_count*16);out.put(at,8,memory_count);
        // FullMemory 的 raw 資料必須連續，而且位於整份 dump 的最後。
        const auto base=out.reserve(0);out.put(at+8,8,base);
        for(std::size_t i=0;i<memory_count;++i){const auto& region=snapshot.memory[i];out.put(at+16+i*16,8,region.base);out.put(at+24+i*16,8,region.bytes.size());out.append(region.bytes,1);}
    }else {const auto at=stream(5,4+memory_count*16);out.put(at,4,memory_count);
        for(std::size_t i=0;i<memory_count;++i){const auto& region=snapshot.memory[i];out.put(at+4+i*16,8,region.base);out.location(at+12+i*16,region.bytes);}
    }
    if(streams.size()>expected)throw std::length_error("minidump 目錄容量錯誤");out.put(8,4,streams.size());
    for(std::size_t i=0;i<streams.size();++i){const auto at=32+i*12;out.put(at,4,streams[i].type);out.put(at+4,4,streams[i].size);out.put(at+8,4,streams[i].rva);}
    return std::move(out.bytes);
}
}
