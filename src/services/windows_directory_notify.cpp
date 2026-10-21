#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "directory_watch.hpp"
#include <algorithm>

namespace x64arm {
void WindowsServices::poll_directory_notifications() {
    std::erase_if(directory_reads_,[&](const auto& item){return std::none_of(files_.begin(),files_.end(),[&](const auto& file){return file.second.object==item.first;});});
    for(auto& [object,watch]:directory_reads_){(void)object;watch->poll();}
    for(auto& [handle,watch]:change_notifications_){
        watch->poll();const auto event=runtime_.wait_objects.find(handle);
        if(event!=runtime_.wait_objects.end() && !event->second.signaled() && watch->take({}))event->second.set_signal(true);
    }
}
PipeResult WindowsServices::read_directory_notification(const PendingFileIo& io,GuestMemory& memory) {
    const auto file=files_.find(io.handle);if(file==files_.end())return {0,995};
    const auto watch=directory_reads_.find(file->second.object);if(watch==directory_reads_.end())return {0,995};
    std::vector<std::uint8_t> bytes(io.output_size);
    const auto result=watch->second->take(bytes);if(!result)return {0,0,true};
    if(result->bytes)memory.write(GuestAddress{io.output},std::span(bytes).first(static_cast<std::size_t>(result->bytes)));
    return {static_cast<std::uint32_t>(result->bytes),result->error};
}
std::optional<std::uint32_t> WindowsServices::invoke_directory_notification(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto u32=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const auto finish=[&](std::uint64_t result)->std::optional<std::uint32_t>{arg.return_to(state,result);return std::nullopt;};
    const auto fail=[&](std::uint32_t error,bool first=false){last_error_=error;return finish(first?(x86_?UINT32_MAX:UINT64_MAX):0);};
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許監看宿主目錄變化");
    if(symbol.starts_with("FindFirstChangeNotification")){
        if(!arg(0) || !u32(2) || (u32(2)&~0x17fU))return fail(87,true);
        if(change_notifications_.size()>=64)return fail(8,true);
        const auto path=symbol.ends_with('W')?utf16_to_utf8(read_utf16(memory,GuestAddress{arg(0)})):read_ansi(memory,arg(0));
        const auto info=filesystem_.information(path);if(info.error)return fail(info.error,true);
        if(!(info.metadata.attributes&0x10))return fail(267,true);
        const auto opened=filesystem_.open_directory_observer(path);if(opened.error)return fail(opened.error,true);
        std::uint32_t error=0;auto watch=DirectoryWatch::create(*opened.file,u32(1)!=0,u32(2),4096,error);
        if(!watch)return fail(error,true);
        const auto handle=runtime_.allocate_wait_handle();WaitObject event;event.kind=WaitObject::Kind::ChangeNotification;event.manual_reset=true;event.access=0x100000;
        change_notifications_.emplace(handle,std::move(watch));runtime_.wait_objects.emplace(handle,std::move(event));return finish(handle);
    }
    if(symbol=="FindCloseChangeNotification" || symbol=="FindNextChangeNotification"){
        const auto found=change_notifications_.find(arg(0));if(found==change_notifications_.end())return fail(6);
        if(symbol=="FindCloseChangeNotification"){change_notifications_.erase(found);runtime_.wait_objects.erase(arg(0));runtime_.handle_flags.erase(arg(0));}
        else {runtime_.wait_objects.at(arg(0)).set_signal(false);poll_directory_notifications();}
        return finish(1);
    }
    const auto file=files_.find(arg(0));if(file==files_.end())return fail(6);
    if(!(file->second.metadata.attributes&0x10))return fail(267);
    if(!(file->second.access&0x80000001))return fail(5);
    const auto previous=std::find_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& io){return io.kind==PendingFileIo::Kind::Directory && io.blocking && io.thread==threads_.current && io.handle==arg(0);});
    if(previous!=pending_file_io_.end()){
        if(!previous->done)throw HostCallFault("目錄變更尚未完成卻恢復執行緒");
        const auto result=previous->result;pending_file_io_.erase(previous);threads_.items.at(threads_.current).wait={};
        return result.error?fail(result.error):finish(1);
    }
    PendingFileIo io;io.kind=PendingFileIo::Kind::Directory;io.handle=arg(0);io.thread=threads_.current;
    io.output=arg(1);io.output_size=u32(2);io.overlapped=arg(6);io.routine=arg(7);io.blocking=!io.overlapped;io.count_pointer=io.blocking?arg(5):0;
    if(!io.output || !io.output_size || io.output_size>output_limit || !u32(4) || (u32(4)&~0x17fU))return fail(87);
    if(io.output&3)return fail(998);
    if((io.blocking && !io.count_pointer) || (io.overlapped && (file->second.mode&0x20)) || (io.routine && (!io.overlapped || file->second.completion_port)))return fail(87);
    if(pending_file_io_.size()>=1024)return fail(8);
    if(io.overlapped && std::any_of(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& item){return item.overlapped==io.overlapped;}))return fail(87);
    if(io.routine && threads_.items.at(threads_.current).apcs.size()+static_cast<std::size_t>(std::count_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& p){return p.thread==io.thread && p.routine;}))>=1024)return fail(8);
    const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
    memory.check(GuestAddress{io.output},io.output_size,Access::Write);
    if(io.overlapped){
        memory.check(GuestAddress{io.overlapped},x86_?20:32,Access::Write);
        io.event=io.routine?0:memory.load(GuestAddress{io.overlapped+(x86_?16:24)},bits);
        if(io.event&~UINT64_C(1)){const auto event=runtime_.wait_objects.find(io.event&~UINT64_C(1));if(event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event)return fail(6);}
    }
    auto watch=directory_reads_.find(file->second.object);
    if(watch!=directory_reads_.end() && !watch->second->matches(u32(3)!=0,u32(4)))return fail(50);
    if(watch==directory_reads_.end()){
        if(directory_reads_.size()>=64)return fail(8);
        auto native=file->second.native;
        if(!native){const auto opened=filesystem_.open_directory_observer(file->second.path);if(opened.error)return fail(opened.error);native=opened.file;}
        std::uint32_t error=0;auto created=DirectoryWatch::create(*native,u32(3)!=0,u32(4),io.output_size,error);
        if(!created)return fail(error);
        directory_reads_.emplace(file->second.object,std::move(created));
    }
    if(io.count_pointer){memory.check(GuestAddress{io.count_pointer},4,Access::Write);memory.store(GuestAddress{io.count_pointer},32,0);}
    if(io.overlapped){
        memory.store(GuestAddress{io.overlapped},bits,0x103);memory.store(GuestAddress{io.overlapped+unit},bits,0);
        if(io.event&~UINT64_C(1))runtime_.wait_objects.at(io.event&~UINT64_C(1)).set_signal(false);
    }
    io.notify=io.overlapped!=0;set_file_io_signaled(io.handle,false);pending_file_io_.push_back(io);poll_directory_notifications();poll_file_io(memory);
    if(io.overlapped)return finish(1); // 已成功排入非同步要求，不回 ReadFile 的 FALSE/997。
    const auto current=std::find_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& item){return item.kind==PendingFileIo::Kind::Directory && item.blocking && item.thread==io.thread && item.handle==io.handle;});
    if(current->done){const auto result=current->result;pending_file_io_.erase(current);return result.error?fail(result.error):finish(1);}
    auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::FileIo;thread.wait.handles={io.handle};schedule(state,memory);return std::nullopt;
}
}
