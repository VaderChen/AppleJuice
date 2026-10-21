#include "x64arm/windows_services.hpp"
#include "x64arm/gui_rpc.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> gui_rpc_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{{"internal-gui-rpc","Invoke",4}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_gui_rpc(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using namespace gui_rpc;
    WindowsCall call(state,memory,service);
    auto found=std::find_if(gui_.sent_messages.begin(),gui_.sent_messages.end(),[&](const auto& entry){
        const auto& request=entry.second;
        return request.rpc && request.sender==threads_.current && request.sender_stack==state.reg(Gpr::Rsp) && request.sender_rip==state.rip.value;
    });
    const auto finish=[&](std::uint64_t output,std::uint64_t error_pointer,std::uint64_t value,std::uint32_t error){
        // ULONG／HRESULT／64 位元 callback 結果各自保持原值，錯誤另存 DWORD。
        if(output)memory.store(GuestAddress{output},64,value);
        if(error_pointer)memory.store(GuestAddress{error_pointer},32,error);
        call.return_to(state,value);threads_.items.at(threads_.current).wait={};
    };
    if(found==gui_.sent_messages.end()){
        const auto output=call(2),error_pointer=call(3),packet=call(1),receiver_id=call(0);
        if(output)memory.check(GuestAddress{output},8,Access::Write);
        if(error_pointer)memory.check(GuestAddress{error_pointer},4,Access::Write);
        if(!packet || !error_pointer){finish(output,error_pointer,0,invalid_argument);return std::nullopt;}
        const auto receiver=threads_.items.find(receiver_id);
        if(receiver==threads_.items.end() || receiver->second.status==ThreadStatus::Exited){finish(output,error_pointer,0,disconnected);return std::nullopt;}
        memory.check(GuestAddress{packet},size,Access::Read);
        GuestCallback callback;callback.target=memory.load(GuestAddress{packet+target},64);
        const auto count_value=memory.load(GuestAddress{packet+count},32),flag_value=memory.load(GuestAddress{packet+gui_rpc::flags},32);
        callback.wide_arguments=static_cast<std::uint32_t>(memory.load(GuestAddress{packet+wide_arguments},32));
        callback.x86_register_arguments=static_cast<unsigned>(memory.load(GuestAddress{packet+register_arguments},32));
        callback.wide_result=(flag_value&wide_result)!=0;
        if(count_value>16 || !callback.target || (x86_ && callback.target>UINT32_MAX) || (flag_value&~wide_result) ||
           (callback.wide_arguments>>count_value) || callback.x86_register_arguments>2 || callback.x86_register_arguments>count_value ||
           (callback.wide_arguments&((1U<<callback.x86_register_arguments)-1U))){finish(output,error_pointer,0,invalid_argument);return std::nullopt;}
        if(!handles(GuestAddress{callback.target}))memory.check(GuestAddress{callback.target},1,Access::Execute);
        for(unsigned i=0;i<count_value;++i)callback.arguments.push_back(memory.load(GuestAddress{packet+arguments+i*8U},64));
        if(gui_.sent_messages.size()>=8192){finish(output,error_pointer,0,out_of_memory);return std::nullopt;}
        GuiSentMessage request;request.sender=threads_.current;request.receiver=receiver_id;
        request.sender_stack=state.reg(Gpr::Rsp);request.sender_rip=state.rip.value;
        request.result_pointer=output;request.rpc_error_pointer=error_pointer;request.rpc=std::move(callback);
        found=gui_.sent_messages.emplace(gui_.next_object++,std::move(request)).first;
    }
    if(found->second.done){
        const auto request=found->second;gui_.sent_messages.erase(found);
        const auto error=request.error==1400?disconnected:request.error?(0x80070000U|(request.error&0xffffU)):0U;
        finish(request.result_pointer,request.rpc_error_pointer,error?0:request.result,error);return std::nullopt;
    }
    // 阻塞的 STA 仍處理反向 callback，讓 IDataObject::GetData 可回到資料來源執行緒。
    // 沿用 sent-message 管線的 callback unwind、模態續接與執行緒終止清理。
    const auto token=found->first;
    if(dispatch_gui_send(state,memory,0,false))return std::nullopt;
    auto& sender=threads_.items.at(threads_.current);sender.wait={};sender.wait.kind=ThreadWaitKind::GuiSend;
    sender.wait.handles={token};sender.status=ThreadStatus::Waiting;schedule(state,memory);return std::nullopt;
}
}
