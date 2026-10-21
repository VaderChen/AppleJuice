#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
namespace x64arm {
struct OleStreamState {
    struct Storage {
        std::uint64_t global{};bool release{},shell_memory{};
        std::shared_ptr<HostFile> file;
        std::u16string path;
        std::uint32_t mode{};
    };
    struct Stream {std::shared_ptr<Storage> storage;std::uint64_t position{};unsigned references{1};bool lock_bytes{};};
    std::map<std::uint64_t,Stream> streams;std::uint64_t table{},lock_table{};
};
std::span<const WindowsServiceDescriptor> shell_memory_stream_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"shlwapi.dll","SHCreateMemStream",2},{"shcore.dll","SHCreateMemStream",2},
        {"shlwapi.dll","#12",2}
    };return table;
}
std::span<const WindowsServiceDescriptor> ole_stream_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ole32.dll","CreateStreamOnHGlobal",3},{"ole32.dll","GetHGlobalFromStream",2},
        {"ole32.dll","CreateILockBytesOnHGlobal",3},{"ole32.dll","GetHGlobalFromILockBytes",2},
        {"ole32.dll","ReadClassStm",2},{"ole32.dll","WriteClassStm",2},
        {"ole32.dll","OleSaveToStream",2},{"ole32.dll","OleLoadFromStream",3},
        {"internal-ole-stream","QueryInterface",3},{"internal-ole-stream","AddRef",1},{"internal-ole-stream","Release",1},
        {"internal-ole-stream","Read",4},{"internal-ole-stream","Write",4},{"internal-ole-stream","Seek",4,2},{"internal-ole-stream","SetSize",2,2},
        {"internal-ole-stream","CopyTo",5,4},{"internal-ole-stream","Commit",2},{"internal-ole-stream","Revert",1},
        {"internal-ole-stream","LockRegion",4,6},{"internal-ole-stream","UnlockRegion",4,6},{"internal-ole-stream","Stat",3},{"internal-ole-stream","Clone",2},
        {"internal-ole-lockbytes","QueryInterface",3},{"internal-ole-lockbytes","AddRef",1},{"internal-ole-lockbytes","Release",1},
        {"internal-ole-lockbytes","ReadAt",5,2},{"internal-ole-lockbytes","WriteAt",5,2},{"internal-ole-lockbytes","Flush",1},
        {"internal-ole-lockbytes","SetSize",2,2},{"internal-ole-lockbytes","LockRegion",4,6},{"internal-ole-lockbytes","UnlockRegion",4,6},{"internal-ole-lockbytes","Stat",3}
    };return table;
}
bool WindowsServices::same_ole_streams(const WindowsServices& other)const{
    if(!ole_streams_ || !other.ole_streams_)return ole_streams_==other.ole_streams_;
    const auto& left=*ole_streams_;const auto& right=*other.ole_streams_;
    if(left.table!=right.table || left.lock_table!=right.lock_table || left.streams.size()!=right.streams.size())return false;
    std::map<const OleStreamState::Storage*,std::uint64_t> left_aliases,right_aliases;
    for(const auto& [handle,stream]:left.streams){
        const auto found=right.streams.find(handle);if(found==right.streams.end())return false;const auto& candidate=found->second;
        if(stream.position!=candidate.position || stream.references!=candidate.references || stream.lock_bytes!=candidate.lock_bytes ||
           stream.storage->global!=candidate.storage->global || stream.storage->release!=candidate.storage->release || stream.storage->shell_memory!=candidate.storage->shell_memory ||
           stream.storage->file!=candidate.storage->file || stream.storage->path!=candidate.storage->path || stream.storage->mode!=candidate.storage->mode)return false;
        const auto alias=left_aliases.emplace(stream.storage.get(),handle).first->second;
        if(alias!=right_aliases.emplace(candidate.storage.get(),handle).first->second)return false;
    }
    return true;
}
std::optional<std::uint32_t> WindowsServices::resume_ole_stream_copy(GuestState& state,GuestMemory& memory,std::uint64_t scratch,std::uint32_t status){
    const auto count=memory.load(GuestAddress{scratch},64),output=memory.load(GuestAddress{scratch+8},64),written=memory.load(GuestAddress{scratch+16},32);
    if(output)memory.store(GuestAddress{output},64,written);if(!(status&0x80000000U) && written!=count)status=written>count?0x8000ffffU:0x80030070U;
    (void)free_heap(memory,scratch,state);complete_call(state,memory,status);return std::nullopt;
}
void WindowsServices::detach_ole_streams(){
    if(!ole_streams_)ole_streams_=std::make_shared<OleStreamState>();
    // services 的值複本必須隔離 COM 狀態，但同一複本內的 IStream Clone 仍共用儲存。
    if(ole_streams_.use_count()>1){
        auto replacement=std::make_shared<OleStreamState>(*ole_streams_);
        std::map<const OleStreamState::Storage*,std::shared_ptr<OleStreamState::Storage>> aliases;
        for(auto& [handle,stream]:replacement->streams){
            const auto previous=stream.storage.get();auto found=aliases.find(previous);
            if(found==aliases.end())found=aliases.emplace(previous,std::make_shared<OleStreamState::Storage>(*stream.storage)).first;
            stream.storage=found->second;
        }
        ole_streams_=std::move(replacement);
    }
}
std::uint32_t WindowsServices::open_shell_file_stream(GuestMemory& memory,std::u16string path,std::uint32_t mode,std::uint32_t attributes,bool create,std::uint64_t& output){
    output=0;constexpr std::uint32_t invalid=0x800300ffU;
    // 直接 IStream 支援 STGM 存取、分享、建立與刪除；交易／priority／simple 需要另一個儲存契約。
    if((mode&~0x04001073U) || (mode&3U)>2 || (mode&0x70U)>0x40U || ((mode&0x70U) && (mode&0x70U)<0x10U))return invalid;
    const auto access=mode&3U;const std::uint32_t share=(mode&0x70U)==0x10U?0U:(mode&0x70U)==0x20U?1U:(mode&0x70U)==0x30U?2U:3U;
    const auto disposition=(mode&0x1000U)?2U:create?1U:3U;
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不能建立無法重播的原生檔案 IStream");
    if(!filesystem_.writable() && (access || disposition!=3 || (mode&0x04000000U)))return 0x80030005U;
    if(attributes&~0x840031a7U)return invalid;
    auto opened=filesystem_.writable()?filesystem_.open_host(utf16_to_utf8(path),(access==0?0x80000000U:access==1?0x40000000U:0xc0000000U)|(disposition!=3?0x40000000U:0U),disposition,attributes|((mode&0x04000000U)?0x04000000U:0),share):filesystem_.open_reader(utf16_to_utf8(path),{}, {},share);
    if(opened.error)return 0x80070000U|((disposition==1 && opened.error==183)?80U:(opened.error&65535U));
    if(!opened.existed && (attributes&0x31a7U))if(const auto error=opened.file->set_basic(0,0,0,0,attributes&0x31a7U))return 0x80070000U|(error&65535U);
    detach_ole_streams();
    auto& bridge=*ole_streams_;if(bridge.streams.size()>=65536)return 0x8007000eU;
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(!bridge.table){const auto table=runtime_allocate(memory,{},14*unit);unsigned i=0;for(const auto& entry:ole_stream_service_descriptors())if(entry.family=="internal-ole-stream")memory.store(GuestAddress{table+unit*i++},bits,resolve(entry.family,entry.symbol).value);bridge.table=table;}
    OleStreamState::Stream stream;stream.storage=std::make_shared<OleStreamState::Storage>();stream.storage->file=std::move(opened.file);stream.storage->path=std::move(path);stream.storage->mode=mode;
    const auto object=allocate_heap(memory,unit);if(!object)return 0x8007000eU;
    try{memory.store(GuestAddress{object},bits,bridge.table);bridge.streams.emplace(object,std::move(stream));}catch(...){(void)free_heap(memory,object,GuestState{});throw;}output=object;return 0;
}
std::optional<std::uint32_t> WindowsServices::invoke_ole_stream(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol=="ReadClassStm" || service.symbol=="WriteClassStm" || service.symbol=="OleSaveToStream" || service.symbol=="OleLoadFromStream")
        return invoke_ole_persistence(state,memory,service);
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const bool shell_memory=(service.family=="shlwapi.dll" || service.family=="shcore.dll") && (name=="SHCreateMemStream" || name=="#12");
    constexpr unsigned invalid=0x80070057U,pointer_error=0x80030009U,out_of_memory=0x8007000eU,medium_full=0x80030070U;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    try{
    detach_ole_streams();
    auto& bridge=*ole_streams_;
    const auto create=[&](OleStreamState::Stream stream){
        if(bridge.streams.size()>=65536)return UINT64_C(0);
        auto& cache=stream.lock_bytes?bridge.lock_table:bridge.table;
        if(!cache){std::vector<WindowsServiceDescriptor> descriptors;const auto family=stream.lock_bytes?"internal-ole-lockbytes":"internal-ole-stream";
            for(const auto& entry:ole_stream_service_descriptors())if(entry.family==family)descriptors.push_back(entry);
            const auto table=runtime_allocate(memory,{},descriptors.size()*unit);for(std::size_t i=0;i<descriptors.size();++i)memory.store(GuestAddress{table+i*unit},bits,resolve(descriptors[i].family,descriptors[i].symbol).value);cache=table;}
        const auto address=allocate_heap(memory,unit);if(!address)return UINT64_C(0);try{memory.store(GuestAddress{address},bits,cache);bridge.streams.emplace(address,std::move(stream));}catch(...){(void)free_heap(memory,address,state);throw;}return address;
    };
    const auto resize=[&](OleStreamState::Storage& storage,std::uint64_t size)->unsigned{
        const auto found=gui_.global_memory.find(storage.global);if(found==gui_.global_memory.end())return 0x80030006U;auto& block=found->second;if(block.clipboard_owned)return 0x80030005U;if(size>output_limit || size>memory.limit())return medium_full;if(size==block.size)return 0;
        if(!size && (block.flags&2)){if(block.locks)return 0x80030021U;if(block.address)(void)free_heap(memory,block.address,state);block.address=0;block.size=0;return 0;}
        const auto allocation=allocations_.find(block.address);const auto capacity=allocation==allocations_.end()?0:allocation->second;
        if(size>capacity){if(block.locks)return 0x80030021U;const auto address=allocate_heap(memory,size);if(!address)return out_of_memory;try{std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));const auto keep=static_cast<std::size_t>(std::min(size,block.size));if(keep)memory.read(GuestAddress{block.address},std::span(bytes).first(keep));memory.write(GuestAddress{address},bytes);auto replacement=block;replacement.address=address;replacement.size=size;
                if(!(block.flags&2)){gui_.global_memory.emplace(address,replacement);const auto previous=storage.global;storage.global=address;if(block.address)(void)free_heap(memory,block.address,state);gui_.global_memory.erase(previous);}else{if(block.address)(void)free_heap(memory,block.address,state);block=replacement;}
            }catch(...){(void)free_heap(memory,address,state);throw;}
        }else {if(size>block.size)memory.write(GuestAddress{block.address+block.size},std::vector<std::uint8_t>(static_cast<std::size_t>(size-block.size)));block.size=size;}
        return 0;
    };
        if(shell_memory){
            // 複製初始 bytes，不借用呼叫者的 buffer；公開介面不暴露 HGLOBAL。
            const auto count=d(1);if((!arg(0) && count) || count>output_limit || gui_.global_memory.size()>=65536)return finish(0);
            std::vector<std::uint8_t> initial(count);if(count)memory.read(GuestAddress{arg(0)},initial);
            const auto handle=gui_.next_object++;std::uint64_t address=0;bool inserted=false;
            try{
                if(count){address=allocate_heap(memory,count);if(!address)return finish(0);memory.write(GuestAddress{address},initial);}
                gui_.global_memory.emplace(handle,GuiGlobalMemory{address,count,2,0,false});inserted=true;
                OleStreamState::Stream stream;stream.storage=std::make_shared<OleStreamState::Storage>(OleStreamState::Storage{handle,true,true,{}, {},0});
                const auto object=create(std::move(stream));if(object)return finish(object);
            }catch(...){if(inserted)gui_.global_memory.erase(handle);if(address)(void)free_heap(memory,address,state);throw;}
            gui_.global_memory.erase(handle);if(address)(void)free_heap(memory,address,state);return finish(0);
        }
        if(name=="CreateStreamOnHGlobal" || name=="CreateILockBytesOnHGlobal"){
            if(!arg(2))return finish(invalid);memory.store(GuestAddress{arg(2)},bits,0);auto handle=arg(0);bool allocated=false;
            if(handle){const auto found=gui_.global_memory.find(handle);if(found==gui_.global_memory.end() || (found->second.flags&0x100) || found->second.clipboard_owned)return finish(invalid);if(found->second.size>output_limit)return finish(out_of_memory);}
            else {if(gui_.global_memory.size()>=65536)return finish(out_of_memory);handle=gui_.next_object++;gui_.global_memory.emplace(handle,GuiGlobalMemory{0,0,2,0,false});allocated=true;}
            try{OleStreamState::Stream stream;stream.storage=std::make_shared<OleStreamState::Storage>(OleStreamState::Storage{handle,d(1)!=0,false,{}, {},0});stream.lock_bytes=name=="CreateILockBytesOnHGlobal";const auto object=create(std::move(stream));if(!object){if(allocated)gui_.global_memory.erase(handle);return finish(out_of_memory);}memory.store(GuestAddress{arg(2)},bits,object);return finish(0);}catch(...){if(allocated)gui_.global_memory.erase(handle);throw;}
        }
        const bool get_global=name=="GetHGlobalFromStream" || name=="GetHGlobalFromILockBytes";
        if(get_global){if(!arg(1))return finish(invalid);memory.store(GuestAddress{arg(1)},bits,0);}
        const auto found=bridge.streams.find(arg(0));if(found==bridge.streams.end())return finish(invalid);auto& stream=found->second;
        const bool lock_bytes=service.family=="internal-ole-lockbytes" || name=="GetHGlobalFromILockBytes";
        if(lock_bytes!=stream.lock_bytes)return finish(invalid);
        if(get_global){if(stream.storage->shell_memory || stream.storage->file)return finish(invalid);memory.store(GuestAddress{arg(1)},bits,stream.storage->global);return finish(0);}
        if(name=="QueryInterface"){
            if(!arg(2))return finish(0x80004003U);std::array<std::uint8_t,16> iid{};if(arg(1))memory.read(GuestAddress{arg(1)},iid);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return finish(0x80004003U);
            constexpr std::array<std::uint8_t,16> unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},istream{12,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46},sequential{0x30,0x3a,0x73,0x0c,0x1c,0x2a,0xce,0x11,0xad,0xe5,0,0xaa,0,0x44,0x77,0x3d};
            constexpr std::array<std::uint8_t,16> ilockbytes{10,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
            if(iid!=unknown && (lock_bytes?iid!=ilockbytes:iid!=istream && iid!=sequential))return finish(0x80004002U);++stream.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);
        }
        if(name=="AddRef")return finish(++stream.references);
        if(name=="Release"){
            if(--stream.references)return finish(stream.references);auto storage=stream.storage;bridge.streams.erase(found);(void)free_heap(memory,arg(0),state);
            if(storage.use_count()==1 && storage->release){const auto block=gui_.global_memory.find(storage->global);if(block!=gui_.global_memory.end()){if(block->second.address)(void)free_heap(memory,block->second.address,state);gui_.global_memory.erase(block);}}return finish(0);
        }
        if(stream.storage->file){
            if(!runtime_.network.external_io)throw HostCallFault("比較模式不能操作無法重播的原生檔案 IStream");
            auto& storage=*stream.storage;const auto info=storage.file->information();if(info.error)return finish(0x80070000U|info.error);const auto size=info.metadata.size;
            const auto io_error=[](std::uint32_t error){return error?0x80070000U|(error&65535U):0;};
            if(name=="Read" || name=="Write"){
                if(arg(3))memory.store(GuestAddress{arg(3)},32,0);const auto count=d(2);const bool writing=name=="Write";
                if((writing && !(storage.mode&3U)) || (!writing && (storage.mode&3U)==1))return finish(0x80030005U);
                if(count && !arg(1))return finish(pointer_error);if(!count)return finish(0);if(count>output_limit)return finish(out_of_memory);
                memory.check(GuestAddress{arg(1)},count,writing?Access::Read:Access::Write);std::vector<std::uint8_t> bytes(count);
                if(writing)memory.read(GuestAddress{arg(1)},bytes);const auto result=writing?storage.file->write(bytes,stream.position):storage.file->read(bytes,stream.position);
                if(!writing && result.bytes)memory.write(GuestAddress{arg(1)},std::span(bytes).first(static_cast<std::size_t>(result.bytes)));stream.position+=result.bytes;
                if(arg(3))memory.store(GuestAddress{arg(3)},32,result.bytes);return finish(result.error?io_error(result.error):writing && result.bytes<count?medium_full:result.bytes==count?0:1);
            }
            if(name=="Seek"){
                if(d(2)>2)return finish(0x80030001U);const auto base=d(2)==0?0:d(2)==1?stream.position:size;const auto signed_value=std::bit_cast<std::int64_t>(arg(1));std::uint64_t position{};
                if(signed_value<0){const auto amount=UINT64_C(0)-arg(1);if(amount>base)return finish(0x80030001U);position=base-amount;}else {if(arg(1)>UINT64_MAX-base)return finish(0x80030001U);position=base+arg(1);}
                if(position>INT64_MAX)return finish(0x80030001U);if(arg(3))memory.store(GuestAddress{arg(3)},64,position);stream.position=position;return finish(0);
            }
            if(name=="SetSize"){if(!(storage.mode&3U))return finish(0x80030005U);return finish(io_error(storage.file->truncate(arg(1))));}
            if(name=="Commit"){if(d(1)&~15U)return finish(invalid);return finish(io_error(storage.file->flush()));}
            if(name=="Revert")return finish(0x80030001U);
            if(name=="Clone"){if(!arg(1))return finish(pointer_error);memory.store(GuestAddress{arg(1)},bits,0);auto clone=stream;clone.references=1;const auto object=create(std::move(clone));if(!object)return finish(out_of_memory);memory.store(GuestAddress{arg(1)},bits,object);return finish(0);}
            if(name=="LockRegion" || name=="UnlockRegion"){
                if(d(3)!=1 && d(3)!=2)return finish(0x80030001U);return finish(io_error(storage.file->lock_range(arg(1),arg(2),d(3)==2,name=="UnlockRegion")));
            }
            if(name=="Stat"){
                if(!arg(1))return finish(pointer_error);if(d(2)>2)return finish(invalid);memory.check(GuestAddress{arg(1)},x86_?72:80,Access::Write);
                std::uint64_t name_pointer{};if(d(2)!=1){const auto bytes=utf16_bytes(storage.path);name_pointer=allocate_heap(memory,bytes.size());if(!name_pointer)return finish(out_of_memory);memory.write(GuestAddress{name_pointer},bytes);gui_.task_allocations.insert(name_pointer);}
                memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(x86_?72:80));memory.store(GuestAddress{arg(1)},bits,name_pointer);memory.store(GuestAddress{arg(1)+unit},32,2);
                const auto base=arg(1)+(x86_?8:16);memory.store(GuestAddress{base},64,size);memory.store(GuestAddress{base+8},64,info.metadata.modified);memory.store(GuestAddress{base+16},64,info.metadata.created);memory.store(GuestAddress{base+24},64,info.metadata.accessed);
                memory.store(GuestAddress{base+32},32,storage.mode);memory.store(GuestAddress{base+36},32,3);return finish(0);
            }
            if(name=="CopyTo"){
                for(unsigned i=3;i<5;++i)if(arg(i))memory.store(GuestAddress{arg(i)},64,0);if(!arg(1))return finish(pointer_error);if((storage.mode&3U)==1)return finish(0x80030005U);
                const auto count=stream.position<size?std::min(arg(2),size-stream.position):0;if(!count)return finish(0);if(count>output_limit)return finish(out_of_memory);
                const auto table=memory.load(GuestAddress{arg(1)},bits),write=memory.load(GuestAddress{table+4*unit},bits);if(!handles(GuestAddress{write}))memory.check(GuestAddress{write},1,Access::Execute);
                const auto scratch=allocate_heap(memory,count+32);if(!scratch)return finish(out_of_memory);
                try{std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));const auto result=storage.file->read(bytes,stream.position);if(result.error){(void)free_heap(memory,scratch,state);return finish(io_error(result.error));}
                    memory.store(GuestAddress{scratch},64,result.bytes);memory.store(GuestAddress{scratch+8},64,arg(4));memory.store(GuestAddress{scratch+16},32,0);memory.write(GuestAddress{scratch+32},std::span(bytes).first(static_cast<std::size_t>(result.bytes)));
                    stream.position+=result.bytes;if(arg(3))memory.store(GuestAddress{arg(3)},64,result.bytes);start_callbacks(state,memory,{{write,{arg(1),scratch+32,result.bytes,scratch+16}}},Continuation::OleStreamCopy,scratch);return std::nullopt;
                }catch(...){(void)free_heap(memory,scratch,state);throw;}
            }
            return finish(0x80030001U);
        }
        const auto global=gui_.global_memory.find(stream.storage->global);if(global==gui_.global_memory.end())return finish(0x80030006U);const auto size=global->second.size;
        if(name=="ReadAt" || name=="WriteAt"){
            const auto offset=arg(1);const auto count=d(3);if(arg(4))memory.store(GuestAddress{arg(4)},32,0);
            if(count && !arg(2))return finish(pointer_error);if(!count)return finish(0);
            if(name=="ReadAt"){
                const auto bytes=offset<size?std::min<std::uint64_t>(count,size-offset):0;
                if(bytes){std::vector<std::uint8_t> data(static_cast<std::size_t>(bytes));memory.read(GuestAddress{global->second.address+offset},data);memory.write(GuestAddress{arg(2)},data);}
                if(arg(4))memory.store(GuestAddress{arg(4)},32,bytes);return finish(0);
            }
            if(offset>output_limit || count>output_limit-offset)return finish(medium_full);
            std::vector<std::uint8_t> data(count);memory.read(GuestAddress{arg(2)},data);
            if(const auto error=resize(*stream.storage,std::max(size,offset+count)))return finish(error);
            memory.write(GuestAddress{gui_.global_memory.at(stream.storage->global).address+offset},data);
            if(arg(4))memory.store(GuestAddress{arg(4)},32,count);return finish(0);
        }
        if(name=="Read" || name=="Write"){
            const auto count=d(2);if(arg(3))memory.store(GuestAddress{arg(3)},32,0);if(count && !arg(1))return finish(pointer_error);if(!count)return finish(0);
            if(name=="Read"){const auto bytes=stream.position<size?std::min<std::uint64_t>(count,size-stream.position):0;if(bytes){std::vector<std::uint8_t> buffer(static_cast<std::size_t>(bytes));memory.read(GuestAddress{global->second.address+stream.position},buffer);memory.write(GuestAddress{arg(1)},buffer);}stream.position+=bytes;if(arg(3))memory.store(GuestAddress{arg(3)},32,bytes);return finish(bytes==count?0:1);}
            if(stream.position>output_limit || count>output_limit-stream.position)return finish(medium_full);std::vector<std::uint8_t> buffer(count);memory.read(GuestAddress{arg(1)},buffer);if(const auto error=resize(*stream.storage,std::max(size,stream.position+count)))return finish(error);
            memory.write(GuestAddress{gui_.global_memory.at(stream.storage->global).address+stream.position},buffer);stream.position+=count;if(arg(3))memory.store(GuestAddress{arg(3)},32,count);return finish(0);
        }
        if(name=="Seek"){
            if(d(2)>2)return finish(0x80030001U);const auto base=d(2)==0?0:d(2)==1?stream.position:size;const auto distance=std::bit_cast<std::int64_t>(arg(1));std::uint64_t position=0;
            if(distance<0){const auto magnitude=UINT64_C(0)-arg(1);if(magnitude>base)return finish(0x80030001U);position=base-magnitude;}else {if(arg(1)>UINT64_MAX-base)return finish(0x80030001U);position=base+arg(1);}if(arg(3))memory.store(GuestAddress{arg(3)},64,position);stream.position=position;return finish(0);
        }
        if(name=="SetSize")return finish(resize(*stream.storage,arg(1)));
        if(name=="Stat"){
            if(!arg(1))return finish(pointer_error);if(d(2)>2)return finish(invalid);std::vector<std::uint8_t> bytes(x86_?72:80);memory.write(GuestAddress{arg(1)},bytes);memory.store(GuestAddress{arg(1)+unit},32,lock_bytes?3:2);memory.store(GuestAddress{arg(1)+(x86_?8:16)},64,size);memory.store(GuestAddress{arg(1)+(x86_?40:48)},32,2);return finish(0);
        }
        if(name=="Clone"){if(!arg(1))return finish(pointer_error);memory.store(GuestAddress{arg(1)},bits,0);auto cloned=stream;cloned.references=1;const auto address=create(std::move(cloned));if(!address)return finish(out_of_memory);memory.store(GuestAddress{arg(1)},bits,address);return finish(0);}
        if(name=="LockRegion" || name=="UnlockRegion")return finish(0x80030001U);
        if(name=="Commit" || name=="Revert" || name=="Flush")return finish(0); // 直接模式，資料已即時寫入共用 HGLOBAL。
        if(name=="CopyTo"){
            if(!arg(1))return finish(pointer_error);for(unsigned i=3;i<5;++i)if(arg(i))memory.store(GuestAddress{arg(i)},64,0);const auto count=stream.position<size?std::min(arg(2),size-stream.position):0;if(!count)return finish(0);
            const auto table=memory.load(GuestAddress{arg(1)},bits),write=memory.load(GuestAddress{table+4*unit},bits);if(!handles(GuestAddress{write}))memory.check(GuestAddress{write},1,Access::Execute);
            const auto scratch=allocate_heap(memory,count+32);if(!scratch)return finish(out_of_memory);try{memory.store(GuestAddress{scratch},64,count);memory.store(GuestAddress{scratch+8},64,arg(4));memory.store(GuestAddress{scratch+16},32,0);std::vector<std::uint8_t> data(static_cast<std::size_t>(count));memory.read(GuestAddress{global->second.address+stream.position},data);memory.write(GuestAddress{scratch+32},data);stream.position+=count;if(arg(3))memory.store(GuestAddress{arg(3)},64,count);start_callbacks(state,memory,{{write,{arg(1),scratch+32,count,scratch+16}}},Continuation::OleStreamCopy,scratch);return std::nullopt;}catch(...){(void)free_heap(memory,scratch,state);throw;}
        }
        return finish(0x80004001U);
    }catch(const std::bad_alloc&){return finish(shell_memory?0:out_of_memory);}catch(const std::length_error&){return finish(shell_memory?0:out_of_memory);}
}
#include "ole_persistence_handlers.inc"
}
