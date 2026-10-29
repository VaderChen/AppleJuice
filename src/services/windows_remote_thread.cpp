#include "x64arm/windows_services.hpp"
#include "x64arm/thread_context.hpp"
#include "x64arm/security_access.hpp"
#include "x64arm/process_identity.hpp"
#include "x64arm/shared_wake.hpp"
#include "thread_creation_rpc.hpp"
#include "windows_forced_thread_stop.hpp"
#include "monotonic_clock.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <tuple>

namespace x64arm {
namespace rpc=thread_creation_rpc;
struct RemoteThreadState {
    struct Client {
        std::uint32_t pid{},access{},saved_error{};std::uint64_t birth{},sequence{};
        bool inherit{};RemoteThreadOutputs outputs;std::shared_ptr<rpc::Channel> channel;
        ~Client(){if(channel)channel->cancel();}
    };
    struct Server {
        std::shared_ptr<rpc::Channel> channel;std::uint64_t handle{},id{};
        std::array<std::uint64_t,5> request{},result{};std::uint32_t mode{};bool suspended{},replied{};
        std::chrono::steady_clock::time_point deadline;
    };
    using Key=std::tuple<std::uint32_t,std::uint64_t,std::uint64_t>;
    std::map<std::uint64_t,Client> clients;
    std::map<Key,Server> servers;
};
std::uint32_t WindowsServices::begin_remote_thread_creation(GuestMemory& memory,std::uint64_t process,std::uint32_t rights,
    std::uint64_t entry,std::uint64_t parameter,std::uint64_t stack,const GuestThreadBootstrap& bootstrap,RemoteThreadOutputs outputs){
    (void)memory;
    try{
        std::uint32_t pid=0;std::uint64_t birth=0;
        if(const auto error=remote_thread_process_identity(process,rights,pid,birth))return error;
        if(!remote_threads_)remote_threads_=std::make_shared<RemoteThreadState>();
        auto& operations=remote_threads_->clients;
        if(operations.contains(threads_.current) || operations.size()>=1024)return 8;
        const auto mode=x86_?ExecutionMode::X86:ExecutionMode::X64;
        const auto context=bootstrap.context?thread_context::capture(*bootstrap.context,mode,x86_?0x1003fU:0x10001fU):std::vector<std::uint8_t>{};
        if(bootstrap.security_descriptor.size()>rpc::maximum_size-rpc::header_size-context.size())return 8;
        std::vector<std::uint8_t> request(rpc::header_size+bootstrap.security_descriptor.size()+context.size());
        rpc::put(request,0,rpc::signature,8);rpc::put(request,8,1);rpc::put(request,16,x86_?32:64);
        const auto flags=(outputs.suspended?1U:0U)|(bootstrap.skip_thread_attach?2U:0U)|(bootstrap.hidden_from_debugger?4U:0U)|
            (bootstrap.context?8U:0U)|(outputs.default_stack?16U:0U);
        rpc::put(request,20,flags);rpc::put(request,24,bootstrap.access);rpc::put(request,28,bootstrap.security_descriptor.size());
        rpc::put(request,32,entry,8);rpc::put(request,40,parameter,8);rpc::put(request,48,stack,8);
        rpc::put(request,56,bootstrap.stack_low,8);rpc::put(request,64,bootstrap.stack_high,8);rpc::put(request,72,bootstrap.stack_allocation,8);
        rpc::put(request,80,context.size());
        std::copy(bootstrap.security_descriptor.begin(),bootstrap.security_descriptor.end(),request.begin()+rpc::header_size);
        std::copy(context.begin(),context.end(),request.end()-static_cast<std::ptrdiff_t>(context.size()));
        auto channel=rpc::Channel::create(mapping_session(),request);
        auto [position,inserted]=operations.try_emplace(threads_.current);(void)inserted;auto& operation=position->second;
        operation.pid=pid;operation.birth=birth;operation.access=bootstrap.access;operation.inherit=bootstrap.inherit;
        operation.saved_error=last_error_;operation.outputs=std::move(outputs);operation.channel=std::move(channel);
        const auto error=begin_remote_family_request(pid,birth,rpc::family,rpc::create,{request.size(),0,0,0,0},x86_?32:64,
            operation.channel->descriptor(),-1,operation.sequence);
        if(error){operations.erase(position);return error;}
        auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};
        thread.wait.kind=ThreadWaitKind::RemoteThreadCreation;
        threads_.yield=true;notify_concurrency_changed();return 0;
    }catch(const rpc::Error& error){return error.code;}
     catch(const std::bad_alloc&){return 8;}
     catch(const std::invalid_argument&){return 87;}
}
void WindowsServices::cancel_remote_thread_creation(std::uint64_t thread){
    if(!remote_threads_)return;
    const auto found=remote_threads_->clients.find(thread);if(found==remote_threads_->clients.end())return;
    auto& operation=found->second;operation.channel->cancel();
    if(operation.sequence){
        cancel_remote_family_request(operation.pid,operation.sequence);
        (void)queue_remote_family_cancel(operation.pid,operation.birth,rpc::family,operation.sequence);
    }
    remote_threads_->clients.erase(found);if(const auto wake=mapping_wake())wake->notify();
}
bool WindowsServices::finish_remote_thread_creation(GuestState& state,GuestMemory& memory,std::uint64_t owner){
    if(!remote_threads_)return true;
    const auto found=remote_threads_->clients.find(owner);if(found==remote_threads_->clients.end())return true;
    auto& operation=found->second;
    const auto response=poll_remote_family_request(memory,operation.pid,operation.sequence);
    if(!response.ready){
        threads_.items.at(owner).wait.deadline=(runtime_.host_clock?detail::monotonic_ticks():runtime_.clock_ticks)+500000;
        return false;
    }
    const auto outputs=operation.outputs;const auto saved_error=operation.saved_error;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    std::uint32_t error=response.error;std::uint64_t handle=0;
    try{
        if(!error && (response.mode!=(x86_?32U:64U) || !response.values[0] || response.values[0]>UINT32_MAX ||
            !response.values[1] || response.values[1]>UINT32_MAX || !response.values[2] || response.values[3] || response.values[4]))error=13;
        if(!error){
            // 等候期間客體可取消或解除輸出映射；交付前重新探測全部輸出。
            const auto probe=[&](std::uint64_t at,std::size_t size){if(at)memory.check(GuestAddress{at},size,Access::Write);};
            probe(outputs.handle,unit);probe(outputs.client,2*unit);probe(outputs.identifier,4);
            for(const auto& attribute:outputs.attributes){probe(attribute.address,attribute.kind==0x10003?2*unit:unit);probe(attribute.returned,unit);}
            handle=import_created_thread(operation.pid,operation.birth,static_cast<std::uint32_t>(response.values[0]),
                static_cast<std::uint32_t>(response.values[1]),operation.access,operation.inherit);
            if(!handle)throw rpc::Error{8};
            const auto client=[&](std::uint64_t at){memory.store(GuestAddress{at},bits,operation.pid);memory.store(GuestAddress{at+unit},bits,response.values[1]);};
            if(outputs.client)client(outputs.client);
            if(outputs.identifier)memory.store(GuestAddress{outputs.identifier},32,response.values[1]);
            for(const auto& attribute:outputs.attributes){
                if(attribute.kind==0x10003)client(attribute.address);else memory.store(GuestAddress{attribute.address},bits,response.values[2]);
                if(attribute.returned)memory.store(GuestAddress{attribute.returned},bits,attribute.kind==0x10003?2*unit:unit);
            }
            if(outputs.handle)memory.store(GuestAddress{outputs.handle},bits,handle);
            if(!operation.channel->commit())throw rpc::Error{995};
            if(outputs.native && !outputs.handle){close_process(handle);runtime_.handle_flags.erase(handle);handle=0;}
        }
    }catch(const rpc::Error& failure){error=failure.code;}
     catch(const MemoryFault&){error=998;}
     catch(const std::bad_alloc&){error=8;}
     catch(const HostCallFault&){error=13;}
    if(error && handle){close_process(handle);runtime_.handle_flags.erase(handle);handle=0;}
    cancel_remote_thread_creation(owner);
    poll_handle_transfers(memory,true); // 送出交付／取消通知，喚醒目標的既有 descriptor watcher。
    auto& thread=threads_.items.at(owner);
    if(outputs.native){
        const auto status=!error?0U:error==5?0xc0000022U:error==6?0xc0000008U:error==8?0xc0000017U:
            error==50?0xc00000bbU:error==998?0xc0000005U:error==121?0xc00000b5U:error==995?0xc0000120U:
            error==87?0xc000000dU:0xc0000001U;
        thread.last_native_status=status;
        memory.store(GuestAddress{thread.teb+(x86_?0x34:0x68)},32,saved_error);
        if(owner==threads_.current)last_error_=saved_error;
        WindowsCall(state,memory,outputs.arguments).return_to(state,status);
    }else{
        if(error){memory.store(GuestAddress{thread.teb+(x86_?0x34:0x68)},32,error);if(owner==threads_.current)last_error_=error;}
        WindowsCall(state,memory,outputs.arguments).return_to(state,handle);
    }
    return true;
}
std::uint32_t WindowsServices::receive_remote_thread_creation(GuestMemory& memory,std::uint32_t caller,std::uint64_t birth,
    std::uint64_t sequence,std::uint32_t code,std::uint32_t timeout,const std::array<std::uint64_t,5>& values,std::uint32_t mode,int& descriptor){
    const auto saved_error=last_error_;
    struct Restore{std::uint32_t& location;std::uint32_t value;~Restore(){location=value;}} restore{last_error_,saved_error};
    try{
        if(code!=rpc::create)return 50;
        if(mode!=(x86_?32U:64U))return 50;
        if(descriptor<0 || values[1] || values[2] || values[3] || values[4])return 87;
        if(!remote_threads_)remote_threads_=std::make_shared<RemoteThreadState>();
        auto channel=std::make_shared<rpc::Channel>(descriptor,values[0]);descriptor=-1;
        auto& operations=remote_threads_->servers;const RemoteThreadState::Key key{caller,birth,sequence};
        if(const auto found=operations.find(key);found!=operations.end()){
            if(channel->identity()!=found->second.channel->identity() || values!=found->second.request || mode!=found->second.mode)return 13;
            (void)complete_remote_family_request(caller,sequence,birth,channel->phase().load()==2?995U:0U,found->second.result,mode);return 0;
        }
        if(operations.size()>=1024)return 8;
        if(channel->phase().load(std::memory_order_acquire))return 995;
        const auto bytes=channel->read();const auto flags=rpc::get(bytes,20),security_size=rpc::get(bytes,28),context_size=rpc::get(bytes,80);
        if(rpc::get(bytes,0,8)!=rpc::signature || rpc::get(bytes,8)!=1 || rpc::get(bytes,16)!=mode || (flags&~31U) || rpc::get(bytes,84) ||
           security_size>bytes.size()-rpc::header_size || context_size!=bytes.size()-rpc::header_size-security_size ||
           ((flags&8U)?context_size!=thread_context::size(x86_?ExecutionMode::X86:ExecutionMode::X64):context_size!=0))return 13;
        GuestThreadBootstrap bootstrap;bootstrap.access=0x1fffffU;
        bootstrap.skip_thread_attach=(flags&2U)!=0;bootstrap.hidden_from_debugger=(flags&4U)!=0;
        bootstrap.security_descriptor.assign(bytes.begin()+rpc::header_size,bytes.begin()+rpc::header_size+static_cast<std::ptrdiff_t>(security_size));
        (void)security::parse_relative_descriptor(bootstrap.security_descriptor);
        GuestState initial;
        initial.segments=x86_?std::array<std::uint16_t,6>{0x23,0x1b,0x23,0x23,0x3b,0}:
                             std::array<std::uint16_t,6>{0x2b,0x33,0x2b,0x2b,0x53,0x2b};
        if(flags&8U){
            GuestState context;context.segments=initial.segments;
            bootstrap.context=thread_context::restore(context,x86_?ExecutionMode::X86:ExecutionMode::X64,std::span(bytes).last(context_size));
            bootstrap.stack_low=rpc::get(bytes,56,8);bootstrap.stack_high=rpc::get(bytes,64,8);bootstrap.stack_allocation=rpc::get(bytes,72,8);
            const auto sp=bootstrap.context->reg(Gpr::Rsp);
            if(!bootstrap.stack_allocation || bootstrap.stack_allocation>bootstrap.stack_low || bootstrap.stack_low>=bootstrap.stack_high ||
               sp<bootstrap.stack_low || sp>=bootstrap.stack_high || bootstrap.stack_high>user_address_limit())return 87;
            memory.check(GuestAddress{sp},x86_?4:8,Access::Write);
        }
        auto requested=rpc::get(bytes,48,8);if(flags&16U)requested=std::max(requested,runtime_.image.stack_size);
        auto [position,inserted]=operations.try_emplace(key);(void)inserted;auto& operation=position->second;
        operation.channel=std::move(channel);operation.request=values;operation.mode=mode;operation.suspended=(flags&1U)!=0;
        operation.deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout);
        try{
            operation.handle=create_guest_thread(initial,memory,rpc::get(bytes,32,8),rpc::get(bytes,40,8),requested,4,&bootstrap);
            if(!operation.handle)throw rpc::Error{last_error_?last_error_:8U};
            operation.id=thread_id(operation.handle);const auto& thread=threads_.items.at(operation.id);
            operation.result={operation.id,thread.public_id,thread.teb,0,0};
        }catch(...){operations.erase(position);throw;}
        operation.replied=!complete_remote_family_request(caller,sequence,birth,0,operation.result,mode);
        threads_.yield=true;notify_concurrency_changed();
        return 0;
    }catch(const rpc::Error& error){return error.code;}
     catch(const SecurityDescriptorError& error){return error.code;}
     catch(const MemoryFault&){return 998;}
     catch(const std::bad_alloc&){return 8;}
     catch(const std::invalid_argument&){return 87;}
     catch(const HostCallFault& error){
        if(std::getenv("X64ARM_TRACE_THREAD_CREATION"))std::fprintf(stderr,"remote_thread_create_contract pid=%u caller=%u detail=%s\n",process_id(),caller,error.what());
        return 50;
     }
}
void WindowsServices::poll_remote_thread_creations(GuestMemory& memory){
    if(!remote_threads_ || remote_threads_->servers.empty())return;
    const auto now=std::chrono::steady_clock::now();bool changed=false;
    for(auto it=remote_threads_->servers.begin();it!=remote_threads_->servers.end();){
        auto& operation=it->second;const auto [caller,birth,sequence]=it->first;(void)sequence;
        if(operation.handle){
            if(now>=operation.deadline || process_identity::query(caller).gone({caller,birth}))operation.channel->cancel();
            const auto phase=operation.channel->phase().load(std::memory_order_acquire);
            if(!phase && !operation.replied)operation.replied=!complete_remote_family_request(caller,sequence,birth,0,operation.result,operation.mode);
            if(phase){
                auto& thread=threads_.items.at(operation.id);
                if(phase==1){if(!operation.suspended && thread.suspended)--thread.suspended;}
                else if(awaiting_initial_thread_)discard_unstarted_thread(memory,operation.id,995);
                else (void)detail::prepare_forced_thread_stop(thread,995,GuestAddress{callback_return_.value+0xa0});
                runtime_.process_thread_handles.erase(operation.handle);runtime_.thread_handle_access.erase(operation.handle);runtime_.handle_flags.erase(operation.handle);
                operation.handle=0;changed=true;
            }
        }
        if(!operation.handle && now>=operation.deadline+std::chrono::seconds(60))it=remote_threads_->servers.erase(it);else ++it;
    }
    if(changed){threads_.yield=true;notify_concurrency_changed();}
}
bool WindowsServices::remote_thread_creations_waiting()const{
    return remote_threads_ && std::any_of(remote_threads_->servers.begin(),remote_threads_->servers.end(),[](const auto& item){return item.second.handle!=0;});
}
}
