#include "x64arm/windows_services.hpp"
#include <algorithm>

namespace x64arm {
namespace {
bool valid_frame(const GuestMemory& memory,const GuestState& state,std::uint32_t frame){
    const auto low=memory.load(GuestAddress{state.fs_base+8},32),high=memory.load(GuestAddress{state.fs_base+4},32);
    if((frame&3U) || frame<low || frame>high || high-frame<8)return false;
    try{memory.check(GuestAddress{frame},8,Access::Read);return true;}catch(const MemoryFault&){return false;}
}
}
void WindowsServices::leave_x86_exception_handler(GuestMemory& memory,ExceptionDispatch& dispatch){
    if(dispatch.x86_guard && memory.load(GuestAddress{dispatch.original.fs_base},32)==dispatch.x86_guard)
        memory.store(GuestAddress{dispatch.original.fs_base},32,dispatch.x86_previous);
    dispatch.x86_guard=0;
}
void WindowsServices::prune_x86_exception_callbacks(GuestState& state,GuestMemory& memory){
    if(!x86_ || state.rip==callback_return_ || state.rip==exception_entry_)return;
    while(!runtime_.callbacks.empty()){
        const auto& callback=runtime_.callbacks.back();
        if(callback.continuation!=Continuation::Exception || state.reg(Gpr::Rsp)<callback.saved.reg(Gpr::Rsp))break;
        const auto depth=callback.exception_depth;
        // x86 personality 可在 RtlUnwind 後直接恢復 ESP 並跳往 catch；被跨越的主機回呼一併丟棄。
        if(!depth || depth>exceptions_.stack.size())throw HostCallFault("Win32 SEH callback 深度不合法");
        leave_x86_exception_handler(memory,exceptions_.stack[depth-1]);
        discard_callbacks(runtime_.callbacks.size()-1,state,memory);exceptions_.stack.resize(depth-1);
    }
}
std::optional<std::uint32_t> WindowsServices::advance_x86_exception(GuestState& state,GuestMemory& memory){
    auto& dispatch=exceptions_.stack.back();const bool unwind=dispatch.stage==ExceptionStage::Unwind;
    if(!unwind && (runtime_.user_mode_exception_policy&1U) && dispatch.user_callback_depth==SIZE_MAX)
        for(std::size_t i=runtime_.callbacks.size();i-->0;){const auto& frame=runtime_.callbacks[i];
            if(frame.index<frame.callbacks.size() && frame.callbacks[frame.index].user_mode_callback){dispatch.user_callback_depth=i;break;}}
    if(!unwind && !dispatch.etw_thread)for(std::size_t i=runtime_.callbacks.size();i-->0;)if(runtime_.callbacks[i].continuation==Continuation::EtwProcess){
        const auto& frame=runtime_.callbacks[i];dispatch.etw_thread=frame.value;dispatch.etw_callback_depth=i;dispatch.etw_exception_depth=frame.exception_depth;dispatch.etw_seh_boundary=frame.etw_seh_boundary;break;}
    if(!unwind && dispatch.protected_callback_depth==SIZE_MAX)for(std::size_t i=runtime_.callbacks.size();i-->0;)if(callback_protects_exception(runtime_.callbacks[i],dispatch.code)){dispatch.protected_callback_depth=i;break;}
    if(!unwind && dispatch.protected_callback_depth!=SIZE_MAX){
        if((dispatch.etw_thread && dispatch.etw_callback_depth>dispatch.protected_callback_depth) || (dispatch.user_callback_depth!=SIZE_MAX && dispatch.user_callback_depth>dispatch.protected_callback_depth))dispatch.protected_callback_depth=SIZE_MAX;
        else{dispatch.etw_thread=0;dispatch.user_callback_depth=SIZE_MAX;}
    }
    while(dispatch.x86_next!=UINT32_MAX && (!unwind || dispatch.x86_next!=dispatch.x86_target)){
        if(!unwind && dispatch.user_callback_depth!=SIZE_MAX && dispatch.x86_next==runtime_.callbacks[dispatch.user_callback_depth].user_seh_boundary)break;
        if(!unwind && dispatch.etw_thread && dispatch.x86_next==dispatch.etw_seh_boundary)break;
        if(!unwind && dispatch.protected_callback_depth!=SIZE_MAX && dispatch.x86_next==runtime_.callbacks[dispatch.protected_callback_depth].user_seh_boundary)break;
        const auto frame=dispatch.x86_next;
        if(++dispatch.x86_steps>256)throw HostCallFault("Win32 SEH chain 超過 256 個框架");
        if(!valid_frame(memory,dispatch.original,frame)){
            if(unwind)throw HostCallFault("Win32 unwind 堆疊框架不合法");
            dispatch.flags|=8;memory.store(GuestAddress{dispatch.record+4},32,dispatch.flags);dispatch.stage=ExceptionStage::Unhandled;state.rip=exception_entry_;return std::nullopt;
        }
        const auto next=static_cast<std::uint32_t>(memory.load(GuestAddress{frame},32));
        if(next!=UINT32_MAX && next<=frame)throw HostCallFault("Win32 SEH chain 未向上推進");
        if(unwind && dispatch.x86_target && frame>dispatch.x86_target)throw HostCallFault("Win32 unwind 目標不在 SEH chain");
        const auto handler=memory.load(GuestAddress{frame+4},32);
        if(handler==callback_return_.value+0x90){
            const auto owner=std::find_if(exceptions_.stack.begin(),exceptions_.stack.end(),[&](const auto& item){return item.x86_guard==frame;});
            if(owner==exceptions_.stack.end())throw HostCallFault("Win32 SEH 內部框架來源不合法");
            if(unwind)memory.store(GuestAddress{dispatch.original.fs_base},32,next);
            else {dispatch.flags|=0x10;dispatch.x86_nested=std::max(dispatch.x86_nested,owner->x86_frame);memory.store(GuestAddress{dispatch.record+4},32,dispatch.flags);}
            dispatch.x86_next=next;continue;
        }
        if(!handles(GuestAddress{handler}))memory.check(GuestAddress{handler},1,Access::Execute);
        dispatch.x86_frame=frame;dispatch.x86_next=next;
        memory.store(GuestAddress{dispatch.dispatcher},32,0);
        state=dispatch.original;
        // 與 OS 的 nested exception registration 同用途；僅供 SEH walker 使用，不是客體 CPU 指令。
        const auto guard=static_cast<std::uint32_t>((state.reg(Gpr::Rsp)-128)&~UINT64_C(3));
        if(!valid_frame(memory,state,guard))throw HostCallFault("Win32 SEH handler 堆疊不足");
        dispatch.x86_previous=static_cast<std::uint32_t>(memory.load(GuestAddress{state.fs_base},32));dispatch.x86_guard=guard;
        memory.store(GuestAddress{guard},32,dispatch.x86_previous);memory.store(GuestAddress{guard+4},32,callback_return_.value+0x90);
        memory.store(GuestAddress{state.fs_base},32,guard);
        // x86 的 EXCEPTION_ROUTINE 是 cdecl；共用 callback 返回會還原 caller ESP，不假設 ret 16。
        start_callbacks(state,memory,{{handler,{dispatch.record,frame,dispatch.context,dispatch.dispatcher}}},Continuation::Exception);
        return std::nullopt;
    }
    if(!unwind && dispatch.user_callback_depth!=SIZE_MAX)return finish_user_callback_exception(state,memory);
    if(!unwind && dispatch.etw_thread){dispatch.stage=ExceptionStage::Unwind;dispatch.x86_next=static_cast<std::uint32_t>(memory.load(GuestAddress{dispatch.original.fs_base},32));
        dispatch.x86_target=dispatch.etw_seh_boundary;dispatch.flags|=2;memory.store(GuestAddress{dispatch.record+4},32,dispatch.flags);state.rip=exception_entry_;return {};}
    if(!unwind && dispatch.protected_callback_depth!=SIZE_MAX){dispatch.stage=ExceptionStage::Unwind;dispatch.x86_next=static_cast<std::uint32_t>(memory.load(GuestAddress{dispatch.original.fs_base},32));dispatch.x86_target=runtime_.callbacks[dispatch.protected_callback_depth].user_seh_boundary;dispatch.flags|=2;memory.store(GuestAddress{dispatch.record+4},32,dispatch.flags);state.rip=exception_entry_;return {};}
    if(!unwind){dispatch.stage=ExceptionStage::Unhandled;state.rip=exception_entry_;return std::nullopt;}
    if(dispatch.x86_target && dispatch.x86_next!=dispatch.x86_target)throw HostCallFault("Win32 unwind 找不到目標框架");
    if(dispatch.etw_thread)return finish_etw_exception(state,memory);
    if(dispatch.protected_callback_depth!=SIZE_MAX)return finish_protected_callback_exception(state,memory);
    if(dispatch.restore_jump){
        const auto buffer=dispatch.x86_jump_buffer;const auto restored=dispatch.target_context;
        state=read_context(memory,dispatch.context,dispatch.original);exceptions_.stack.pop_back();
        return finish_x86_longjmp(state,memory,buffer,restored);
    }
    state=read_context(memory,dispatch.context,dispatch.original);state.reg(Gpr::Rax)=dispatch.return_value;
    exceptions_.stack.pop_back();return std::nullopt;
}
void WindowsServices::begin_x86_unwind(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto target=static_cast<std::uint32_t>(arg(0));const auto record=arg(2),value=arg(3);
    if(target && target!=UINT32_MAX && !valid_frame(memory,state,target))throw HostCallFault("Win32 unwind 目標框架不合法");
    std::uint32_t code=0xc0000027U,flags=0;
    if(record){memory.check(GuestAddress{record},80,Access::Read);memory.check(GuestAddress{record+4},4,Access::Write);code=static_cast<std::uint32_t>(memory.load(GuestAddress{record},32));flags=static_cast<std::uint32_t>(memory.load(GuestAddress{record+4},32));if(memory.load(GuestAddress{record+16},32)>15)throw HostCallFault("Win32 unwind 例外參數過多");}
    auto caller=state;arg.return_to(caller,value);begin_exception(caller,memory,code,flags,{});
    auto& dispatch=exceptions_.stack.back();dispatch.stage=ExceptionStage::Unwind;dispatch.x86_target=target;dispatch.return_value=value;
    // i386 ABI 的 TargetIp 保留不用：清理 SEH registrations 後回到 RtlUnwind caller，由 personality 執行最後跳轉。
    if(record){dispatch.record=record;memory.store(GuestAddress{dispatch.pointers},32,record);}
    dispatch.flags=flags|2U|(target?0U:4U);memory.store(GuestAddress{dispatch.record+4},32,dispatch.flags);
    state=caller;
}
}
