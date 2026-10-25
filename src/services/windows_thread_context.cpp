#include "x64arm/windows_services.hpp"
#include "x64arm/thread_context.hpp"

namespace x64arm {
// 跨程序請求共用執行緒控制流程；於協調執行緒的 schedule／wake 處理已保存的 context。
std::uint32_t WindowsServices::local_primary_thread_control(std::uint32_t operation,ExecutionMode mode,std::uint32_t flags,std::uint32_t& value,std::vector<std::uint8_t>& bytes,std::uint32_t target_thread) {
    const auto found=threads_.items.find(target_thread);if(found==threads_.items.end() || found->second.status==ThreadStatus::Exited)return 87;
    const auto actual=x86_?ExecutionMode::X86:ExecutionMode::X64;
    if(!operation){value=actual==ExecutionMode::X86?32:64;return 0;}
    if((operation==1 || operation==4 || operation==5) && mode!=actual)return 87;
    auto& thread=found->second;
    if(operation==1 || operation==4) {
        if(!thread.suspended && !debugger_stopped())return 87;
        if(threads_.current==target_thread && !threads_.context_saved)return 170;
        if(const auto error=thread_context::validate_flags(mode,flags))return error;
        try{
            if(operation==1)bytes=thread_context::capture(thread.context,mode,flags);
            else {
                if(bytes.size()!=thread_context::size(mode))return 87;
                if((flags&1U) && thread.status==ThreadStatus::Waiting)return 170;
                const auto offset=thread_context::flags_offset(mode);std::uint32_t embedded=0;for(unsigned i=0;i<4;++i)embedded|=std::uint32_t{bytes[offset+i]}<<(i*8);
                if(embedded!=flags)return 13;
                thread.context=thread_context::restore(thread.context,mode,bytes);bytes.clear();value=1;
                threads_.remaining=0;threads_.yield=true;notify_concurrency_changed();
            }
        }catch(const std::bad_alloc&){return 8;}catch(const HostCallFault&){return 50;}catch(const std::invalid_argument&){return 87;}
        return 0;
    }
    if(operation==2 || operation==5) {
        if(thread.suspended>=127)return 156;value=thread.suspended++;
    } else if(operation==3) {value=thread.suspended;if(thread.suspended)--thread.suspended;}
    else return 87;
    threads_.remaining=0;threads_.yield=true;notify_concurrency_changed();return 0;
}
std::optional<std::uint32_t> WindowsServices::invoke_thread_control(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall call(state,memory,service);const bool native=service.family=="ntdll.dll";
    const auto name=native?service.symbol.substr(2):service.symbol;const auto saved_error=last_error_;
    const bool getter=name=="GetThreadContext" || name=="Wow64GetThreadContext";
    const bool setter=name=="SetThreadContext" || name=="Wow64SetThreadContext";
    const bool wow64=name.starts_with("Wow64");const bool suspend=name=="SuspendThread" || name=="Wow64SuspendThread";
    const auto mode=wow64 || x86_?ExecutionMode::X86:ExecutionMode::X64;
    const auto fail=[&](std::uint32_t error){
        if(native){
            const auto status=error==5?0xc0000022U:error==6?(object_type(call(0)).empty()?0xc0000008U:0xc0000024U):
                error==8?0xc0000017U:error==87?0xc000004bU:error==156?0xc000004aU:error==998?0xc0000005U:
                error==128?0x80000002U:error==121?0xc00000b5U:error==50?0xc00000bbU:0xc0000001U;
            threads_.items.at(threads_.current).last_native_status=status;last_error_=saved_error;call.return_to(state,status);
        }else{last_error_=error;call.return_to(state,getter || setter?0:UINT32_MAX);}
        return std::optional<std::uint32_t>{};
    };
    const auto finish=[&](std::uint32_t value){
        if(native){
            if(call(1))memory.store(GuestAddress{call(1)},32,value);
            threads_.items.at(threads_.current).last_native_status=0;last_error_=saved_error;
        }
        call.return_to(state,native?0:value);return std::optional<std::uint32_t>{};
    };
    try {
        // 輸出只有 ULONG，且先探測再改計數；自我暫停在寫回及 ABI 返回後才排程。
        if(native && call(1)){
            if(call(1)%4)return fail(128);
            memory.check(GuestAddress{call(1)},4,Access::Write);
        }
        const auto id=thread_id(call(0));std::uint32_t pid=0,remote_tid=1,rights=id?thread_access(call(0)):0;std::uint64_t birth=0;
        if(!id)if(const auto error=process_thread_target(call(0),pid,rights,&birth,&remote_tid)){
            if(native && !suspend && error==87 && (rights&0x1002U) && process_signaled(call(0))==true)return finish(0);
            return fail(error);
        }
        if(!(rights&(getter?8U:setter?16U:native && !suspend?0x1002U:2U)))return fail(5);
        if(id && (getter || setter || wow64) && mode!=(x86_?ExecutionMode::X86:ExecutionMode::X64))return fail(87);
        std::uint32_t flags=0,value=0;std::vector<std::uint8_t> context;
        if(getter || setter) {
            if(!call(1))return fail(87);
            if(call(1)%(mode==ExecutionMode::X86?4U:16U))return fail(998);
            memory.check(GuestAddress{call(1)},thread_context::size(mode),Access::Read);
            if(getter)memory.check(GuestAddress{call(1)},thread_context::size(mode),Access::Write);
            flags=static_cast<std::uint32_t>(memory.load(GuestAddress{call(1)+thread_context::flags_offset(mode)},32));
            if(const auto error=thread_context::validate_flags(mode,flags))return fail(error);
            if(setter){context.resize(thread_context::size(mode));memory.read(GuestAddress{call(1)},context);}
        }
        if(id) {
            auto& thread=threads_.items.at(id);
            if(thread.status==ThreadStatus::Exited){if(native && !suspend)return finish(0);return fail(87);}
            if(getter) {
                if(id!=threads_.current && !thread.suspended)return fail(87);
                context=thread_context::capture(id==threads_.current?state:thread.context,mode,flags);
            } else if(setter) {
                if(!thread.suspended || id==threads_.current)return fail(87);
                if((flags&1U) && thread.status==ThreadStatus::Waiting)return fail(170);
                try{thread.context=thread_context::restore(thread.context,mode,context);}catch(const HostCallFault&){return fail(50);}value=1;
            } else if(suspend) {
                if(thread.suspended>=127)return fail(156);value=thread.suspended++;
                if(id==threads_.current){(void)finish(value);schedule(state,memory);return std::nullopt;}
            } else {value=thread.suspended;if(thread.suspended)--thread.suspended;}
            if(!getter)notify_concurrency_changed();
        } else {
            if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許跨程序執行緒 control");
            const auto operation=getter?1U:setter?4U:suspend?(wow64?5U:2U):3U;
            if(const auto debug=debug_thread_control(memory,pid,birth,remote_tid,operation,mode,flags,value,context)){if(*debug)return fail(*debug);}
            else if(const auto error=transfer_thread_control(memory,pid,operation,mode,flags,value,context,birth,remote_tid))return fail(error);
        }
        if(getter){thread_context::write_selected(memory,call(1),mode,context);value=1;}
        return finish(value);
    } catch(const MemoryFault&){return fail(998);}
      catch(const std::bad_alloc&){return fail(8);}
      catch(const std::invalid_argument&){return fail(87);}
}
}
