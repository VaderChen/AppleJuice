#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "windows_etw_support.hpp"
#include <algorithm>

namespace x64arm {
std::optional<std::uint32_t> WindowsServices::finish_etw_exception(GuestState& state,GuestMemory& memory){
    const auto dispatch=exceptions_.stack.back();const auto found=etw_processes_.find(dispatch.etw_thread);
    if(found==etw_processes_.end())throw HostCallFault("ETW 例外邊界沒有 ProcessTrace operation");const auto saved=found->second.saved;
    if(x86_)memory.store(GuestAddress{saved.fs_base},32,dispatch.etw_seh_boundary);
    discard_callbacks(dispatch.etw_callback_depth,state,memory);discard_etw_process(state,memory,dispatch.etw_thread);exceptions_.stack.resize(dispatch.etw_exception_depth);
    state=saved;threads_.items.at(dispatch.etw_thread).wait={};complete_call(state,memory,998);return {};
}
void WindowsServices::discard_etw_process(GuestState& state,GuestMemory& memory,std::uint64_t thread){
    const auto found=etw_processes_.find(thread);if(found==etw_processes_.end())return;const auto operation=std::move(found->second);etw_processes_.erase(found);
    (void)runtime_.traces.finish(operation.handles);if(operation.scratch)(void)free_heap(memory,operation.scratch,state);
    for(auto id:operation.handles)if(!runtime_.traces.consumer(id)){const auto consumer=etw_consumers_.find(id);if(consumer==etw_consumers_.end())continue;
        if(consumer->second.last_event)(void)free_heap(memory,consumer->second.last_event,state);(void)free_heap(memory,consumer->second.snapshot,state);etw_consumers_.erase(consumer);}
}
std::optional<std::uint32_t> WindowsServices::resume_etw_process(GuestState& state,GuestMemory& memory,std::uint64_t thread,std::uint64_t result){
    const auto found=etw_processes_.find(thread);if(found==etw_processes_.end())throw HostCallFault("ETW ProcessTrace 回呼沒有 operation");
    if(found->second.scratch){(void)free_heap(memory,found->second.scratch,state);found->second.scratch=0;}
    if(found->second.buffer_callback && !static_cast<std::uint32_t>(result)){const auto saved=found->second.saved;discard_etw_process(state,memory,thread);state=saved;complete_call(state,memory,1223);return {};}
    found->second.buffer_callback=false;
    if(found->second.class_pending){
        const auto consumer=found->second.class_consumer,event=found->second.class_event;
        const auto guid=found->second.class_guid;
        found->second.class_pending=false;
        // 前一個公開事件 callback 可重入移除／替換分類 callback 或關閉 trace；派送前重新核對。
        const auto registration=etw_class_callbacks_.find(guid);
        const auto trace=runtime_.traces.consumer(consumer);
        if(registration!=etw_class_callbacks_.end() && etw_consumers_.contains(consumer) && trace && !trace->closing){
            start_callbacks(state,memory,{{registration->second,{event}}},Continuation::EtwProcess,thread);return {};
        }
    }
    return advance_etw_process(state,memory,thread);
}
std::optional<std::uint32_t> WindowsServices::advance_etw_process(GuestState& state,GuestMemory& memory,std::uint64_t thread){
    const auto shape=etw::guest::layout(x86_);const auto finish=[&](std::uint32_t status){const auto saved=etw_processes_.at(thread).saved;discard_etw_process(state,memory,thread);state=saved;threads_.items.at(thread).wait={};complete_call(state,memory,status);return std::optional<std::uint32_t>{};};
    try{while(true){auto& operation=etw_processes_.at(thread);const auto delivery=runtime_.traces.take(operation.handles);
        if(delivery.kind==etw::DeliveryKind::End)return finish(0);
        if(delivery.kind==etw::DeliveryKind::Wait){auto& current=threads_.items.at(thread);current.status=ThreadStatus::Waiting;current.wait={};current.wait.kind=ThreadWaitKind::EtwTrace;current.wait.handles=operation.handles;schedule(state,memory);return {};}
        auto& info=etw_consumers_.at(delivery.consumer);const auto snapshot=info.snapshot;
        if(delivery.kind==etw::DeliveryKind::Buffer){if(info.buffers_read!=UINT32_MAX)++info.buffers_read;
            memory.store(GuestAddress{snapshot+shape.unit*2+8},32,info.buffers_read);memory.store(GuestAddress{snapshot+shape.buffer_size},32,delivery.buffer.size);
            memory.store(GuestAddress{snapshot+shape.filled},32,delivery.buffer.filled);memory.store(GuestAddress{snapshot+shape.lost},32,0);
            if(info.buffer_callback){operation.buffer_callback=true;start_callbacks(state,memory,{{info.buffer_callback,{snapshot}}},Continuation::EtwProcess,thread);return {};}continue;
        }
        auto record=delivery.record;if(info.header.mode&0x800U)record.flags|=2;
        const auto timestamp=(info.mode&0x1000U)?record.timestamp:delivery.time;
        const auto allocation=allocate_heap(memory,etw::guest::event_size(record,x86_,false));if(!allocation)return finish(8);
        try{const auto bytes=etw::guest::event(record,x86_,false,allocation,timestamp,0);memory.write(GuestAddress{allocation},bytes);
            memory.write(GuestAddress{snapshot+shape.current_event},std::span(bytes).first(shape.event_trace));}
        catch(...){(void)free_heap(memory,allocation,state);throw;}
        if(info.last_event)(void)free_heap(memory,info.last_event,state);info.last_event=allocation;memory.store(GuestAddress{snapshot+shape.unit*2},64,delivery.time);
        const auto class_callback=record.kind==etw::Kind::Modern?etw_class_callbacks_.end():etw_class_callbacks_.find(record.provider);
        if(!info.event_callback && class_callback==etw_class_callbacks_.end())continue;
        std::uint64_t event=allocation;
        if(!info.event_callback){operation.buffer_callback=false;start_callbacks(state,memory,{{class_callback->second,{allocation}}},Continuation::EtwProcess,thread);return {};}
        if(info.mode&0x10000000U){operation.scratch=allocate_heap(memory,etw::guest::event_size(record,x86_,true));if(!operation.scratch)return finish(8);
            memory.write(GuestAddress{operation.scratch},etw::guest::event(record,x86_,true,operation.scratch,timestamp,info.context));event=operation.scratch;}
        operation.class_pending=record.kind!=etw::Kind::Modern;
        operation.class_guid=record.provider;operation.class_consumer=delivery.consumer;operation.class_event=allocation;
        operation.buffer_callback=false;start_callbacks(state,memory,{{info.event_callback,{event}}},Continuation::EtwProcess,thread);return {};
    }}catch(const etw::guest::Fault& error){return finish(error.code);}catch(const MemoryFault&){return finish(998);}catch(const etw::Error& error){return finish(error.failure==etw::Failure::Unsupported?50U:13U);}
}
std::optional<std::uint32_t> WindowsServices::invoke_etw_consumer(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto shape=etw::guest::layout(x86_);const bool opening=name.starts_with("OpenTrace");
    const auto finish=[&](std::uint64_t value,std::uint32_t error=0){if(opening)last_error_=error;arg.return_to(state,value,opening);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){return finish(opening?(x86_?UINT32_MAX:UINT64_MAX):error,error);};
    try{
        if(name=="SetTraceCallback" || name=="RemoveTraceCallback"){
            if(!arg(0) || (name=="SetTraceCallback" && !arg(1)))return finish(87);
            etw::Guid guid{};memory.read(GuestAddress{arg(0)},guid);
            if(name=="RemoveTraceCallback")return finish(etw_class_callbacks_.erase(guid)?0U:4200U);
            if(etw_class_callbacks_.size()>=4096 && !etw_class_callbacks_.contains(guid))return finish(8);
            etw_class_callbacks_.insert_or_assign(guid,arg(1));return finish(0);
        }
        if(opening){if(!arg(0))return fail(87);memory.check(GuestAddress{arg(0)},shape.logfile,Access::Read);memory.check(GuestAddress{arg(0)},shape.logfile,Access::Write);
            const auto file=memory.load(GuestAddress{arg(0)},static_cast<std::uint8_t>(shape.unit*8)),logger=memory.load(GuestAddress{arg(0)+shape.unit},static_cast<std::uint8_t>(shape.unit*8));
            const auto mode=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+shape.unit*2+12},32));if(mode&~0x10001100U)return fail(87);
            const bool realtime=(mode&0x100U)!=0,wide=name.ends_with('W');if((file && logger) || (realtime?(!logger || file):(!file || logger)))return fail(161);
            const auto source=wide?read_utf16(memory,GuestAddress{realtime?logger:file}):utf8_to_utf16(read_ansi(memory,realtime?logger:file,32767));etw::Result result;
            if(realtime){const auto session=runtime_.traces.find_session(source);const auto found=etw_sessions_.find(session);if(found==etw_sessions_.end())return fail(4201);
                if(!etw_session_access(found->second))return fail(5);result=runtime_.traces.open_realtime(source);}
            else{const auto opened=filesystem_.open_reader(utf16_to_utf8(source));if(opened.error)return fail(opened.error);if(opened.metadata.size>512U*1024U*1024U)return fail(8);
                etw::Bytes bytes(static_cast<std::size_t>(opened.metadata.size));std::size_t at=0;while(at<bytes.size()){const auto read=opened.file->read(std::span(bytes).subspan(at),at);if(read.error || !read.bytes)return fail(read.error?read.error:38U);at+=static_cast<std::size_t>(read.bytes);}
                try{result=runtime_.traces.open_file(etw::parse(bytes));}catch(const etw::Error& error){return fail(error.failure==etw::Failure::Unsupported?50U:13U);}}
            if(result.status!=etw::Status::Success)return fail(etw::win_error(result.status));EtwConsumerInfo info;info.header=runtime_.traces.consumer(result.handle)->header;info.mode=mode;
            info.event_callback=memory.load(GuestAddress{arg(0)+shape.event_callback},static_cast<std::uint8_t>(shape.unit*8));info.buffer_callback=memory.load(GuestAddress{arg(0)+shape.buffer_callback},static_cast<std::uint8_t>(shape.unit*8));
            info.context=memory.load(GuestAddress{arg(0)+shape.context},static_cast<std::uint8_t>(shape.unit*8));
            const auto header_name=utf16_bytes(info.header.session_name),header_file=utf16_bytes(info.header.file_name);etw::Bytes top_name;
            if(wide)top_name=utf16_bytes(source);else{const auto text=utf16_to_utf8(source);top_name.assign(text.begin(),text.end());top_name.push_back(0);}
            const auto extent=shape.logfile+top_name.size()+1+header_name.size()+header_file.size();info.snapshot=allocate_heap(memory,extent);
            if(!info.snapshot){(void)runtime_.traces.close(result.handle);return fail(8);}
            try{etw::Bytes snapshot(shape.logfile);memory.read(GuestAddress{arg(0)},snapshot);memory.write(GuestAddress{info.snapshot},snapshot);
                const auto top=info.snapshot+shape.logfile;const auto header_logger=(top+top_name.size()+1)&~UINT64_C(1),header_logfile=header_logger+header_name.size();
                memory.write(GuestAddress{top},top_name);memory.write(GuestAddress{header_logger},header_name);memory.write(GuestAddress{header_logfile},header_file);
                const auto bits=static_cast<std::uint8_t>(shape.unit*8);memory.store(GuestAddress{info.snapshot},bits,realtime?0:top);memory.store(GuestAddress{info.snapshot+shape.unit},bits,realtime?top:0);
                memory.store(GuestAddress{info.snapshot+shape.unit*2},64,info.header.start_time);memory.store(GuestAddress{info.snapshot+shape.unit*2+8},32,0);
                memory.write(GuestAddress{info.snapshot+shape.current_event},etw::Bytes(shape.event_trace));memory.write(GuestAddress{info.snapshot+shape.logfile_header},etw::guest::logfile_header(info.header,x86_,header_logger,header_logfile));
                memory.store(GuestAddress{info.snapshot+shape.buffer_size},32,info.header.buffer_size);memory.store(GuestAddress{info.snapshot+shape.filled},32,0);memory.store(GuestAddress{info.snapshot+shape.lost},32,0);
                memory.store(GuestAddress{info.snapshot+shape.kernel},32,0);
                memory.read(GuestAddress{info.snapshot},snapshot);memory.write(GuestAddress{arg(0)},snapshot);etw_consumers_.emplace(result.handle,std::move(info));
            }catch(...){(void)runtime_.traces.close(result.handle);(void)free_heap(memory,info.snapshot,state);throw;}return finish(result.handle);
        }
        if(name=="CloseTrace"){const auto status=runtime_.traces.close(arg(0));if(status==etw::Status::Success){const auto found=etw_consumers_.find(arg(0));if(found!=etw_consumers_.end()){
                if(found->second.last_event)(void)free_heap(memory,found->second.last_event,state);(void)free_heap(memory,found->second.snapshot,state);etw_consumers_.erase(found);}}
            return finish(etw::win_error(status));}
        if(name!="ProcessTrace")throw HostCallFault("未知 ETW consumer 入口");const auto thread=threads_.current;if(etw_processes_.contains(thread))return advance_etw_process(state,memory,thread);
        const auto count=static_cast<std::uint32_t>(arg(1));if(!count || count>64)return finish(24);if(!arg(0))return finish(87);
        memory.check(GuestAddress{arg(0)},std::size_t{count}*8,Access::Read);std::vector<std::uint64_t> handles;for(std::uint32_t i=0;i<count;++i){const auto handle=memory.load(GuestAddress{arg(0)+std::uint64_t{i}*8},64);if(!etw_consumers_.contains(handle))return finish(6);handles.push_back(handle);}
        const auto start=arg(2)?std::optional{memory.load(GuestAddress{arg(2)},64)}:std::nullopt,end=arg(3)?std::optional{memory.load(GuestAddress{arg(3)},64)}:std::nullopt;
        const auto status=runtime_.traces.begin(handles,start,end);if(status!=etw::Status::Success)return finish(etw::win_error(status));
        EtwProcessOperation operation;operation.saved=state;operation.handles=std::move(handles);etw_processes_.emplace(thread,std::move(operation));return advance_etw_process(state,memory,thread);
    }catch(const etw::guest::Fault& error){return fail(error.code);}catch(const MemoryFault&){return fail(998);}catch(const etw::Error& error){return fail(error.failure==etw::Failure::Unsupported?50U:13U);}catch(const std::bad_alloc&){return fail(8);}
}
}
