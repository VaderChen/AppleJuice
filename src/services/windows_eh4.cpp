#include "x64arm/windows_services.hpp"
#include <bit>

namespace x64arm {
namespace {
std::uint32_t dword(const GuestMemory& memory,std::uint64_t at) {return static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));}
std::int32_t signed_dword(const GuestMemory& memory,std::uint64_t at) {return std::bit_cast<std::int32_t>(dword(memory,at));}
std::uint32_t frame_offset(std::uint32_t ebp,std::int32_t offset) {
    const auto address=static_cast<std::int64_t>(ebp)+offset;
    if(address<0 || address>UINT32_MAX)throw HostCallFault("Win32 EH4 框架位址溢位");
    return static_cast<std::uint32_t>(address);
}
struct Scope {std::int32_t parent;std::uint32_t filter,handler;};
Scope scope(const GuestMemory& memory,const Eh4Operation& operation,std::int32_t level) {
    if(level<0 || level>=4096)throw HostCallFault("Win32 SEH scope 索引不合法或超過 4096");
    const auto at=static_cast<std::uint64_t>(operation.scope)+operation.scope_header+static_cast<unsigned>(level)*12;
    if(at>UINT32_MAX-11)throw HostCallFault("Win32 SEH scope 位址溢位");
    Scope entry{signed_dword(memory,at),dword(memory,at+4),dword(memory,at+8)};
    if(entry.parent!=operation.root_level && (entry.parent<0 || entry.parent>=level))throw HostCallFault("Win32 SEH scope 未向外層推進");
    return entry;
}
}
std::optional<std::uint32_t> WindowsServices::invoke_eh4(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    if(!x86_)throw HostCallFault("EH3／EH4 registration handler 僅適用 Win32");
    const WindowsCall arg(state,memory,service);Eh4Operation operation;operation.entry=state;
    operation.local_only=service.symbol=="_local_unwind2" || service.symbol=="_seh_longjmp_unwind";
    operation.legacy3=service.symbol!="_except_handler4_common";operation.arguments=service.arguments;
    const unsigned start=operation.legacy3?0:2;
    if(operation.legacy3){operation.root_level=-1;operation.scope_header=0;operation.level=operation.parent=operation.target=-1;}
    else{operation.cookie=static_cast<std::uint32_t>(arg(0));operation.checker=static_cast<std::uint32_t>(arg(1));}
    if(operation.local_only){
        if(service.symbol=="_seh_longjmp_unwind"){
            memory.check(GuestAddress{arg(0)},64,Access::Read);operation.frame=dword(memory,arg(0)+24);operation.target=signed_dword(memory,arg(0)+28);
        }else{operation.frame=static_cast<std::uint32_t>(arg(0));operation.target=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(1)));}
        if(operation.frame==UINT32_MAX){arg.return_to(state,0);return std::nullopt;}
        operation.unwinding=true;operation.disposition=0;
        if(operation.target<operation.root_level || operation.target>=4096)throw HostCallFault("Win32 local unwind 終止索引不合法");
    }else{
        operation.record=static_cast<std::uint32_t>(arg(start));operation.frame=static_cast<std::uint32_t>(arg(start+1));
        operation.context=static_cast<std::uint32_t>(arg(start+2));operation.dispatcher=static_cast<std::uint32_t>(arg(start+3));
    }
    const auto low=dword(memory,state.fs_base+8),high=dword(memory,state.fs_base+4);
    if((operation.frame&3U) || operation.frame<low+8ULL || operation.frame>high || high-operation.frame<16 || (!operation.legacy3 && (!operation.cookie || !operation.checker)))
        throw HostCallFault("Win32 SEH registration／cookie 參數不合法");
    memory.check(GuestAddress{operation.frame-8},24,Access::Read);
    memory.check(GuestAddress{operation.frame-4},20,Access::Write);
    if(!operation.local_only)memory.check(GuestAddress{operation.record},80,Access::Read);
    if(operation.context || !operation.legacy3)memory.check(GuestAddress{operation.context},716,Access::Read);
    if(!operation.local_only && dword(memory,operation.record+16)>15)throw HostCallFault("Win32 EH4 例外參數過多");
    operation.ebp=operation.frame+16;operation.scope=dword(memory,operation.frame+8);
    if(!operation.legacy3){operation.scope^=dword(memory,operation.cookie);memory.check(GuestAddress{operation.scope},16,Access::Read);}
    operation.level=signed_dword(memory,operation.frame+12);
    if(!operation.local_only)operation.unwinding=(dword(memory,operation.record+4)&0x66U)!=0;
    const auto sp=state.reg(Gpr::Rsp);
    if(sp<low+32ULL)throw HostCallFault("Win32 EH4 callback 堆疊不足");
    operation.pointers=sp-16;memory.store(GuestAddress{operation.pointers},32,operation.record);memory.store(GuestAddress{operation.pointers+4},32,operation.context);
    if(!operation.unwinding)memory.store(GuestAddress{operation.frame-4},32,operation.pointers);
    for(std::size_t i=exceptions_.stack.size();i>0;--i) {
        const auto& dispatch=exceptions_.stack[i-1];
        if(dispatch.record==operation.record && dispatch.x86_frame==operation.frame) {operation.exception_depth=i;operation.callback_depth=dispatch.callback_depth;break;}
    }
    const auto token=runtime_.next_eh4_operation++;runtime_.eh4_operations.emplace(token,std::move(operation));
    // EH3／EH4 共用續接器；filter／finally 皆由客體 CPU 執行，只有 EH4 排程 cookie 檢查。
    try {return resume_eh4(state,memory,token,0);}
    catch(...) {runtime_.eh4_operations.erase(token);throw;}
}
std::optional<std::uint32_t> WindowsServices::resume_eh4(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result) {
    auto found=runtime_.eh4_operations.find(token);if(found==runtime_.eh4_operations.end())throw HostCallFault("Win32 EH4 callback 缺少狀態");
    auto& operation=found->second;
    const auto callback=[&](std::vector<GuestCallback> calls) -> std::optional<std::uint32_t> {
        state=operation.entry;state.reg(Gpr::Rsp)-=32;state.reg(Gpr::Rbp)=operation.ebp;state.rflags&=~UINT64_C(0x400);
        start_callbacks(state,memory,std::move(calls),Continuation::Eh4Operation,token);return std::nullopt;
    };
    const auto cookies=[&](Eh4Operation::AfterCookies next) -> std::optional<std::uint32_t> {
        if(operation.legacy3){operation.phase=Eh4Operation::Phase::Cookies;operation.after_cookies=next;return resume_eh4(state,memory,token,0);}
        std::vector<GuestCallback> checks;
        for(unsigned i=0;i<2;++i) {
            const auto offset=signed_dword(memory,operation.scope+i*8);if(i==0 && offset==-2)continue;
            const auto xor_offset=signed_dword(memory,operation.scope+i*8+4);
            const auto cookie=dword(memory,frame_offset(operation.ebp,offset))^frame_offset(operation.ebp,xor_offset);
            checks.push_back({operation.checker,{cookie},false,false,0,false,1});
        }
        operation.phase=Eh4Operation::Phase::Cookies;operation.after_cookies=next;
        return callback(std::move(checks));
    };
    const auto finish=[&]() -> std::optional<std::uint32_t> {
        const auto saved=operation.entry;const auto value=operation.disposition;const auto arguments=operation.arguments;
        runtime_.eh4_operations.erase(token);state=saved;
        WindowsCall(state,memory,WindowsServiceDescriptor{"crt","SEH scope continuation",arguments}).return_to(state,value);return std::nullopt;
    };
    try {
        // steps == 0 代表新進入，還沒有排程 cookie callback。
        if(!operation.steps++)return cookies(Eh4Operation::AfterCookies::Walk);
        if(operation.steps>8192)throw HostCallFault("Win32 EH4 callback 過多");
        if(operation.phase==Eh4Operation::Phase::Cookies) {
            if(operation.after_cookies==Eh4Operation::AfterCookies::Return)return finish();
            if(operation.after_cookies==Eh4Operation::AfterCookies::Transfer) {
                const auto copy=operation;const auto sp=dword(memory,copy.frame-8);
                const auto low=dword(memory,copy.entry.fs_base+8),high=dword(memory,copy.entry.fs_base+4);
                if(sp<low || sp>high || (sp&3U))throw HostCallFault("Win32 EH4 catch 的 SavedESP 不合法");
                memory.check(GuestAddress{copy.handler},1,Access::Execute);
                auto restored=copy.context?read_context(memory,copy.context,copy.entry):copy.entry;
                if(copy.exception_depth) {
                    if(copy.exception_depth>exceptions_.stack.size())throw HostCallFault("Win32 EH4 catch 例外深度不合法");
                    leave_x86_exception_handler(memory,exceptions_.stack[copy.exception_depth-1]);
                    discard_callbacks(copy.callback_depth,state,memory);exceptions_.stack.resize(copy.exception_depth-1);
                }
                runtime_.eh4_operations.erase(token);state=restored;state.reg(Gpr::Rbp)=copy.ebp;state.reg(Gpr::Rsp)=sp;state.rip=GuestAddress{copy.handler};return std::nullopt;
            }
        } else if(operation.phase==Eh4Operation::Phase::Filter) {
            const auto disposition=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(result));
            if(disposition<0) {operation.disposition=0;return cookies(Eh4Operation::AfterCookies::Return);}
            if(disposition>0) {
                operation.target=operation.level;operation.phase=Eh4Operation::Phase::GlobalUnwind;
                return callback({{resolve("ntdll.dll","RtlUnwind").value,{operation.frame,0,operation.record,0}}});
            }
            operation.level=operation.parent;
        } else if(operation.phase==Eh4Operation::Phase::GlobalUnwind || operation.phase==Eh4Operation::Phase::Finally) {
            operation.level=signed_dword(memory,operation.frame+12);
        }
        const bool local_unwind=operation.unwinding || operation.phase==Eh4Operation::Phase::GlobalUnwind || operation.phase==Eh4Operation::Phase::Finally;
        while(operation.level!=operation.root_level && (!local_unwind || operation.level!=operation.target)) {
            if(++operation.steps>8192)throw HostCallFault("Win32 EH4 scope 鏈過長");
            const auto entry=scope(memory,operation,operation.level);operation.parent=entry.parent;
            if(local_unwind) {
                memory.store(GuestAddress{operation.frame+12},32,static_cast<std::uint32_t>(entry.parent));operation.level=entry.parent;
                if(!entry.filter) {operation.phase=Eh4Operation::Phase::Finally;operation.called=true;return callback({{entry.handler,{}}});}
            } else {
                if(entry.filter) {operation.phase=Eh4Operation::Phase::Filter;operation.called=true;return callback({{entry.filter,{}}});}
                operation.level=entry.parent;
            }
        }
        if(local_unwind && !operation.unwinding) {
            if(operation.level!=operation.target)throw HostCallFault("Win32 EH4 catch 目標 scope 已不存在");
            const auto selected=scope(memory,operation,operation.target);operation.handler=selected.handler;
            memory.store(GuestAddress{operation.frame+12},32,static_cast<std::uint32_t>(selected.parent));
            return cookies(Eh4Operation::AfterCookies::Transfer);
        }
        if(operation.local_only && operation.level!=operation.target)throw HostCallFault("Win32 local unwind 找不到終止索引");
        if(operation.called)return cookies(Eh4Operation::AfterCookies::Return);
        return finish();
    } catch(...) {runtime_.eh4_operations.erase(token);throw;}
}
}
