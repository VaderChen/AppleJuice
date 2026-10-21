#include "x64arm/windows_services.hpp"
#include "crt_lowio_abi.hpp"
#include <algorithm>

namespace x64arm {
void WindowsServices::initialize_crt_lowio(GuestMemory& memory){
    const auto format=crt_lowio::layout(x86_);const auto width=static_cast<std::uint8_t>(format.pointer*8);
    runtime_.lowio_table=runtime_allocate(memory,{},crt_lowio::block_count*format.pointer,format.pointer);
    runtime_.bad_ioinfo=runtime_allocate(memory,{},format.stride,format.pointer);
    memory.store(GuestAddress{runtime_.bad_ioinfo},width,UINT64_MAX);
    memory.store(GuestAddress{runtime_.bad_ioinfo+format.flags},8,crt_lowio::text);
    for(unsigned fd=0;fd<3;++fd)if(auto* file=crt_file(runtime_base_.value+0x800+fd*128))publish_crt_lowio(memory,*file);
}
void WindowsServices::refresh_crt_lowio(GuestMemory& memory){
    if(!runtime_.lowio_table)return;
    for(unsigned fd=0;fd<3;++fd)if(auto* file=crt_file(crt_descriptor_stream(fd))){
        if(import_crt_lowio(memory,*file))publish_crt_lowio(memory,*file);
    }
    runtime_.lowio_standard_dirty=false;
}
void WindowsServices::import_crt_startup(GuestState& state,GuestMemory& memory){
    if(runtime_.startup_reserved2.empty())return;
    const auto decoded=crt_lowio::inherited_descriptors(runtime_.startup_reserved2,runtime_.startup_reserved2_pointer_bytes);
    if(!decoded)return; // 不把非 CRT 的合法保留資料誤解成 descriptor table。
    for(const auto& entry:*decoded){
        if(x86_ && entry.handle>UINT32_MAX)continue;
        CrtFile file;file.text=(entry.flags&crt_lowio::text)!=0;file.append=(entry.flags&crt_lowio::append)!=0;file.lowio_eof=(entry.flags&crt_lowio::eof)!=0;
        if(entry.handle>=0x100 && entry.handle<=0x102 && standard_open_[entry.handle-0x100]){
            file.standard_handle=entry.handle;file.readable=entry.handle==0x100;file.writable=entry.handle!=0x100;
        }else if(const auto known=files_.find(entry.handle);known!=files_.end()){
            const auto& value=known->second;file.native=value.native;file.metadata=value.metadata;file.key=value.key;file.os_handle=entry.handle;file.sharing=value.sharing;
            if(value.bytes)file.bytes=*value.bytes;file.readable=(value.access&0x80000000U)!=0;file.writable=(value.access&0x40000000U)!=0;
        }else continue; // HANDLE 未實際繼承，不建立有效 fd 或回傳假成功。
        const auto installed=crt_install_file(state,memory,std::move(file),entry.descriptor);
        if(!installed)throw HostCallFault("無法安裝繼承的 CRT 描述元");
        auto& stored=runtime_.streams.at(installed);
        if(entry.descriptor<3)runtime_.descriptor_modes[entry.descriptor]=stored.text?0x4000U:0x8000U;
        stored.lowio_read_flags=static_cast<std::uint8_t>(entry.flags&crt_lowio::read_newline);
        publish_crt_lowio(memory,stored);
    }
}
std::uint64_t WindowsServices::crt_lowio_record(GuestMemory& memory,std::uint32_t descriptor,bool create){
    if(descriptor>=crt_lowio::descriptor_limit || !runtime_.lowio_table)return 0;
    const auto format=crt_lowio::layout(x86_);const auto width=static_cast<std::uint8_t>(format.pointer*8);
    const auto block=descriptor/crt_lowio::records_per_block;auto& address=runtime_.lowio_blocks.at(block);
    if(!address && create){
        const auto allocated=allocate_heap(memory,format.stride*crt_lowio::records_per_block);if(!allocated)throw std::bad_alloc{};
        try{for(unsigned i=0;i<crt_lowio::records_per_block;++i){const auto record=allocated+i*format.stride,critical=record+format.critical;
            memory.store(GuestAddress{record},width,UINT64_MAX);
            for(unsigned j=0;j<3;++j)memory.store(GuestAddress{record+format.lookahead+j},8,10);
            memory.store(GuestAddress{record+format.extended},32,4);
            memory.store(GuestAddress{critical+(x86_?4U:8U)},32,UINT32_MAX);
            runtime_.critical_sections.emplace(critical,CriticalSection{});
        }
        memory.store(GuestAddress{runtime_.lowio_table+block*format.pointer},width,allocated);address=allocated;
        }catch(...){for(unsigned i=0;i<crt_lowio::records_per_block;++i)runtime_.critical_sections.erase(allocated+i*format.stride+format.critical);GuestState release;(void)free_heap(memory,allocated,release);throw;}
    }
    return address?address+(descriptor%crt_lowio::records_per_block)*format.stride:0;
}
std::uint64_t WindowsServices::crt_lowio_handle(const CrtFile& file)const{
    return file.lowio_handle_override?file.lowio_handle:file.standard_handle?file.standard_handle:file.os_handle;
}
void WindowsServices::publish_crt_lowio(GuestMemory& memory,CrtFile& file){
    if(!runtime_.lowio_table)return;const auto record=crt_lowio_record(memory,file.descriptor,true);if(!record)return;
    const auto format=crt_lowio::layout(x86_);const auto handle=crt_lowio_handle(file);std::uint8_t kind=0;
    if(handle>=0x100 && handle<=0x102){const auto type=standard_file_type(static_cast<unsigned>(handle-0x100));kind=static_cast<std::uint8_t>(type==2?crt_lowio::tty:type==3?crt_lowio::pipe:0);}
    else if(const auto native=files_.find(handle);native!=files_.end())kind=static_cast<std::uint8_t>(native->second.pipe?crt_lowio::pipe:(native->second.serial || native->second.hid)?crt_lowio::tty:0);
    const auto flags=static_cast<std::uint8_t>(crt_lowio::opened|kind|(file.lowio_eof?crt_lowio::eof:0)|(file.text?crt_lowio::text:0)|(file.append?crt_lowio::append:0)|(file.noinherit?crt_lowio::noinherit:0)|file.lowio_read_flags);
    memory.store(GuestAddress{record},static_cast<std::uint8_t>(format.pointer*8),handle);
    memory.store(GuestAddress{record+format.flags},8,flags);
    memory.store(GuestAddress{record+format.extended},32,file.lowio_extended);
    for(unsigned i=0;i<3;++i)memory.store(GuestAddress{record+format.lookahead+i},8,file.lowio_lookahead[i]);
    file.lowio_handle=handle;file.lowio_flags=flags;file.lowio_published=true;
}
void WindowsServices::close_crt_lowio(GuestMemory& memory,std::uint32_t descriptor){
    const auto record=crt_lowio_record(memory,descriptor,false);if(!record)return;const auto format=crt_lowio::layout(x86_);
    memory.store(GuestAddress{record},static_cast<std::uint8_t>(format.pointer*8),UINT64_MAX);
    memory.store(GuestAddress{record+format.flags},8,0);
    for(unsigned i=0;i<3;++i)memory.store(GuestAddress{record+format.lookahead+i},8,10);
    memory.store(GuestAddress{record+format.extended},32,crt_lowio::critical_initialized);
}
bool WindowsServices::import_crt_lowio(GuestMemory& memory,CrtFile& file){
    if(!file.lowio_published)return true;const auto record=crt_lowio_record(memory,file.descriptor,false);if(!record)return true;
    const auto format=crt_lowio::layout(x86_);const auto flags=static_cast<std::uint8_t>(memory.load(GuestAddress{record+format.flags},8));
    if(!(flags&crt_lowio::opened)){memory.store(GuestAddress{runtime_.errno_cell},32,9);return false;}
    const auto extended=static_cast<std::uint32_t>(memory.load(GuestAddress{record+format.extended},32));
    if(crt_lowio::text_mode(extended)!=crt_lowio::text_mode(file.lowio_extended)){file.lowio_low_surrogate.reset();file.lowio_multibyte_size=0;}
    file.lowio_extended=extended;
    for(unsigned i=0;i<3;++i)file.lowio_lookahead[i]=static_cast<std::uint8_t>(memory.load(GuestAddress{record+format.lookahead+i},8));
    if(flags!=file.lowio_flags){file.text=(flags&crt_lowio::text)!=0;file.append=(flags&crt_lowio::append)!=0;file.lowio_eof=(flags&crt_lowio::eof)!=0;file.noinherit=(flags&crt_lowio::noinherit)!=0;file.lowio_read_flags=static_cast<std::uint8_t>(flags&crt_lowio::read_newline);
        file.lowio_flags=flags;}
    if(file.descriptor<3)runtime_.descriptor_modes[file.descriptor]=crt_lowio::mode_value(file.text,crt_lowio::text_mode(extended));
    const auto handle=memory.load(GuestAddress{record},static_cast<std::uint8_t>(format.pointer*8));
    if(handle!=file.lowio_handle){
        file.lowio_handle_override=true;file.lowio_handle=handle;file.standard_handle=0;file.os_handle=handle;
        file.native.reset();file.bytes.clear();file.buffer.used=file.buffer.cursor=0;
        file.lowio_low_surrogate.reset();file.lowio_needed_input=0;
        if(handle>=0x100 && handle<=0x102){file.standard_handle=handle;file.os_handle=0;file.readable=handle==0x100;file.writable=handle!=0x100;}
        else if(const auto target=files_.find(handle);target!=files_.end()){
            const auto& value=target->second;file.native=value.native;file.metadata=value.metadata;file.key=value.key;
            if(value.bytes)file.bytes=*value.bytes;file.readable=(value.access&0x80000000U)!=0;file.writable=(value.access&0x40000000U)!=0;
        }
    }
    return true;
}
WindowsServices::CrtIoScope::CrtIoScope(WindowsServices& host,GuestState& state,GuestMemory& guest,std::span<const CrtIoLock> locks)
    :services(host),memory(guest),owner(host.threads_.current),operation(host.begin_crt_io_locks(state,guest,locks)){}
WindowsServices::CrtIoScope::CrtIoScope(WindowsServices& host,GuestState& state,GuestMemory& guest,std::uint64_t stream)
    :services(host),memory(guest),owner(host.threads_.current){
    std::array<CrtIoLock,2> locks{};std::size_t count=0;
    if(auto* file=host.crt_file(stream)){
        locks[count++]={file->stream_address,false};
        if(const auto row=host.crt_lowio_record(guest,file->descriptor,false))locks[count++]={row+crt_lowio::layout(host.x86_).critical,true};
    }
    operation=host.begin_crt_io_locks(state,guest,std::span(locks).first(count));
}
WindowsServices::CrtIoScope::~CrtIoScope(){if(operation)services.retire_crt_io_lock_scope(memory,owner,*operation);}
std::optional<std::uint64_t> WindowsServices::begin_crt_io_locks(GuestState& state,GuestMemory& memory,std::span<const CrtIoLock> requested){
    const auto owner=threads_.current;
    auto found=runtime_.crt_io_operations.find(owner);
    CrtIoOperation* operation=nullptr;
    if(found!=runtime_.crt_io_operations.end())for(auto& candidate:found->second)if(candidate.id && candidate.call==state.rip.value && candidate.stack==state.reg(Gpr::Rsp)){operation=&candidate;break;}
    if(requested.empty())return operation?operation->id:0;
    std::vector<CrtIoLock> pending;pending.reserve(requested.size());
    for(const auto lock:requested){
        if(operation && std::find(operation->locks.begin(),operation->locks.end(),lock)!=operation->locks.end())continue;
        if(std::find(pending.begin(),pending.end(),lock)!=pending.end())continue;
        pending.push_back(lock);
        auto& locks=lock.lowio?runtime_.critical_sections:runtime_.file_locks;
        const auto section=locks.find(lock.key);
        if(section!=locks.end() && section->second.owner && section->second.owner!=owner){
            auto& thread=threads_.items.at(owner);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::CrtLockRetry;thread.wait.handles={lock.key};thread.wait.critical=lock.lowio;schedule(state,memory);return {};
        }
        if(section!=locks.end() && section->second.recursion==UINT32_MAX)throw HostCallFault("CRT I/O 鎖遞迴過深");
        if(lock.lowio){if(section==locks.end())throw HostCallFault("CRT lowio critical section 未初始化");memory.check(GuestAddress{lock.key},x86_?24U:40U,Access::Write);}
        else locks.try_emplace(lock.key);
    }
    if(pending.empty())return operation->id;
    if(!operation){
        auto& operations=runtime_.crt_io_operations[owner];
        for(auto& candidate:operations)if(!candidate.id){operation=&candidate;break;}
        if(!operation){operations.emplace_back();operation=&operations.back();}
        if(!runtime_.crt_io_next || runtime_.crt_io_next==UINT64_MAX)throw HostCallFault("CRT I/O operation ID 已耗盡");
        operation->id=runtime_.crt_io_next++;operation->call=state.rip.value;operation->stack=state.reg(Gpr::Rsp);
    }
    const auto id=operation->id;
    try{
        operation->locks.reserve(operation->locks.size()+pending.size());
        for(const auto lock:pending){
            auto& section=(lock.lowio?runtime_.critical_sections:runtime_.file_locks).at(lock.key);
            section.owner=owner;++section.recursion;operation->locks.push_back(lock);
            if(lock.lowio){memory.store(GuestAddress{lock.key+(x86_?4:8)},32,section.recursion-1U);memory.store(GuestAddress{lock.key+(x86_?8:12)},32,section.recursion);memory.store(GuestAddress{lock.key+(x86_?12:16)},x86_?32:64,thread_identifier(owner));}
        }
    }catch(...){retire_crt_io_lock_scope(memory,owner,id,true);throw;}
    return id;
}
void WindowsServices::retire_crt_io_lock_scope(GuestMemory& memory,std::uint64_t owner,std::uint64_t id,bool force)noexcept{
    if(!id)return;
    if(!force)if(const auto thread=threads_.items.find(owner);thread!=threads_.items.end() && thread->second.status==ThreadStatus::Waiting && (thread->second.wait.kind==ThreadWaitKind::StandardInput || thread->second.wait.kind==ThreadWaitKind::CrtFileRetry || thread->second.wait.kind==ThreadWaitKind::CrtLockRetry))return;
    const auto found=runtime_.crt_io_operations.find(owner);if(found==runtime_.crt_io_operations.end())return;
    for(auto& operation:found->second)if(operation.id==id){
        for(auto at=operation.locks.rbegin();at!=operation.locks.rend();++at){
            auto& locks=at->lowio?runtime_.critical_sections:runtime_.file_locks;const auto section=locks.find(at->key);
            if(section==locks.end() || section->second.owner!=owner || !section->second.recursion)continue;
            auto& value=section->second;if(!--value.recursion)value.owner=0;
            if(at->lowio)try{memory.store(GuestAddress{at->key+(x86_?4:8)},32,value.recursion-1U);memory.store(GuestAddress{at->key+(x86_?8:12)},32,value.recursion);memory.store(GuestAddress{at->key+(x86_?12:16)},x86_?32:64,thread_identifier(value.owner));}catch(const MemoryFault&){} // 已解除映射的未定義 guest 用法仍回收 native ownership。
        }
        operation.id=operation.call=operation.stack=0;operation.locks.clear();return;
    }
}
void WindowsServices::discard_crt_io_locks(GuestMemory& memory,std::uint64_t owner)noexcept{
    const auto found=runtime_.crt_io_operations.find(owner);if(found==runtime_.crt_io_operations.end())return;
    for(auto& operation:found->second)if(operation.id)retire_crt_io_lock_scope(memory,owner,operation.id,true);
    runtime_.crt_io_operations.erase(found);
}
}
