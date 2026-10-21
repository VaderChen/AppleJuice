#include "x64arm/windows_services.hpp"
#include "x64arm/startup_profile.hpp"
#include "x64arm/debug_transport.hpp"
#include "x64arm/process_debugger.hpp"
#include "windows_debug_state.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <system_error>

namespace x64arm {
namespace {
constexpr std::uint32_t read_memory=0x201,write_memory=0x202,flush_memory=0x203,query_memory=0x204,protect_memory=0x205;
constexpr std::uint32_t check_read=0x206,check_write=0x207,query_debugger=0x208;
constexpr std::size_t transfer_limit=65536,request_header=24;
void append(std::vector<std::uint8_t>& bytes,std::uint64_t value,unsigned width=8){for(unsigned i=0;i<width;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
std::uint64_t load(std::span<const std::uint8_t> bytes,std::size_t at,unsigned width=8){
    if(at>bytes.size() || width>bytes.size()-at)throw std::invalid_argument("跨程序記憶體封包截斷");
    std::uint64_t result=0;for(unsigned i=0;i<width;++i)result|=std::uint64_t{bytes[at+i]}<<(i*8);return result;
}
std::uint32_t transport_error(const std::system_error& error){
    switch(error.code().value()){
    case EACCES:case EPERM:return 5;
    case ENOENT:case ECONNREFUSED:case ESRCH:return 87;
    case ENOMEM:case ENOBUFS:case EMFILE:case ENFILE:return 8;
    case ENOTSUP:return 50;
    case ETIMEDOUT:return 1460;
    default:return 13;
    }
}
}
void WindowsServices::configure_debug_memory(){
#if defined(__APPLE__)
    if(debug_memory_)return;
    debug_memory_=std::make_shared<DebugMemoryState>(this,mapping_session());
#endif
}
bool WindowsServices::executable_memory_range(const GuestMemory& memory,std::uint64_t begin,std::uint64_t size) const {
    if(!size || size>UINT64_MAX-begin)return false;const auto end=begin+size;
    auto mapping=memory.query(GuestAddress{begin});if(!mapping)mapping=memory.next_mapping(GuestAddress{begin});
    for(;mapping && mapping->base.value<end;mapping=memory.next_mapping(GuestAddress{mapping->base.value+mapping->size}))
        if(mapping->permissions&permission(Access::Execute))return true;
    return false;
}
void WindowsServices::poll_debug_memory(GuestMemory& memory,unsigned milliseconds,bool force){
#if defined(__APPLE__)
    if(!debug_memory_ || debug_memory_->owner!=this)return;auto& channel=*debug_memory_;
    // 排程器對空通道最多每毫秒探查一次；同步請求、停止中的偵錯器及共享喚醒立即探查。
    const auto now=std::chrono::steady_clock::now();
    if(!force && !milliseconds && channel.pending.empty() && now<channel.next_idle_poll)return;
    const auto close_peer=[&](const debugger::Peer& peer){try{channel.transport.close(peer);}catch(const std::system_error&){}};
    auto events=std::move(channel.pending);channel.pending.clear();
    auto incoming=[&]{startup_profile::Scope measurement("scheduler",force?"debug_transport_forced":milliseconds?"debug_transport_wait":"debug_transport_idle");return channel.transport.poll(milliseconds);}();
    channel.next_idle_poll=events.empty() && incoming.empty()?now+std::chrono::milliseconds(1):std::chrono::steady_clock::time_point{};
    events.insert(events.end(),std::make_move_iterator(incoming.begin()),std::make_move_iterator(incoming.end()));bool replied=false;
    // 所有呼叫點都在協調執行緒；原生 worker 批次已返回，不能在 worker 直接修改映射。
    for(auto& event:events){
        if(event.kind==debugger::TransportEvent::Kind::Connected)continue;
        if(event.kind==debugger::TransportEvent::Kind::Disconnected){
            if(channel.target && channel.target->peer==event.peer)channel.target->disconnected=true;
            if(channel.active.contains(event.peer.connection))channel.disconnected.insert(event.peer.connection);
            std::erase_if(channel.replies,[&](const auto& item){return item.first.first==event.peer.connection;});
            continue;
        }
        const auto& request=event.frame;
        if(request.flags==1){
            const auto active=channel.active.find(event.peer.connection);
            if(active!=channel.active.end() && active->second==request.sequence && channel.replies.size()<128)
                channel.replies.emplace(std::pair{event.peer.connection,request.sequence},std::move(event.frame));
            continue;
        }
        if(request.flags!=0){close_peer(event.peer);continue;}
        debugger::Frame reply;reply.kind=request.kind;reply.flags=1;reply.sequence=request.sequence;
        try {
            if(dispatch_debug_request(memory,event,reply)){}
            else {
            if(request.error || request.payload.size()<request_header)throw std::invalid_argument("跨程序記憶體請求格式無效");
            const auto address=load(request.payload,0),size=load(request.payload,8),birth=load(request.payload,16);
            if(!birth || birth!=channel.transport.birth()){reply.error=87;}
            else if(request.kind!=write_memory && request.kind!=protect_memory && request.payload.size()!=request_header){reply.error=87;}
            else if(request.kind==read_memory || request.kind==write_memory || request.kind==check_read || request.kind==check_write){
                const auto limit=user_address_limit();
                const bool probe=request.kind==check_read || request.kind==check_write;
                if((!probe && size>transfer_limit) || address>=limit || size>limit-address || size>memory.limit())reply.error=299;
                else if(probe){
                    if(request.kind==check_write && (runtime_.dynamic_code_policy&1U) && executable_memory_range(memory,address,size))reply.error=1655;
                    else memory.check(GuestAddress{address},static_cast<std::size_t>(size),request.kind==check_write?Access::Write:Access::Read);
                }
                else if(request.kind==read_memory){reply.payload.resize(static_cast<std::size_t>(size));memory.read(GuestAddress{address},reply.payload);}
                else if(request.payload.size()!=request_header+size)reply.error=87;
                else if((runtime_.dynamic_code_policy&1U) && executable_memory_range(memory,address,size))reply.error=1655;
                else {memory.write(GuestAddress{address},std::span<const std::uint8_t>(request.payload).subspan(request_header));notify_concurrency_changed();}
            }else if(request.kind==query_debugger){
                if(address || size)reply.error=87;
                else {const auto attached=debugger_attached();const auto native=attached?process_debugger::Result{0,true,0,false}:process_debugger::self(memory,peb_base_,runtime_.network.external_io);
                    if(native.error)reply.error=native.error;else append(reply.payload,native.present?1U:0U);}
            }else if(request.kind==flush_memory){
                const auto limit=user_address_limit();
                if(address && (address>=limit || size>limit-address))reply.error=87;
                else {
                    if(address && size && size<=UINT64_C(64)*1024*1024)memory.flush_instruction_cache(GuestAddress{address},static_cast<std::size_t>(size));
                    else memory.flush_instruction_cache();
                    notify_concurrency_changed();
                }
            }else if(request.kind==query_memory){
                if(size || address>=user_address_limit())reply.error=87;
                else {
                    const auto info=virtual_memory_information(memory,address&~UINT64_C(4095));
                    append(reply.payload,info.base);append(reply.payload,info.allocation);append(reply.payload,info.size);
                    append(reply.payload,info.initial,4);append(reply.payload,info.state,4);
                    append(reply.payload,info.protection,4);append(reply.payload,info.type,4);
                }
            }else if(request.kind==protect_memory){
                if(request.payload.size()!=request_header+4)reply.error=87;
                else {
                    const auto mode=static_cast<std::uint32_t>(load(request.payload,request_header,4));
                    const auto at=address&~UINT64_C(4095);const auto mapping=memory.query(GuestAddress{address});
                    const auto limit=user_address_limit();
                    const auto access=mode==1?0:mode==2?1:mode==4?3:mode==16?4:mode==32?5:mode==64?7:-1;
                    if(!size || !mapping || address>=limit || size>limit-address || size>memory.limit() || address+size>UINT64_MAX-4095)reply.error=487;
                    else if(access<0)reply.error=87;
                    else {
                        const auto end=(address+size+4095)&~UINT64_C(4095);
                        if(!protection_allocation_range(at,end))reply.error=487;
                        else if((runtime_.dynamic_code_policy&1U) && ((access&4) || ((access&2) && executable_memory_range(memory,at,end-at))))reply.error=1655;
                        else {
                            const auto old=mapping->permissions==7?64U:mapping->permissions==3?4U:mapping->permissions==5?32U:mapping->permissions==4?16U:mapping->permissions?2U:1U;
                            append(reply.payload,old,4);
                            try{memory.protect(GuestAddress{at},static_cast<std::size_t>(end-at),static_cast<Permissions>(access));notify_concurrency_changed();}
                            catch(const MemoryFault&){reply.error=487;}catch(const std::invalid_argument&){reply.error=5;}
                        }
                    }
                }
            }else reply.error=50;
            }
        }catch(const MemoryFault&){reply.error=299;}catch(const std::bad_alloc&){reply.error=8;}catch(const std::invalid_argument&){reply.error=87;}
        if(reply.error)reply.payload.clear();
        try{channel.transport.send(event.peer,reply);replied=true;}catch(const std::system_error&){close_peer(event.peer);}
    }
    // 非阻塞送出本批回覆後才恢復 guest；新到達事件保存給下一次安全點，不丟棄也不遞迴處理。
    if(replied)channel.pending=channel.transport.poll(0);
#else
    (void)memory;(void)milliseconds;(void)force;
#endif
}
std::uint32_t WindowsServices::transfer_debug_memory(GuestMemory& memory,std::uint32_t pid,std::uint64_t birth,std::uint32_t operation,std::uint64_t address,std::uint64_t size,std::vector<std::uint8_t>& data){
#if defined(__APPLE__)
    if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許跨程序記憶體 I/O");
    if(!debug_memory_ || debug_memory_->owner!=this)return 50;
    if(!pid || !birth || pid==debug_memory_->transport.pid())return 87;
    auto& channel=*debug_memory_;if(channel.next==UINT64_MAX)return 8;
    const auto kind=operation==1?read_memory:operation==2?write_memory:operation==3?flush_memory:operation==4?query_memory:operation==5?protect_memory:operation==6?check_read:operation==7?check_write:operation==8?query_debugger:0U;
    if(!kind || ((operation==1 || operation==2) && size>transfer_limit) || (operation==2 && data.size()!=size) || (operation==5 && data.size()!=4))return 87;
    try {
        debugger::Frame request;request.kind=kind;request.sequence=channel.next++;
        append(request.payload,address);append(request.payload,size);append(request.payload,birth);
        if(operation==2 || operation==5)request.payload.insert(request.payload.end(),data.begin(),data.end());
        const auto peer=channel.transport.connect(pid,birth);
        struct Close {DebugMemoryState& channel;debugger::Peer peer;~Close(){channel.active.erase(peer.connection);channel.disconnected.erase(peer.connection);
            std::erase_if(channel.replies,[&](const auto& item){return item.first.first==peer.connection;});try{channel.transport.close(peer);}catch(const std::system_error&){}}} close{channel,peer};
        channel.active.emplace(peer.connection,request.sequence);
        channel.transport.send(peer,request);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        for(;;){
            poll_handle_transfers(memory,true);
            poll_debug_memory(memory,1);
            const auto identity=process_debugger::query(pid);
            if(identity.error || identity.exited || identity.identity!=birth)return 87;
            if(channel.disconnected.erase(peer.connection))return 87;
            const auto found=channel.replies.find({peer.connection,request.sequence});
            if(found!=channel.replies.end()){
                auto reply=std::move(found->second);channel.replies.erase(found);
                if(reply.kind!=kind || reply.flags!=1)return 13;
                if(reply.error)return reply.payload.empty()?reply.error:13U;
                const auto expected=operation==1?static_cast<std::size_t>(size):operation==4?40U:operation==5?4U:operation==8?8U:0U;
                if(reply.payload.size()!=expected)return 13;
                data=std::move(reply.payload);return 0;
            }
            if(std::chrono::steady_clock::now()>=deadline)return 1460;
        }
    }catch(const std::system_error& error){return transport_error(error);}catch(const std::bad_alloc&){return 8;}catch(const std::invalid_argument&){return 87;}
#else
    (void)memory;(void)pid;(void)birth;(void)operation;(void)address;(void)size;(void)data;return 50;
#endif
}
}
