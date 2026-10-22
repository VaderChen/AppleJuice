#include "x64arm/windows_services.hpp"
#include "x64arm/process_debugger.hpp"
#include "x64arm/thread_context.hpp"
#include "x64arm/unicode.hpp"
#include "windows_debug_state.hpp"
#include <chrono>
#include <thread>

namespace x64arm {
namespace {
// 內部協議：target birth、controller guest TID、兩個操作參數，各為 LE64。
// 事件先 offer 再確認 deliver，caller 輸出失敗不會吃掉事件。
constexpr std::uint32_t attach=0x301,offer=0x302,deliver=0x303,resume=0x304,detach=0x305,get_context=0x306,set_context=0x307,architecture=0x308,suspend_thread=0x309,resume_thread=0x30a,unicode_events=0x30b;
std::uint64_t word(std::span<const std::uint8_t> bytes,std::size_t at){
    std::uint64_t value=0;for(unsigned i=0;i<8;++i)value|=std::uint64_t{bytes[at+i]}<<(i*8);return value;
}
void append(std::vector<std::uint8_t>& bytes,std::uint64_t value){for(unsigned i=0;i<8;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
}
bool WindowsServices::debugger_attached()const noexcept {
    return debug_memory_ && debug_memory_->owner==this && debug_memory_->target.has_value();
}
bool WindowsServices::debugger_stopped()const noexcept {
    return debugger_attached() && (debug_memory_->target->disconnected || !debug_memory_->target->events.empty());
}
bool WindowsServices::dispatch_debug_request(GuestMemory& memory,const debugger::TransportEvent& event,debugger::Frame& reply){
    const auto& request=event.frame;if(request.kind<attach || request.kind>unicode_events)return false;
    auto& channel=*debug_memory_;
    const auto context_mode=x86_?ExecutionMode::X86:ExecutionMode::X64;
    const auto expected=32+(request.kind==set_context?thread_context::size(context_mode):0);
    if(request.error || request.payload.size()!=expected){reply.error=87;return true;}
    const auto birth=word(request.payload,0),controller=word(request.payload,8),a=word(request.payload,16),b=word(request.payload,24);
    if(birth!=channel.transport.birth() || !controller || controller>UINT32_MAX){reply.error=87;return true;}
    try{
        if(request.kind==architecture){if(a || b)reply.error=87;else append(reply.payload,x86_?32U:64U);return true;}
        if(request.kind==attach){
            if(a || b){reply.error=87;return true;}
            if(channel.target){reply.error=5;return true;}
            if(channel.breakpoint_thread){reply.error=170;return true;}
            // API 內部臨時輪詢不一定已保存目前 context；要求對端重試下一安全點。
            if(!threads_.context_saved || threads_.items.empty()){reply.error=170;return true;}
            DebugTargetState target;target.peer=event.peer;target.controller_thread=controller;
            target.events.attach(channel.transport.pid(),birth,controller);
            bool first=true;
            for(const auto& [id,thread]:threads_.items){
                if(thread.status==ThreadStatus::Exited)continue;
                if(id>UINT32_MAX)throw debugger::Error{87};
                debugger::Payload payload;
                // handles 由 controller 的 handle table 配置；不可傳送 target 的 handle 值。
                if(first){debugger::CreateProcess info;info.base=runtime_.image.base;info.teb=thread.teb;payload=info;first=false;}
                // DebugActiveProcess 的既有執行緒快照，lpStartAddress 依契約為 NULL。
                else payload=debugger::CreateThread{0,thread.teb,0};
                target.events.push(channel.transport.pid(),birth,{channel.transport.pid(),static_cast<std::uint32_t>(id),std::move(payload)});
            }
            if(first){reply.error=87;return true;}
            const auto initial=target.events.offer(controller)->event.tid;
            if(!channel.breakpoint_page){
                auto base=x86_?UINT64_C(0x7d000000):UINT64_C(0x3ffd10000000);
                const auto limit=base+0x1000000;
                while(base<limit && !memory.range_available(GuestAddress{base},4096))base+=4096;
                if(base==limit || memory.remaining_capacity()<4096){reply.error=8;return true;}
                // CC；間接 JMP。兩條都由 CPU/JIT 執行，且不破壞 GPR、flags 或堆疊。
                std::array<std::uint8_t,24> code{0xcc,0xff,0x25};
                const auto operand=x86_?base+16:9;
                for(unsigned i=0;i<4;++i)code[3+i]=static_cast<std::uint8_t>(operand>>(i*8));
                memory.map(GuestAddress{base},4096,read_execute,code);channel.breakpoint_page=base;
            }
            target.initial_thread=initial;
            for(const auto base:runtime_.module_order){
                const auto found=runtime_.loaded_modules.find(base);if(found==runtime_.loaded_modules.end())continue;
                debugger::LoadDll info;info.base=found->second.image.base;
                target.modules.insert(info.base);
                target.events.push(channel.transport.pid(),birth,{channel.transport.pid(),initial,info});
            }
            channel.target=std::move(target);memory.store(GuestAddress{peb_base_.value+2},8,1);
            threads_.remaining=0;threads_.yield=true;notify_concurrency_changed();return true;
        }
        if(!channel.target || channel.target->peer!=event.peer || channel.target->controller_thread!=controller){reply.error=5;return true;}
        auto& target=*channel.target;
        if(request.kind==offer){
            if(a || b){reply.error=87;return true;}
            const auto* candidate=target.events.offer(controller);
            if(!candidate){reply.error=121;return true;}
            auto output=candidate->event;
            if(const auto* string=std::get_if<debugger::DebugString>(&output.payload);string && string->unicode && !target.unicode){
                if(target.ansi_sequence!=candidate->sequence){
                    // 虛擬 Windows 的 GetACP 為 UTF-8，沿用共用 UTF-16 轉換。
                    auto text=utf16_to_utf8(read_utf16(memory,GuestAddress{string->address}));
                    if(text.size()>65534){std::size_t end=65534;while((static_cast<unsigned char>(text[end])&0xc0)==0x80)--end;text.resize(end);}
                    if(!channel.debug_string_page){
                        auto base=x86_?UINT64_C(0x7c000000):UINT64_C(0x3ffd20000000);const auto limit=base+0x1000000;
                        while(base<limit && !memory.range_available(GuestAddress{base},65536))base+=65536;
                        if(base==limit || memory.remaining_capacity()<65536){reply.error=8;return true;}
                        memory.map(GuestAddress{base},65536,read_write);channel.debug_string_page=base;
                    }
                    std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);
                    memory.write(GuestAddress{channel.debug_string_page},bytes);
                    target.ansi_string={channel.debug_string_page,static_cast<std::uint16_t>(bytes.size()),false};target.ansi_sequence=candidate->sequence;
                }
                output.payload=target.ansi_string;
            }
            append(reply.payload,candidate->sequence);
            auto bytes=debugger::encode(output,ExecutionMode::X64);reply.payload.insert(reply.payload.end(),bytes.begin(),bytes.end());
        }else if(request.kind==unicode_events){
            if(a || b){reply.error=87;return true;}target.unicode=true;
        }else if(request.kind==deliver){
            if(b || !a){reply.error=87;return true;}
            if(target.delivered!=a){(void)target.events.deliver(controller,a);target.delivered=a;}
        }else if(request.kind==resume){
            // 高 32 位為 TID，低 32 位為 DBG_* disposition；序號拒絕重送舊 Continue。
            if(!a || a!=target.delivered){reply.error=87;return true;}
            const auto completed=target.events.continue_event(controller,channel.transport.pid(),static_cast<std::uint32_t>(b>>32),static_cast<std::uint32_t>(b));
            target.continued=completed.sequence;target.disposition=static_cast<std::uint32_t>(b);target.delivered=0;
            notify_concurrency_changed();
        }else if(request.kind==get_context || request.kind==set_context){
            if(!threads_.context_saved){reply.error=170;return true;}
            const auto found=threads_.items.find(a);
            if(found==threads_.items.end() || found->second.status==ThreadStatus::Exited || (b>>32)!=(x86_?32U:64U)){reply.error=87;return true;}
            if(!debugger_stopped() && !found->second.suspended){reply.error=87;return true;}
            const auto flags=static_cast<std::uint32_t>(b);
            if(const auto error=thread_context::validate_flags(context_mode,flags)){reply.error=error;return true;}
            if(request.kind==get_context)reply.payload=thread_context::capture(found->second.context,context_mode,flags);
            else {
                auto& thread=found->second;
                if((flags&1U) && thread.status==ThreadStatus::Waiting){reply.error=170;return true;}
                const auto bytes=std::span<const std::uint8_t>(request.payload).subspan(32);
                const auto offset=thread_context::flags_offset(context_mode);std::uint32_t embedded=0;
                for(unsigned i=0;i<4;++i)embedded|=std::uint32_t{bytes[offset+i]}<<(i*8);
                if(embedded!=flags){reply.error=13;return true;}
                try{thread.context=thread_context::restore(thread.context,context_mode,bytes);notify_concurrency_changed();}
                catch(const HostCallFault&){reply.error=50;}catch(const std::invalid_argument&){reply.error=87;}
            }
        }else if(request.kind==suspend_thread || request.kind==resume_thread){
            const auto found=threads_.items.find(a);
            if(b || found==threads_.items.end() || found->second.status==ThreadStatus::Exited){reply.error=87;return true;}
            auto& thread=found->second;
            if(request.kind==suspend_thread && thread.suspended>=127){reply.error=87;return true;}
            append(reply.payload,thread.suspended);
            if(request.kind==suspend_thread)++thread.suspended;else if(thread.suspended)--thread.suspended;
            threads_.remaining=0;threads_.yield=true;notify_concurrency_changed();
        }else if(request.kind==detach){
            if(a || b){reply.error=87;return true;}
            channel.target.reset();(void)process_debugger::self(memory,peb_base_,runtime_.network.external_io);notify_concurrency_changed();
        }
    }catch(const debugger::Error& error){reply.error=error.code;}
    return true;
}
void WindowsServices::wait_debugger(GuestMemory& memory){
    while(debugger_stopped()){
        if(debug_memory_->target->disconnected)throw HostCallFault("偵錯控制程序已中斷，停止被偵錯客體");
        // 不執行 wake_threads：I/O 可以完成，但不能在事件交付期間覆寫停止時的 context。
        poll_handle_transfers(memory,true);
        if(debugger_stopped())poll_debug_memory(memory,1);
    }
}
void WindowsServices::queue_debug_thread(std::uint64_t id){
    if(!debugger_attached())return;
    if(id>UINT32_MAX)throw HostCallFault("偵錯事件執行緒識別碼超過 DWORD");
    auto& channel=*debug_memory_;const auto& thread=threads_.items.at(id);
    channel.target->events.push(channel.transport.pid(),channel.transport.birth(),
        {channel.transport.pid(),static_cast<std::uint32_t>(id),debugger::CreateThread{0,thread.teb,thread.entry}});
    // 建立者完成 API 回傳後立即進入協調排程；新執行緒連 TLS callback 都不能先執行。
    // CREATE_SUSPENDED 的計數獨立保留，ContinueDebugEvent 不替它 ResumeThread。
    threads_.remaining=0;threads_.yield=true;notify_concurrency_changed();
}
void WindowsServices::emit_debug_module(GuestState& state,GuestMemory& memory,std::uint64_t base,bool loading){
    if(!debugger_attached())return;
    auto& modules=debug_memory_->target->modules;
    if(loading){
        if(!modules.insert(base).second)return;
        debugger::LoadDll info;info.base=base;
        (void)emit_debug_event(state,memory,info);
    }else if(modules.erase(base)){
        (void)emit_debug_event(state,memory,debugger::UnloadDll{base});
    }
}
void WindowsServices::prepare_debug_execution(GuestState& state,GuestMemory& memory){
    if(!debug_memory_ || debug_memory_->owner!=this)return;
    auto& channel=*debug_memory_;
    // CC 後的間接 JMP 也必須執行完，才能覆寫共用跳板的返回位址。
    // VEH 自行改變續跑位置並離開跳板時，同樣解除重新附加的限制。
    if(channel.breakpoint_thread==threads_.current &&
       (state.rip.value<channel.breakpoint_page || state.rip.value>=channel.breakpoint_page+7) &&
       !state.pending_cpu_trap && exceptions_.stack.empty())channel.breakpoint_thread=0;
    if(!channel.target || channel.target->initial_thread!=threads_.current)return;
    const auto page=GuestAddress{channel.breakpoint_page};
    memory.protect(page,4096,read_write);
    memory.store(GuestAddress{page.value+16},x86_?32:64,state.rip.value);
    memory.protect(page,4096,read_execute);
    channel.target->initial_thread=0;channel.breakpoint_thread=threads_.current;
    state.rip=page;
}
std::uint32_t WindowsServices::emit_debug_event(GuestState& state,GuestMemory& memory,debugger::Payload payload){
    const auto* exception=std::get_if<debugger::Exception>(&payload);
    const bool initial=debug_memory_ && debug_memory_->owner==this &&
        debug_memory_->breakpoint_thread==threads_.current && exception &&
        exception->code==0x80000003U && exception->address==debug_memory_->breakpoint_page;
    const auto resume_initial=[&]{
        if(state.rip.value==debug_memory_->breakpoint_page)++state.rip.value;
    };
    if(!debugger_attached()){
        if(initial){resume_initial();return debugger::handled;}
        return debugger::not_handled;
    }
    auto& channel=*debug_memory_;auto& target=*channel.target;
    if(threads_.current>UINT32_MAX)throw HostCallFault("偵錯事件執行緒識別碼超過 DWORD");
    auto& thread=threads_.items.at(threads_.current);
    struct Saved {bool& value;bool previous;~Saved(){value=previous;}} saved{threads_.context_saved,threads_.context_saved};
    thread.context=state;threads_.context_saved=true;
    const auto sequence=target.events.push(channel.transport.pid(),channel.transport.birth(),{channel.transport.pid(),static_cast<std::uint32_t>(threads_.current),std::move(payload)});
    notify_concurrency_changed();wait_debugger(memory);state=thread.context;
    if(!channel.target || channel.target->continued!=sequence){
        if(initial && !channel.target){resume_initial();return debugger::handled;}
        return debugger::not_handled;
    }
    if(initial && channel.target->disposition==debugger::handled)resume_initial();
    return channel.target->disposition;
}
}
