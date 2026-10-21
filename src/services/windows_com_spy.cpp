#include "x64arm/windows_services.hpp"
#include <algorithm>

namespace x64arm {
// IInitializeSpy：每個執行緒的 spy 依註冊反序收到 Pre／Post Initialize 與 Uninitialize 通知；
// 實際的 CoInitialize(Ex)／CoUninitialize 仍經原本的服務流程執行，PostInitialize 可改寫回傳的 HRESULT。
namespace {
constexpr std::array<std::uint8_t,16> initialize_spy_iid{0x34,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
std::uint64_t spy_method(const GuestMemory& memory,std::uint64_t object,unsigned slot,unsigned unit){
    const auto bits=static_cast<std::uint8_t>(unit*8);const auto table=memory.load(GuestAddress{object},bits);
    if(!table)throw MemoryFault(GuestAddress{object},Access::Read,unit,"IInitializeSpy vtable 為空");
    return memory.load(GuestAddress{table+std::uint64_t{slot}*unit},bits);
}
}
std::span<const WindowsServiceDescriptor> com_spy_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ole32.dll","CoRegisterInitializeSpy",2},{"combase.dll","CoRegisterInitializeSpy",2},
        {"ole32.dll","CoRevokeInitializeSpy",1,1},{"combase.dll","CoRevokeInitializeSpy",1,1}
    };return table;
}
std::uint32_t WindowsServices::com_apartment_references(std::uint64_t thread)const{
    const auto found=gui_.com_initializations.find(thread);return found==gui_.com_initializations.end()?0U:found->second>>4;
}
std::optional<std::uint32_t> WindowsServices::invoke_com_spy(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint32_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    try{
        if(service.symbol=="CoRegisterInitializeSpy"){
            if(!arg(0) || !arg(1))return finish(0x80070057U);
            memory.check(GuestAddress{arg(1)},8,Access::Write);
            const auto query=spy_method(memory,arg(0),0,unit);
            const auto scratch=allocate_heap(memory,16+unit);if(!scratch)return finish(0x8007000eU);
            memory.write(GuestAddress{scratch},initialize_spy_iid);memory.store(GuestAddress{scratch+16},bits,0);
            ComSpyOperation operation;operation.phase=ComSpyOperation::Phase::Register;operation.scratch=scratch;operation.output=arg(1);operation.thread=threads_.current;
            const auto id=next_com_spy_operation_++;com_spy_operations_.emplace(id,std::move(operation));
            start_callbacks(state,memory,{{query,{arg(0),scratch,scratch+16}}},Continuation::ComSpyOperation,id);return std::nullopt;
        }
        // CoRevokeInitializeSpy：ULARGE_INTEGER 依值傳入；只能由註冊的執行緒撤銷。
        const auto cookie=arg(0);
        const auto found=com_spies_.find(threads_.current);if(found==com_spies_.end())return finish(0x80070057U);
        const auto entry=std::find_if(found->second.begin(),found->second.end(),[&](const auto& item){return item.first==cookie;});
        if(entry==found->second.end())return finish(0x80070057U);
        const auto spy=entry->second;found->second.erase(entry);if(found->second.empty())com_spies_.erase(found);
        start_callbacks(state,memory,{{spy_method(memory,spy,2,unit),{spy}}},Continuation::Return,0);return std::nullopt;
    }catch(const MemoryFault&){return finish(0x80004003U);}catch(const std::bad_alloc&){return finish(0x8007000eU);}
}
bool WindowsServices::begin_initialize_spies(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    // 由 spy 流程轉回原本服務的那一次呼叫不再通知。
    if(com_spy_bypass_.erase(threads_.current))return false;
    const auto found=com_spies_.find(threads_.current);if(found==com_spies_.end() || found->second.empty())return false;
    const WindowsCall arg(state,memory,service);const unsigned unit=x86_?4:8;
    ComSpyOperation operation;operation.thread=threads_.current;operation.uninitialize=service.symbol=="CoUninitialize";
    operation.extended=service.symbol=="CoInitializeEx";operation.reserved=operation.uninitialize?0:arg(0);
    operation.mode=operation.extended?static_cast<std::uint32_t>(arg(1)):2U;
    for(auto it=found->second.rbegin();it!=found->second.rend();++it)operation.spies.push_back(it->second);
    const auto references=com_apartment_references(threads_.current);
    std::vector<GuestCallback> callbacks;
    for(const auto spy:operation.spies){
        if(operation.uninitialize)callbacks.push_back({spy_method(memory,spy,5,unit),{spy,references}});
        else callbacks.push_back({spy_method(memory,spy,3,unit),{spy,operation.mode,references}});
    }
    operation.phase=ComSpyOperation::Phase::Before;
    const auto id=next_com_spy_operation_++;com_spy_operations_.emplace(id,std::move(operation));
    start_callbacks(state,memory,std::move(callbacks),Continuation::ComSpyOperation,id);return true;
}
std::optional<std::uint32_t> WindowsServices::resume_com_spy(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t value){
    const auto found=com_spy_operations_.find(id);if(found==com_spy_operations_.end()){complete_call(state,memory,0);return std::nullopt;}
    auto& operation=found->second;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    using Phase=ComSpyOperation::Phase;
    if(operation.phase==Phase::Register){
        const auto status=static_cast<std::uint32_t>(value);const auto spy=memory.load(GuestAddress{operation.scratch+16},bits);
        (void)free_heap(memory,operation.scratch,state);const auto output=operation.output,thread=operation.thread;com_spy_operations_.erase(found);
        if(status){complete_call(state,memory,status);return std::nullopt;}
        if(!spy){complete_call(state,memory,0x80004002U);return std::nullopt;}
        const auto cookie=(std::uint64_t{thread_identifier(thread)}<<32)|(next_com_spy_cookie_++&0xffffffffU);
        com_spies_[thread].emplace_back(cookie,spy);memory.store(GuestAddress{output},64,cookie);
        complete_call(state,memory,0);return std::nullopt;
    }
    if(operation.phase==Phase::Before){
        // 轉回原本的 COM 服務，回到這裡後再送出 Post 通知。
        operation.phase=Phase::Call;com_spy_bypass_.insert(operation.thread);
        if(operation.uninitialize)start_callbacks(state,memory,{{resolve("ole32.dll","CoUninitialize").value,{}}},Continuation::ComSpyOperation,id);
        else if(operation.extended)start_callbacks(state,memory,{{resolve("ole32.dll","CoInitializeEx").value,{operation.reserved,operation.mode}}},Continuation::ComSpyOperation,id);
        else start_callbacks(state,memory,{{resolve("ole32.dll","CoInitialize").value,{operation.reserved}}},Continuation::ComSpyOperation,id);
        return std::nullopt;
    }
    if(operation.phase==Phase::Call){
        com_spy_bypass_.erase(operation.thread);
        operation.status=static_cast<std::uint32_t>(value);operation.phase=Phase::After;operation.index=0;
        const auto references=com_apartment_references(operation.thread);
        if(operation.uninitialize){
            std::vector<GuestCallback> callbacks;for(const auto spy:operation.spies)callbacks.push_back({spy_method(memory,spy,6,unit),{spy,references}});
            operation.index=operation.spies.size();start_callbacks(state,memory,std::move(callbacks),Continuation::ComSpyOperation,id);return std::nullopt;
        }
        const auto spy=operation.spies.front();operation.index=1;
        start_callbacks(state,memory,{{spy_method(memory,spy,4,unit),{spy,operation.status,operation.mode,references}}},Continuation::ComSpyOperation,id);return std::nullopt;
    }
    // PostInitialize 的回傳值成為下一個 spy 的 hrCoInit 與最後的回傳值。
    if(!operation.uninitialize)operation.status=static_cast<std::uint32_t>(value);
    if(operation.index<operation.spies.size()){
        const auto spy=operation.spies[operation.index++];const auto references=com_apartment_references(operation.thread);
        start_callbacks(state,memory,{{spy_method(memory,spy,4,unit),{spy,operation.status,operation.mode,references}}},Continuation::ComSpyOperation,id);return std::nullopt;
    }
    const auto status=operation.uninitialize?0U:operation.status;com_spy_operations_.erase(found);
    complete_call(state,memory,status);return std::nullopt;
}
void WindowsServices::discard_com_spy(GuestState& state,GuestMemory& memory,std::uint64_t id){
    const auto found=com_spy_operations_.find(id);if(found==com_spy_operations_.end())return;
    if(found->second.phase==ComSpyOperation::Phase::Register && found->second.scratch)(void)free_heap(memory,found->second.scratch,state);
    com_spy_bypass_.erase(found->second.thread);com_spy_operations_.erase(found);
}
}
