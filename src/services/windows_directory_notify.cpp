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
PipeResult WindowsServices::read_directory_notification(const PendingFileIo& io,GuestMemory& memory) try {
    const auto file=files_.find(io.handle);if(file==files_.end())return {0,995};
    const auto watch=directory_reads_.find(file->second.object);if(watch==directory_reads_.end())return {0,995};
    // 沒有通知時不依客體 buffer 大小反覆配置；暫存只涵蓋本次待交付的內容。
    std::vector<std::uint8_t> bytes(std::min<std::size_t>(io.output_size,watch->second->pending_bytes()));
    const auto result=watch->second->take(bytes);if(!result)return {0,0,true};
    try{if(result->bytes)memory.write(GuestAddress{io.output},std::span(bytes).first(static_cast<std::size_t>(result->bytes)));}
    catch(const MemoryFault&){return {0,998};}
    // Native 以資訊狀態表示需重新列舉；Win32 保留成功且零 bytes 的既有緩衝溢位語義。
    return {static_cast<std::uint32_t>(result->bytes),io.native_iosb && !result->bytes && !result->error?1022U:result->error};
}catch(const std::bad_alloc&){return {0,8};}
std::optional<std::uint32_t> WindowsServices::invoke_directory_notification(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const bool native=symbol.starts_with("NtNotifyChangeDirectory");
    const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
    const auto u32=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const auto finish=[&](std::uint64_t result)->std::optional<std::uint32_t>{if(native)threads_.items.at(threads_.current).last_native_status=static_cast<std::uint32_t>(result);arg.return_to(state,result);return std::nullopt;};
    const auto fail=[&](std::uint32_t error,bool first=false){if(native)return finish(file_io_status(error));last_error_=error;return finish(first?(x86_?UINT32_MAX:UINT64_MAX):0);};
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許監看宿主目錄變化");
    try {
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
    // 關閉 handle 已發布的取消結果仍屬原要求；先恢復結果，不能被新的 handle 查詢蓋掉。
    const auto previous=std::find_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& io){return io.kind==PendingFileIo::Kind::Directory && io.blocking && io.thread==threads_.current && io.handle==arg(0);});
    if(previous!=pending_file_io_.end()){
        if(!previous->done)throw HostCallFault("目錄變更尚未完成卻恢復執行緒");
        const auto result=previous->result;pending_file_io_.erase(previous);threads_.items.at(threads_.current).wait={};
        return result.error?fail(result.error):finish(native?0:1);
    }
    const auto file=files_.find(arg(0));if(file==files_.end())return fail(6);
    if(!(file->second.metadata.attributes&0x10))return fail(267);
    if(!(file->second.access&0x80000001))return fail(5);
    PendingFileIo io;io.kind=PendingFileIo::Kind::Directory;io.handle=arg(0);io.thread=threads_.current;
    unsigned information_class=1;
    if(native){
        io.output=arg(5);io.output_size=u32(6);io.native_iosb=arg(4);io.native_context=arg(3);io.event=arg(1);io.routine=arg(2);io.blocking=(file->second.mode&0x30U)!=0;
        if(!io.native_iosb)return fail(998);
        memory.check(GuestAddress{io.native_iosb},unit*2,Access::Write);
        if((io.blocking && (io.event || io.routine)) || (io.routine && file->second.completion_port) ||
           (io.native_context && !io.routine && !file->second.completion_port))return fail(87);
        if(symbol.ends_with("Ex"))information_class=u32(9);
    }else {
        io.output=arg(1);io.output_size=u32(2);io.overlapped=arg(6);io.routine=arg(7);io.blocking=!io.overlapped;io.count_pointer=io.blocking?arg(5):0;
        if(symbol=="ReadDirectoryChangesExW")information_class=u32(8);
    }
    const auto filter=u32(native?7:4);const bool subtree=native?static_cast<std::uint8_t>(arg(8))!=0:u32(3)!=0;
    if(information_class<1 || information_class>(native?3U:2U))return native?finish(0xc0000003U):fail(87);
    if((!io.output && io.output_size) || (!native && (!io.output || !io.output_size)) || io.output_size>output_limit || !filter || (filter&~0x17fU))return fail(87);
    if(io.output&3)return fail(998);
    if(!native && ((io.blocking && !io.count_pointer) || (io.overlapped && (file->second.mode&0x30)) || (io.routine && (!io.overlapped || file->second.completion_port))))return fail(87);
    if(pending_file_io_.size()>=1024)return fail(8);
    if(std::any_of(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& item){return (io.overlapped && item.overlapped==io.overlapped) || (io.native_iosb && item.native_iosb==io.native_iosb);}))return fail(87);
    if(io.routine && threads_.items.at(threads_.current).apcs.size()+static_cast<std::size_t>(std::count_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& p){return p.thread==io.thread && p.routine;}))>=1024)return fail(8);
    if(io.output_size)memory.check(GuestAddress{io.output},io.output_size,Access::Write);
    if(io.count_pointer)memory.check(GuestAddress{io.count_pointer},4,Access::Write);
    if(io.overlapped){
        memory.check(GuestAddress{io.overlapped},x86_?20:32,Access::Write);
        io.event=io.routine?0:memory.load(GuestAddress{io.overlapped+(x86_?16:24)},bits);
    }
    const auto event_handle=native?io.event:io.event&~UINT64_C(1);
    if(event_handle){const auto event=runtime_.wait_objects.find(event_handle);if(event==runtime_.wait_objects.end() || event->second.kind!=WaitObject::Kind::Event)return fail(6);}
    auto watch=directory_reads_.find(file->second.object);
    if(watch!=directory_reads_.end() && !watch->second->matches(subtree,filter,information_class))return fail(50);
    if(watch==directory_reads_.end()){
        if(directory_reads_.size()>=64)return fail(8);
        auto source=file->second.native;
        if(!source){const auto opened=filesystem_.open_directory_observer(file->second.path);if(opened.error)return fail(opened.error);source=opened.file;}
        std::uint32_t error=0;auto created=DirectoryWatch::create(*source,subtree,filter,io.output_size,error,information_class);
        if(!created)return fail(error);
        directory_reads_.emplace(file->second.object,std::move(created));
    }
    if(io.count_pointer)memory.store(GuestAddress{io.count_pointer},32,0);
    const auto status_block=native?io.native_iosb:io.overlapped;
    if(status_block){
        memory.store(GuestAddress{status_block},bits,0x103);memory.store(GuestAddress{status_block+unit},bits,0);
        if(event_handle)runtime_.wait_objects.at(event_handle).set_signal(false);
    }
    io.notify=!io.blocking;set_file_io_signaled(io.handle,false);pending_file_io_.push_back(io);poll_directory_notifications();poll_file_io(memory);
    if(!io.blocking)return finish(native?memory.load(GuestAddress{io.native_iosb},bits):1); // Win32 排入要求成功；Native 區分即時完成與 pending。
    const auto current=std::find_if(pending_file_io_.begin(),pending_file_io_.end(),[&](const auto& item){return item.kind==PendingFileIo::Kind::Directory && item.blocking && item.thread==io.thread && item.handle==io.handle;});
    if(current->done){const auto result=current->result;pending_file_io_.erase(current);return result.error?fail(result.error):finish(native?0:1);}
    auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::FileIo;thread.wait.handles={io.handle};schedule(state,memory);return std::nullopt;
    }catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(8);}
}
}
