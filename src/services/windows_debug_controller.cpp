#include "x64arm/windows_services.hpp"
#include "x64arm/process_debugger.hpp"
#include "x64arm/thread_context.hpp"
#include "windows_debug_state.hpp"
#include <chrono>
#include <system_error>
#include <thread>

namespace x64arm {
namespace {
void append(std::vector<std::uint8_t>& bytes,std::uint64_t value){for(unsigned i=0;i<8;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
std::uint64_t word(std::span<const std::uint8_t> bytes,std::size_t offset=0){
    if(offset>bytes.size() || bytes.size()-offset<8)throw debugger::Error{13};std::uint64_t value=0;
    for(unsigned i=0;i<8;++i)value|=std::uint64_t{bytes[offset+i]}<<(i*8);return value;
}
}
std::uint32_t WindowsServices::debugger_request(GuestMemory& memory,std::uint32_t pid,std::uint32_t kind,std::uint64_t a,std::uint64_t b,std::vector<std::uint8_t>& data){
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許跨程序偵錯");
    if(!debug_memory_ || debug_memory_->owner!=this)return 50;
    auto& channel=*debug_memory_;const auto found=channel.controllers.find(pid);if(found==channel.controllers.end())return 87;
    auto& controller=found->second;if(controller.disconnected || channel.next==UINT64_MAX)return controller.disconnected?87U:8U;
    const auto peer=controller.peer;
    if(channel.active.contains(peer.connection))return 170;
    debugger::Frame request;request.kind=kind;request.sequence=channel.next++;
    for(auto value:{peer.birth,controller.owner,a,b})append(request.payload,value);
    request.payload.insert(request.payload.end(),data.begin(),data.end());
    struct Active {DebugMemoryState& channel;std::uint64_t peer;~Active(){channel.active.erase(peer);channel.disconnected.erase(peer);std::erase_if(channel.replies,[&](const auto& item){return item.first.first==peer;});}} active{channel,peer.connection};
    channel.active.emplace(peer.connection,request.sequence);
    try {
        channel.transport.send(peer,request);const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        for(;;){
            poll_handle_transfers(memory,true);poll_debug_memory(memory,1);
            const auto identity=process_debugger::query(pid);
            if(identity.error || identity.exited || identity.identity!=peer.birth || channel.disconnected.contains(peer.connection)){controller.disconnected=true;return 87;}
            const auto reply=channel.replies.find({peer.connection,request.sequence});
            if(reply!=channel.replies.end()){
                if(reply->second.kind!=kind || reply->second.flags!=1 || (reply->second.error && !reply->second.payload.empty()))return 13;
                if(reply->second.error)return reply->second.error;
                data=std::move(reply->second.payload);return 0;
            }
            if(std::chrono::steady_clock::now()>=deadline)return 1460;
        }
    }catch(const std::system_error&){controller.disconnected=true;return 87;}
     catch(const std::invalid_argument&){controller.disconnected=true;return 13;}
}
std::optional<std::uint32_t> WindowsServices::debug_thread_control(GuestMemory& memory,std::uint32_t pid,std::uint64_t birth,std::uint32_t tid,std::uint32_t operation,ExecutionMode mode,std::uint32_t flags,std::uint32_t& value,std::vector<std::uint8_t>& bytes){
    if(!debug_memory_ || debug_memory_->owner!=this)return {};
    const auto found=debug_memory_->controllers.find(pid);if(found==debug_memory_->controllers.end())return {};
    if(found->second.peer.birth!=birth)return 87;
    const auto kind=operation==1?0x306U:operation==4?0x307U:operation==2 || operation==5?0x309U:operation==3?0x30aU:0U;
    if(!kind)return 87;
    const auto spec=operation==1 || operation==4?((std::uint64_t{mode==ExecutionMode::X86?32U:64U}<<32)|flags):0;
    if(const auto error=debugger_request(memory,pid,kind,tid,spec,bytes))return error;
    const auto expected=operation==1?thread_context::size(mode):operation==4?0U:8U;
    if(bytes.size()!=expected)return 13;
    if(operation==2 || operation==3 || operation==5){value=static_cast<std::uint32_t>(word(bytes));bytes.clear();}
    else value=1;
    return 0;
}
bool WindowsServices::finish_debug_wait(GuestState& state,GuestMemory& memory,std::uint64_t owner,std::uint64_t deadline){
    const WindowsCall call(state,memory,2);const auto output=call(0);
    const auto fail=[&](std::uint32_t error){
        if(owner==threads_.current)last_error_=error;
        if(const auto thread=threads_.items.find(owner);thread!=threads_.items.end())memory.store(GuestAddress{thread->second.teb+(x86_?0x34:0x68)},32,error);
        call.return_to(state,0);return true;
    };
    if(!output)return fail(87);
    if(!debug_memory_ || debug_memory_->owner!=this)return fail(87);
    bool owned=false;
    try {
        memory.check(GuestAddress{output},x86_?96:176,Access::Write);
        for(auto& [pid,controller]:debug_memory_->controllers){
            if(controller.owner!=owner)continue;owned=true;
            if(debug_memory_->unicode_owners.contains(owner) && !controller.unicode){
                std::vector<std::uint8_t> reply;const auto error=debugger_request(memory,pid,0x30b,0,0,reply);
                if(error || !reply.empty())return fail(error?error:13U);
                controller.unicode=true;controller.offered.reset();
            }
            if(controller.delivered)continue;
            if(!controller.offered){
                std::vector<std::uint8_t> reply;const auto error=debugger_request(memory,pid,0x302,0,0,reply);
                if(error==121)continue;if(error)return fail(error);if(reply.size()!=184)return fail(13);
                debugger::Envelope envelope{word(reply),controller.peer.birth,debugger::decode(std::span<const std::uint8_t>(reply).subspan(8),ExecutionMode::X64)};
                if(!envelope.sequence || envelope.event.pid!=pid)return fail(13);
                controller.offered=std::move(envelope);
            }
            auto event=controller.offered->event;
            const auto thread_handle=[&](){
                const auto existing=controller.thread_handles.find(event.tid);if(existing!=controller.thread_handles.end())return existing->second;
                const auto handle=make_debug_handle(pid,controller.peer.birth,event.tid);if(!handle)throw debugger::Error{last_error_};
                try{controller.thread_handles.emplace(event.tid,handle);}catch(...){close_process(handle);throw;}return handle;
            };
            if(auto* created=std::get_if<debugger::CreateProcess>(&event.payload)){
                if(!controller.process_handle)controller.process_handle=make_debug_handle(pid,controller.peer.birth,0);
                if(!controller.process_handle)return fail(last_error_);created->process=controller.process_handle;created->thread=thread_handle();created->file=0;
            }else if(auto* created_thread=std::get_if<debugger::CreateThread>(&event.payload))created_thread->thread=thread_handle();
            else if(auto* loaded=std::get_if<debugger::LoadDll>(&event.payload))loaded->file=0;
            event.tid=remote_thread_identifier(pid,controller.peer.birth,event.tid);
            const auto encoded=debugger::encode(event,x86_?ExecutionMode::X86:ExecutionMode::X64);
            std::vector<std::uint8_t> previous(encoded.size());memory.read(GuestAddress{output},previous);
            memory.write(GuestAddress{output},encoded);
            std::vector<std::uint8_t> reply;const auto error=debugger_request(memory,pid,0x303,controller.offered->sequence,0,reply);
            if(error || !reply.empty()){memory.write(GuestAddress{output},previous);return fail(error?error:13U);}
            controller.delivered=std::move(controller.offered);controller.offered.reset();call.return_to(state,1);return true;
        }
        if(!owned)return fail(87);
        if(runtime_.clock_ticks>=deadline)return fail(121);
        return false;
    }catch(const MemoryFault&){return fail(998);}catch(const debugger::Error& error){return fail(error.code);}catch(const std::bad_alloc&){return fail(8);}
}
std::optional<std::uint32_t> WindowsServices::invoke_debugger(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);const auto symbol=service.symbol;
    const auto fail=[&](std::uint32_t error){last_error_=error;call.return_to(state,0);return std::optional<std::uint32_t>{};};
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許跨程序偵錯 API");
    if(!debug_memory_ || debug_memory_->owner!=this)return fail(50);
    auto& channel=*debug_memory_;
    if(symbol=="WaitForDebugEvent" || symbol=="WaitForDebugEventEx"){
        if(symbol=="WaitForDebugEventEx")channel.unicode_owners.insert(threads_.current);
        const auto duration=static_cast<std::uint32_t>(call(1));
        const auto deadline=duration==UINT32_MAX?UINT64_MAX:runtime_.clock_ticks+std::uint64_t{duration}*10000;
        if(finish_debug_wait(state,memory,threads_.current,deadline))return std::nullopt;
        auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::DebugEvent;thread.wait.deadline=deadline;
        schedule(state,memory);return std::nullopt;
    }
    const auto pid=static_cast<std::uint32_t>(call(0));
    if(!pid || pid==process_id())return fail(87);
    try {
        if(symbol=="DebugActiveProcess"){
            if(channel.controllers.contains(pid))return fail(5);if(channel.controllers.size()>=64)return fail(8);
            const auto identity=process_debugger::query(pid);if(identity.error || identity.exited || !identity.identity)return fail(87);
            DebugControllerState controller;controller.peer=channel.transport.connect(pid,identity.identity);controller.owner=threads_.current;
            const auto peer=controller.peer;
            try{channel.controllers.emplace(pid,std::move(controller));}catch(...){channel.transport.close(peer);throw;}
            const auto rollback=[&](std::uint32_t error){channel.controllers.erase(pid);channel.transport.close(peer);return fail(error);};
            std::vector<std::uint8_t> reply;auto error=debugger_request(memory,pid,0x308,0,0,reply);
            if(error)return rollback(error);if(reply.size()!=8 || (word(reply)!=32 && word(reply)!=64))return rollback(13);
            if(x86_ && word(reply)==64)return rollback(50);
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            do{reply.clear();error=debugger_request(memory,pid,0x301,0,0,reply);if(error!=170)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<deadline);
            if(error || !reply.empty())return rollback(error?error:13U);
            call.return_to(state,1);return std::nullopt;
        }
        const auto found=channel.controllers.find(pid);if(found==channel.controllers.end())return fail(87);auto& controller=found->second;
        if(controller.owner!=threads_.current)return fail(5);
        const auto close_handle=[&](std::uint64_t handle){if(handle){(void)close_process(handle);runtime_.handle_flags.erase(handle);}};
        const auto close_controller=[&](){for(const auto& [tid,handle]:controller.thread_handles){(void)tid;close_handle(handle);}close_handle(controller.process_handle);const auto peer=controller.peer;channel.controllers.erase(found);try{channel.transport.close(peer);}catch(const std::system_error&){};};
        if(symbol=="DebugActiveProcessStop"){
            std::vector<std::uint8_t> reply;if(const auto error=debugger_request(memory,pid,0x305,0,0,reply))return fail(error);if(!reply.empty())return fail(13);
            close_controller();call.return_to(state,1);return std::nullopt;
        }
        const auto tid=static_cast<std::uint32_t>(call(1)),status=static_cast<std::uint32_t>(call(2));
        if(!controller.delivered || remote_thread_identifier(pid,controller.peer.birth,controller.delivered->event.tid)!=tid)return fail(87);
        if(status!=debugger::handled && status!=debugger::not_handled && status!=debugger::reply_later)return fail(87);
        const auto internal_tid=controller.delivered->event.tid;
        std::vector<std::uint8_t> reply;if(const auto error=debugger_request(memory,pid,0x304,controller.delivered->sequence,(std::uint64_t{internal_tid}<<32)|status,reply))return fail(error);if(!reply.empty())return fail(13);
        if(const auto* exited=std::get_if<debugger::ExitProcess>(&controller.delivered->event.payload)){
            finish_debug_handles(pid,controller.peer.birth,0,exited->code);close_controller();
        }else {
            if(const auto* exited_thread=std::get_if<debugger::ExitThread>(&controller.delivered->event.payload)){
                finish_debug_handles(pid,controller.peer.birth,internal_tid,exited_thread->code);
                if(const auto handle=controller.thread_handles.find(internal_tid);handle!=controller.thread_handles.end()){close_handle(handle->second);controller.thread_handles.erase(handle);}
            }
            controller.delivered.reset();
        }
        call.return_to(state,1);return std::nullopt;
    }catch(const debugger::Error& error){return fail(error.code);}catch(const std::system_error&){return fail(87);}catch(const std::bad_alloc&){return fail(8);}catch(const std::invalid_argument&){return fail(87);}
}
}
