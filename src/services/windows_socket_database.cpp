#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "socket_database.hpp"
#include <bit>
#include <system_error>

namespace x64arm {
NetworkState::~NetworkState(){for(auto& [thread,job]:resolvers){(void)thread;job->cancelled.store(true);}for(auto& [handle,lookup]:pending_lookups){(void)handle;lookup.job->cancelled.store(true);}}
void WindowsServices::cancel_socket_lookups(){for(auto& [thread,job]:runtime_.network.resolvers){(void)thread;job->cancelled.store(true);}runtime_.network.resolvers.clear();for(auto& [handle,lookup]:runtime_.network.pending_lookups){(void)handle;lookup.job->cancelled.store(true);}runtime_.network.pending_lookups.clear();}
void WindowsServices::poll_socket_lookups(GuestMemory& memory){
    for(auto it=runtime_.network.resolvers.begin();it!=runtime_.network.resolvers.end();){
        const auto thread=threads_.items.find(it->first);
        if(thread==threads_.items.end() || thread->second.status==ThreadStatus::Exited){it->second->cancelled.store(true);it=runtime_.network.resolvers.erase(it);}else ++it;
    }
    auto& pending=runtime_.network.pending_lookups;
    for(auto item=pending.begin();item!=pending.end();){
        auto& request=item->second;if(!request.job->ready.load(std::memory_order_acquire)){++item;continue;}
        if(!gui_.windows.contains(request.window)){item=pending.erase(item);continue;}
        if(gui_.queue.size()>=10000){++item;continue;}
        std::uint32_t error=request.job->result.error;std::size_t length=0;
        try{if(!error){const auto bytes=socket_database::pack(request.job->result,request.buffer,x86_);length=bytes.size();if(length>request.capacity)error=10055;else memory.write(GuestAddress{request.buffer},bytes);}}
        catch(const MemoryFault&){error=10014;}catch(const std::bad_alloc&){error=10055;}catch(const std::length_error&){error=10055;}
        enqueue_gui_message({request.window,item->first,(static_cast<std::uint64_t>(error)<<16)|(length&65535U),request.message});item=pending.erase(item);
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_socket_database(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using namespace socket_database;const WindowsCall arg(state,memory,service);const auto name=service.symbol;auto& network=runtime_.network;
    const bool async=name.starts_with("WSAAsyncGet"),cancel=name=="WSACancelAsyncRequest";
    const auto finish=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto failed=[&](std::uint32_t error){last_error_=error;return finish(cancel?UINT32_MAX:0);};
    if(!network.startups)return failed(10093);
    if(cancel){const auto found=network.pending_lookups.find(arg(0));if(found==network.pending_lookups.end())return failed(10022);found->second.job->cancelled.store(true);network.pending_lookups.erase(found);return finish(0);}
    if(!network.external_io)throw HostCallFault("compare 不執行無法重播的宿主名稱查詢");
    try{
        Query query;const unsigned offset=async?2:0;
        if(name.ends_with("HostByAddr") || name=="gethostbyaddr")query.kind=Kind::HostAddress;
        else if(name.ends_with("HostByName") || name=="gethostbyname")query.kind=Kind::HostName;
        else if(name.ends_with("ProtoByName") || name=="getprotobyname")query.kind=Kind::ProtocolName;
        else if(name.ends_with("ProtoByNumber") || name=="getprotobynumber")query.kind=Kind::ProtocolNumber;
        else if(name.ends_with("ServByName") || name=="getservbyname")query.kind=Kind::ServiceName;
        else query.kind=Kind::ServicePort;
        const bool host=query.kind==Kind::HostName || query.kind==Kind::HostAddress,protocol=query.kind==Kind::ProtocolName || query.kind==Kind::ProtocolNumber;
        if(query.kind==Kind::HostAddress){query.family=static_cast<std::uint32_t>(arg(offset+2));const auto length=query.family==23?16U:4U;if(query.family!=2 && query.family!=23)return failed(10047);if(arg(offset+1)!=length)return failed(query.family==23?10022:10014);if(!arg(offset))return failed(10014);query.address.resize(length);memory.read(GuestAddress{arg(offset)},query.address);}
        else if(query.kind==Kind::ProtocolNumber || query.kind==Kind::ServicePort)query.number=static_cast<std::uint32_t>(arg(offset));
        else{if(!arg(offset) && query.kind!=Kind::HostName)return failed(10014);query.name=arg(offset)?read_ansi(memory,arg(offset),4096):std::string{};}
        if(!host && !protocol){query.has_protocol=arg(offset+1)!=0;if(query.has_protocol)query.protocol=read_ansi(memory,arg(offset+1),32);}
        if(async){
            const unsigned output=query.kind==Kind::HostAddress?5U:protocol || host?3U:4U;
            const auto capacity=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(output+1)));
            if(!gui_.windows.contains(arg(0)) || !arg(1) || arg(1)>65535 || capacity<0)return failed(10022);
            if(capacity>65535)return failed(10055);if(!arg(output))return failed(10014);memory.check(GuestAddress{arg(output)},static_cast<std::uint32_t>(capacity),Access::Write);
            if(network.pending_lookups.size()>=64)return failed(10035);
            const auto job=submit(std::move(query));if(!job)return failed(10035);const auto handle=network.next_lookup++;
            try{network.pending_lookups.emplace(handle,PendingSocketLookup{arg(0),arg(output),static_cast<std::uint32_t>(arg(1)),static_cast<std::uint32_t>(capacity),job});}catch(...){job->cancelled.store(true);throw;}
            return finish(handle);
        }
        const auto result=lookup(query);if(result.error)return failed(result.error);
        const auto slot=threads_.current+(host?UINT64_C(0x200000000):protocol?UINT64_C(0x300000000):0);
        auto& base=network.service_buffers[slot];if(!base)base=allocate_heap(memory,65536);if(!base)return failed(10055);const auto bytes=pack(result,base,x86_);memory.write(GuestAddress{base},bytes);return finish(base);
    }catch(const MemoryFault&){return failed(10014);}catch(const std::bad_alloc&){return failed(10055);}catch(const std::length_error&){return failed(10055);}catch(const std::system_error&){return failed(10035);}
}
}
