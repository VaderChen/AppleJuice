#include "x64arm/windows_services.hpp"
#include "x64arm/native_v8.hpp"
namespace x64arm {
namespace nv=native_v8;
namespace {
constexpr std::uint32_t request_size=72;
void finish(GuestMemory& memory,std::uint64_t request,nv::Status status,std::uint32_t result,const std::vector<std::uint8_t>& data){
    const auto capacity=memory.load(GuestAddress{request+40},32);const auto output=memory.load(GuestAddress{request+32},64);
    const bool fits=data.size()<=capacity;
    if(fits && !data.empty())memory.write(GuestAddress{output},data);
    memory.store(GuestAddress{request+44},32,data.size());memory.store(GuestAddress{request+48},32,result);
    memory.store(GuestAddress{request+52},32,static_cast<std::uint32_t>(fits?status:nv::Status::Capacity));
}
}
std::optional<std::uint32_t> WindowsServices::invoke_native_v8(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& descriptor){
    const WindowsCall call(state,memory,descriptor);const auto request=call(0);
    memory.check(GuestAddress{request},request_size,Access::Read);memory.check(GuestAddress{request},request_size,Access::Write);
    const auto capacity=memory.load(GuestAddress{request+40},32),length=memory.load(GuestAddress{request+24},32);
    if(capacity>nv::limit || length>nv::limit)throw HostCallFault("V8 模組緩衝區超過上限");
    memory.check(GuestAddress{memory.load(GuestAddress{request+32},64)},static_cast<std::size_t>(capacity),Access::Write);
    nv::Packet packet;packet.header.op=static_cast<nv::Op>(memory.load(GuestAddress{request},32));
    packet.header.isolate=static_cast<std::uint32_t>(memory.load(GuestAddress{request+4},32));
    packet.header.handle=static_cast<std::uint32_t>(memory.load(GuestAddress{request+8},32));
    packet.header.aux=static_cast<std::uint32_t>(memory.load(GuestAddress{request+12},32));
    packet.data.resize(static_cast<std::size_t>(length));memory.read(GuestAddress{memory.load(GuestAddress{request+16},64)},packet.data);
    if(native_v8_aborted_){finish(memory,request,nv::Status::Invalid,0,{});call.return_to(state,static_cast<std::uint32_t>(nv::Status::Invalid));return {};}
    if(!native_v8_)native_v8_=std::make_shared<NativeV8>();
    auto& module=*native_v8_;
    if(packet.header.op!=nv::Op::Version && packet.header.op!=nv::Op::Create){const auto owner=module.owners.find(packet.header.isolate);
        if(owner==module.owners.end() || owner->second!=threads_.current){finish(memory,request,owner==module.owners.end()?nv::Status::Invalid:nv::Status::Thread,0,{});call.return_to(state,static_cast<std::uint32_t>(owner==module.owners.end()?nv::Status::Invalid:nv::Status::Thread));return {};}}
    if(!module.pending.empty() && std::any_of(module.pending.begin(),module.pending.end(),[&](const auto& p){return p.second.owner!=threads_.current;})){
        finish(memory,request,nv::Status::Busy,0,{});call.return_to(state,static_cast<std::uint32_t>(nv::Status::Busy));return {};
    }
    std::uint32_t binding=0;
    if(packet.header.op==nv::Op::Function){const auto target=memory.load(GuestAddress{request+56},64);memory.check(GuestAddress{target},1,Access::Execute);
        if(!module.next_binding)throw HostCallFault("V8 callback token 已耗盡");binding=module.next_binding++;
        module.bindings.emplace(binding,NativeV8::Binding{target,memory.load(GuestAddress{request+64},64),threads_.current,packet.header.isolate});packet.header.aux=binding;
    }
    packet.header.sequence=module.sequence();module.pending.emplace(packet.header.sequence,NativeV8::Pending{request,0,threads_.current,packet.header.sequence,packet.header.isolate,binding});
    ++module.calls;module.send(packet);return advance_native_v8(state,memory,packet.header.sequence);
}
std::optional<std::uint32_t> WindowsServices::advance_native_v8(GuestState& state,GuestMemory& memory,std::uint32_t sequence){
    auto& module=*native_v8_;auto message=module.receive();const auto found=module.pending.find(sequence);
    if(found==module.pending.end())throw HostCallFault("V8 缺少模組操作狀態");auto& pending=found->second;
    if(message.header.kind==nv::Kind::Callback){
        const auto binding=module.bindings.find(message.header.handle);
        if(binding==module.bindings.end() || binding->second.owner!=threads_.current || binding->second.isolate!=message.header.isolate || message.data.size()%4 || message.data.empty() || message.data.size()/4>1025)
            throw HostCallFault("V8 callback 不符合模組契約");
        const auto scratch=allocate_heap(memory,message.data.size()+8);pending.scratch=scratch;
        memory.store(GuestAddress{scratch},32,message.header.sequence);memory.store(GuestAddress{scratch+4},32,message.header.isolate);memory.write(GuestAddress{scratch+8},message.data);
        ++module.callbacks;start_callbacks(state,memory,{{binding->second.target,{message.header.isolate,message.data.size()/4,scratch+8,binding->second.userdata}}},Continuation::NativeV8Operation,sequence);return {};
    }
    if(message.header.kind!=nv::Kind::Response || message.header.sequence!=sequence)throw HostCallFault("V8 模組請求回傳順序不符");
    const auto result=message.header.op==nv::Op::Create?message.header.isolate:message.header.handle;
    if(message.header.status==nv::Status::Ok){
        if(message.header.op==nv::Op::Create)module.owners.emplace(result,threads_.current);
        if(message.header.op==nv::Op::Dispose){module.owners.erase(message.header.isolate);std::erase_if(module.bindings,[&](const auto& b){return b.second.isolate==message.header.isolate;});}
    }else if(pending.binding)module.bindings.erase(pending.binding);
    finish(memory,pending.request,message.header.status,result,message.data);
    const auto status=memory.load(GuestAddress{pending.request+52},32);module.pending.erase(found);
    complete_call(state,memory,status);return {};
}
std::optional<std::uint32_t> WindowsServices::resume_native_v8(GuestState& state,GuestMemory& memory,std::uint32_t sequence,std::uint32_t result){
    if(!native_v8_){complete_call(state,memory,static_cast<std::uint32_t>(nv::Status::Invalid));return {};}
    auto& module=*native_v8_;auto& pending=module.pending.at(sequence);if(!pending.scratch)throw HostCallFault("V8 callback scratch 缺失");
    nv::Packet packet;packet.header.kind=nv::Kind::CallbackReturn;packet.header.sequence=static_cast<std::uint32_t>(memory.load(GuestAddress{pending.scratch},32));
    packet.header.isolate=static_cast<std::uint32_t>(memory.load(GuestAddress{pending.scratch+4},32));packet.header.handle=result;
    if(!result)packet.header.status=nv::Status::Exception;
    free_heap(memory,pending.scratch,state);pending.scratch=0;module.send(std::move(packet));return advance_native_v8(state,memory,sequence);
}
void WindowsServices::discard_native_v8(GuestMemory& memory,std::uint32_t sequence){
    if(!native_v8_)return;auto& module=*native_v8_;const auto found=module.pending.find(sequence);if(found==module.pending.end())return;
    for(const auto& operation:module.pending)if(operation.second.scratch)free_heap(memory,operation.second.scratch,GuestState{});
    // 客體 SEH 非局部跳轉不能留著等待中的原生棧；終止此後端並撤銷所有 Isolate。
    native_v8_.reset();native_v8_aborted_=true;
}
}
