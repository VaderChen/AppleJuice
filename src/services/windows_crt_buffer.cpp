#include "x64arm/windows_services.hpp"
#include "crt_guest_buffer.hpp"
#include "crt_os_error.hpp"
#include <algorithm>

namespace x64arm {
CrtFile* WindowsServices::crt_file(std::uint64_t stream) {
    stream=crt_stream(stream);const auto base=runtime_base_.value+0x800;
    if(stream>=base && stream<=base+256 && (stream-base)%128==0){
        const auto index=static_cast<unsigned>((stream-base)/128);if(!standard_open_[index])return nullptr;
        auto& file=index?runtime_.standard_output[index-1]:runtime_.standard_input;
        file.stream_address=stream;if(!file.lowio_handle_override){file.standard_handle=0x100+index;file.readable=index==0;file.writable=index!=0;}
        file.descriptor=index;file.stdio=true;file.stdio_slot=index;file.text=runtime_.descriptor_modes[index]!=0x8000;
        return &file;
    }
    const auto found=runtime_.streams.find(stream);return found==runtime_.streams.end()?nullptr:&found->second;
}
std::uint64_t WindowsServices::crt_descriptor_stream(std::uint32_t fd) {
    if(fd<3)return standard_open_[fd]?crt_stream(runtime_base_.value+0x800+fd*128):0;
    const auto found=runtime_.descriptors.find(fd);return found==runtime_.descriptors.end()?0:found->second;
}
bool WindowsServices::crt_attach_stream(GuestMemory& memory,CrtFile& file) {
    if(file.stdio_slot!=UINT32_MAX)return true;
    std::uint32_t slot=3;for(const auto& [index,pointer]:runtime_.stdio_slots){(void)pointer;if(index==slot)++slot;else if(index>slot)break;}
    if(slot>=runtime_.max_stdio){memory.store(GuestAddress{runtime_.errno_cell},32,24);return false;}
    runtime_.stdio_slots.emplace(slot,file.stream_address);file.stdio=true;file.stdio_slot=slot;return true;
}
std::uint64_t WindowsServices::crt_install_file(GuestState& state,GuestMemory& memory,CrtFile file,std::optional<std::uint32_t> forced,bool duplicate) {
    const auto fail=[&](unsigned code){memory.store(GuestAddress{runtime_.errno_cell},32,code);return UINT64_C(0);};
    auto descriptor=forced.value_or(runtime_.next_descriptor);
    if(!forced)for(unsigned i=0;i<3;++i)if(!standard_open_[i]){descriptor=i;break;}
    if(!forced)while(descriptor<8192 && (descriptor<3?standard_open_[descriptor]:runtime_.descriptors.contains(descriptor)))++descriptor;
    if(descriptor>=8192)return fail(24);
    try{(void)crt_lowio_record(memory,descriptor,true);}catch(const std::bad_alloc&){return fail(12);}
    const auto previous=forced?crt_descriptor_stream(descriptor):0;
    auto* old=previous?crt_file(previous):nullptr;
    const auto old_buffer=old?old->buffer:CrtFile::Buffer{};
    const auto old_slot=old?old->stdio_slot:UINT32_MAX;
    const bool old_stdio=old && old->stdio;
    const auto base=runtime_base_.value+0x800;
    const bool standard_pointer=previous>=base && previous<=base+256 && (previous-base)%128==0;
    const auto pointer=old && !standard_pointer?previous:allocate_heap(memory,128);
    if(!pointer)return fail(12);
    file.descriptor=descriptor;file.stream_address=pointer;
    file.lowio_published=file.lowio_handle_override=false;file.lowio_handle=0;file.lowio_flags=file.lowio_read_flags=0;
    file.lowio_lookahead={10,10,10};file.lowio_low_surrogate.reset();file.lowio_needed_input=0;file.lowio_multibyte_size=0;
    if(duplicate)file.lowio_eof=false;
    if(forced && old){file.buffer=old_buffer;file.stdio=old_stdio;file.stdio_slot=old_slot;}
    if(descriptor<3){file.stdio=true;file.stdio_slot=descriptor;}
    if(file.stdio && file.stdio_slot==UINT32_MAX && !crt_attach_stream(memory,file)){if(!old || standard_pointer)(void)free_heap(memory,pointer,state);return 0;}
    if(!file.description && !file.standard_handle){file.description=runtime_.allocate_wait_handle();runtime_.file_positions[file.description]=file.position;}
    // 重複描述元共用 FILE_OBJECT 的位置，各自持有可關閉的客體 HANDLE。
    if(!file.standard_handle && (!file.os_handle || duplicate)){
        const auto sibling=file.os_handle?files_.find(file.os_handle):files_.end();
        const auto handle=runtime_.allocate_wait_handle();
        if(sibling!=files_.end())files_.emplace(handle,sibling->second);
        else {
            FileHandle opened;opened.key=file.key;opened.path=file.key;opened.native=file.native;opened.metadata=file.metadata;
            opened.bytes=std::make_shared<const std::vector<std::uint8_t>>(file.bytes);
            opened.access=(file.readable?0x80000000U:0U)|(file.writable?0x40000000U:0U);opened.sharing=file.sharing;
            const auto peer=std::find_if(runtime_.streams.begin(),runtime_.streams.end(),[&](const auto& entry){return entry.second.description==file.description && entry.second.os_handle && files_.contains(entry.second.os_handle);});
            if(peer!=runtime_.streams.end()){opened=files_.at(peer->second.os_handle);}
            else {opened.object=handle;runtime_.win_file_positions[handle]=file.position;}
            files_.emplace(handle,std::move(opened));
        }
        file.os_handle=handle;
    }
    if(file.os_handle)runtime_.handle_flags[file.os_handle]=(runtime_.handle_flags[file.os_handle]&~1U)|(file.noinherit?0U:1U);
    if(old){old->buffer={};old->stdio_slot=UINT32_MAX;(void)crt_close_file(state,memory,previous,false,!standard_pointer);}
    memory.store(GuestAddress{pointer+24},32,(file.readable?1U:0U)|(file.writable?2U:0U));memory.store(GuestAddress{pointer+28},32,descriptor);
    runtime_.descriptors[descriptor]=pointer;
    if(file.stdio_slot>=3 && file.stdio_slot!=UINT32_MAX)runtime_.stdio_slots[file.stdio_slot]=pointer;
    runtime_.streams.emplace(pointer,std::move(file));
    if(descriptor<3){runtime_.crt_redirects[base+descriptor*128]=pointer;standard_open_[descriptor]=true;}
    else if(!forced || descriptor==runtime_.next_descriptor){runtime_.next_descriptor=descriptor+1;}
    crt_buffer_fields(memory,runtime_.streams.at(pointer));return pointer;
}
bool WindowsServices::crt_close_file(GuestState& state,GuestMemory& memory,std::uint64_t stream,bool flush,bool keep) {
    stream=crt_stream(stream);auto* file=crt_file(stream);
    if(!file){memory.store(GuestAddress{runtime_.errno_cell},32,9);return false;}
    bool success=!flush || flush_crt_file(memory,*file);
    if(flush && file->commit_on_close && file->writable && file->native)if(const auto code=file->native->flush()){if(success)crt_os_error::store(memory,runtime_.errno_cell,code);success=false;}
    if(file->buffer.owned)(void)free_heap(memory,file->buffer.address,state);
    if(file->stdio_slot>=3 && file->stdio_slot!=UINT32_MAX)runtime_.stdio_slots.erase(file->stdio_slot);
    const auto base=runtime_base_.value+0x800;
    if(stream>=base && stream<=base+256 && (stream-base)%128==0){const auto handle=crt_lowio_handle(*file);close_crt_lowio(memory,file->descriptor);file->lowio_published=false;file->buffer={};standard_open_[(stream-base)/128]=false;if(handle)close_transfer_handle(memory,handle);return success;}
    const auto description=file->description,os_handle=file->os_handle;const auto descriptor=file->descriptor;
    for(auto redirect=runtime_.crt_redirects.begin();redirect!=runtime_.crt_redirects.end();){
        if(redirect->second!=stream){++redirect;continue;}
        const auto original=redirect->first;if(original>=base && original<=base+256 && (original-base)%128==0){const auto index=(original-base)/128;standard_open_[index]=false;runtime_.handle_flags.erase(0x100+index);}
        redirect=runtime_.crt_redirects.erase(redirect);
    }
    close_crt_lowio(memory,descriptor);runtime_.descriptors.erase(descriptor);runtime_.streams.erase(stream);runtime_.file_locks.erase(stream);
    if(descriptor>=3)runtime_.next_descriptor=std::min(runtime_.next_descriptor,descriptor);
    if(os_handle)close_transfer_handle(memory,os_handle);
    if(description && std::none_of(runtime_.streams.begin(),runtime_.streams.end(),[&](const auto& entry){return entry.second.description==description;}))runtime_.file_positions.erase(description);
    if(!keep)(void)free_heap(memory,stream,state);return success;
}
void WindowsServices::crt_buffer_fields(GuestMemory& memory,CrtFile& file) {
    publish_crt_lowio(memory,file);
    if(!file.stream_address)return;
    const auto& buffer=file.buffer;const auto width=static_cast<std::uint8_t>(x86_?32:64);const auto unit=width/8U;
    const auto next=buffer.direction==CrtFile::Buffer::Direction::Read?buffer.cursor:buffer.used;
    const auto remaining=buffer.direction==CrtFile::Buffer::Direction::Read?buffer.used-buffer.cursor:buffer.direction==CrtFile::Buffer::Direction::Write?buffer.capacity-buffer.used:0U;
    memory.store(GuestAddress{file.stream_address},width,buffer.address?buffer.address+next:0);
    memory.store(GuestAddress{file.stream_address+unit},width,buffer.address);
    memory.store(GuestAddress{file.stream_address+unit*2},32,remaining);
}
std::uint64_t WindowsServices::crt_stream_position(GuestMemory& memory,CrtFile& file,bool pushback) {
    import_crt_buffer(memory,file,x86_);
    auto position=static_cast<std::uint64_t>(crt_file_position(file));const auto& buffer=file.buffer;
    if(buffer.direction==CrtFile::Buffer::Direction::Read && position==buffer.end)position-=buffer.used-buffer.cursor;
    if(buffer.direction==CrtFile::Buffer::Direction::Write){
        std::uint64_t pending=buffer.used;
        if(file.text)for(std::uint32_t i=0;i<buffer.used;++i)pending+=memory.load(GuestAddress{buffer.address+i},8)==10?1U:0U;
        if(position>UINT64_MAX-pending)return UINT64_MAX;position+=pending;
    }
    if(pushback)position=position>=file.pushback.size()?position-file.pushback.size():0;return position;
}
bool WindowsServices::flush_crt_file(GuestMemory& memory,CrtFile& file,bool discard_input) {
    if(!import_crt_lowio(memory,file)){file.error=true;return false;}
    import_crt_buffer(memory,file,x86_);
    auto& buffer=file.buffer;
    if(buffer.direction==CrtFile::Buffer::Direction::Write && buffer.used){
        while(buffer.used){
            std::vector<std::uint8_t> bytes(std::min<std::uint32_t>(buffer.used,65536));memory.read(GuestAddress{buffer.address},bytes);
            const auto count=static_cast<std::uint32_t>(write_crt_file(memory,file,bytes,true));
            if(count){memory.move(GuestAddress{buffer.address},GuestAddress{buffer.address+count},buffer.used-count);buffer.used-=count;}
            if(count<bytes.size()){file.error=true;crt_buffer_fields(memory,file);return false;}
        }
        buffer.direction=CrtFile::Buffer::Direction::None;
    }else if(buffer.direction==CrtFile::Buffer::Direction::Read && discard_input){
        auto position=crt_file_position(file);const auto guard=position.lock();if(static_cast<std::uint64_t>(position)==buffer.end)position=static_cast<std::uint64_t>(position)-(buffer.used-buffer.cursor);
        buffer.used=buffer.cursor=0;buffer.direction=CrtFile::Buffer::Direction::None;
    }
    if(discard_input)file.pushback.clear();crt_buffer_fields(memory,file);return true;
}
bool WindowsServices::flush_crt_streams(GuestMemory& memory) {
    bool result=true;
    for(unsigned i=1;i<3;++i)if(auto* file=crt_file(runtime_base_.value+0x800+i*128))result=flush_crt_file(memory,*file,false) && result;
    for(auto& [pointer,file]:runtime_.streams){(void)pointer;if(file.stdio && file.writable)result=flush_crt_file(memory,file,false) && result;}
    return result;
}
}
