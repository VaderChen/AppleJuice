#include "x64arm/windows_services.hpp"
#include <algorithm>
namespace x64arm {
namespace {
void floating_state(GuestState& to,const GuestState& from){
    to.x87=from.x87;to.x87_control=from.x87_control;to.x87_status=from.x87_status;to.x87_tag=from.x87_tag;to.mxcsr=from.mxcsr;to.xmm=from.xmm;
    to.x87_instruction_pointer=from.x87_instruction_pointer;to.x87_data_pointer=from.x87_data_pointer;to.x87_opcode=from.x87_opcode;to.x87_code_selector=from.x87_code_selector;to.x87_data_selector=from.x87_data_selector;
}
void thread_state(GuestState& to,const GuestState& from){to.fs_base=from.fs_base;to.gs_base=from.gs_base;to.segments=from.segments;to.processor_id=from.processor_id;to.tsc=from.tsc;}
}
std::span<const WindowsServiceDescriptor> fiber_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","ConvertThreadToFiber",1},{"kernel32.dll","ConvertThreadToFiberEx",2},
        {"kernel32.dll","ConvertFiberToThread",0},{"kernel32.dll","IsThreadAFiber",0},
        {"kernel32.dll","CreateFiber",3},{"kernel32.dll","CreateFiberEx",5},
        {"kernel32.dll","SwitchToFiber",1},{"kernel32.dll","DeleteFiber",1}
    };return table;
}
void WindowsServices::destroy_fiber(GuestState& state,GuestMemory& memory,std::uint64_t handle){
    const auto found=threads_.fibers.find(handle);if(found==threads_.fibers.end())return;
    if(found->second.active_thread)throw HostCallFault("不能釋放執行中的 fiber");
    // 被刪除的 fiber 可能停在可回呼的 API 內，必須歸還該框架的暫存。
    auto callbacks=std::move(runtime_.callbacks);auto sorts=std::move(runtime_.sorts);
    runtime_.callbacks=std::move(found->second.callbacks);runtime_.sorts=std::move(found->second.sorts);
    try{discard_callbacks(0,state,memory);}catch(...){runtime_.callbacks=std::move(callbacks);runtime_.sorts=std::move(sorts);throw;}
    runtime_.callbacks=std::move(callbacks);runtime_.sorts=std::move(sorts);
    const auto stack=found->second.stack_allocation,cells=found->second.msvc_exception_cells,area=found->second.msvc_exception_area;
    const auto base=found->second.stack_base,end=base+found->second.stack_size;
    if(stack){
        if(found->second.stack_guard.armed)memory.protect(GuestAddress{base+found->second.stack_guard.guarantee},4096,read_write);
        runtime_.virtual_page_protections.erase(runtime_.virtual_page_protections.lower_bound(base),runtime_.virtual_page_protections.lower_bound(end));
    }
    threads_.fibers.erase(found);
    if(cells)(void)free_heap(memory,cells,state);if(area)(void)free_heap(memory,area,state);
    if(stack)(void)free_heap(memory,stack,state);(void)free_heap(memory,handle,state);
}
std::optional<std::uint32_t> WindowsServices::invoke_fiber(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;auto& thread=threads_.items.at(threads_.current);
    const std::uint8_t bits=x86_?32:64;const unsigned unit=x86_?4:8;const auto fiber_cell=thread.teb+(x86_?0x10:0x20);std::uint64_t result=0;
    if(symbol=="IsThreadAFiber")result=thread.fiber?1:0;
    else if(symbol=="ConvertThreadToFiber" || symbol=="ConvertThreadToFiberEx"){
        const auto flags=symbol.ends_with("Ex")?arg(1):0;
        if(flags&~UINT64_C(1))last_error_=87;
        else if(thread.fiber)last_error_=1280;
        else if(threads_.fibers.size()>=4096)last_error_=8;
        else {
            const auto handle=allocate_heap(memory,unit);if(!handle)last_error_=8;
            else {GuestFiber fiber;fiber.float_switch=(flags&1)!=0;fiber.active_thread=threads_.current;fiber.last_thread=threads_.current;
                fiber.stack_base=thread.stack_base;fiber.stack_size=thread.stack_size;fiber.stack_guard=thread.stack_guard;
                fiber.stack_allocation=thread.fiber_stack_allocation;fiber.fls_values=runtime_.fls_values;
                try{threads_.fibers.emplace(handle,std::move(fiber));}catch(...){(void)free_heap(memory,handle,state);throw;}
                auto& installed=threads_.fibers.at(handle);installed.msvc_exception_cells=thread.msvc_exception_cells;installed.msvc_exception_area=thread.msvc_exception_area;
                thread.msvc_exception_cells=0;thread.msvc_exception_area=0;
                thread.fiber_stack_allocation=0;thread.fiber=handle;memory.store(GuestAddress{handle},bits,arg(0));memory.store(GuestAddress{fiber_cell},bits,handle);result=handle;
            }
        }
    }else if(symbol=="ConvertFiberToThread"){
        if(!thread.fiber)last_error_=1281;
        else {const auto handle=thread.fiber;auto& fiber=threads_.fibers.at(handle);thread.fiber_stack_allocation=fiber.stack_allocation;
            thread.stack_base=fiber.stack_base;thread.stack_size=fiber.stack_size;thread.stack_guard=fiber.stack_guard;
            thread.msvc_exception_cells=fiber.msvc_exception_cells;thread.msvc_exception_area=fiber.msvc_exception_area;
            thread.fiber=0;threads_.fibers.erase(handle);memory.store(GuestAddress{fiber_cell},bits,0);(void)free_heap(memory,handle,state);result=1;}
    }else if(symbol=="CreateFiber" || symbol=="CreateFiberEx"){
        const bool ex=symbol.ends_with("Ex");const auto commit=arg(0),reserve=ex?arg(1):0,flags=ex?arg(2):0,entry=arg(ex?3:1),parameter=arg(ex?4:2);
        const auto requested=std::max(commit,reserve?reserve:std::max(UINT64_C(65536),runtime_.image.stack_size));
        if(flags&~UINT64_C(1))last_error_=87;
        else if(requested>16*1024*1024 || threads_.fibers.size()>=4096)last_error_=8;
        else if(!entry)last_error_=87;
        else {
            if(!handles(GuestAddress{entry}))memory.check(GuestAddress{entry},1,Access::Execute);
            const auto size=(requested+65535)&~UINT64_C(65535);// 在堆疊頂端保留配置器框架保護範圍，避免相鄰 metadata 落入 呼叫框架探查。
            const auto stack=allocate_heap(memory,size+4096+256);
            if(!stack)last_error_=8;
            else {const auto handle=allocate_heap(memory,unit);
                if(!handle){(void)free_heap(memory,stack,state);last_error_=8;}
                else {GuestFiber fiber;fiber.stack_base=(stack+4095)&~UINT64_C(4095);fiber.stack_size=size;fiber.stack_allocation=stack;fiber.float_switch=(flags&1)!=0;
                    initialize_stack_guard(memory,fiber.stack_base,size,fiber.stack_guard);
                    fiber.context.rip=GuestAddress{entry};fiber.context.segments=state.segments;floating_state(fiber.context,state);
                    const auto top=(fiber.stack_base+size)&~UINT64_C(15),sp=top-(x86_?8:40);fiber.context.reg(Gpr::Rsp)=sp;
                    memory.store(GuestAddress{sp},bits,callback_return_.value+0x80);if(x86_)memory.store(GuestAddress{sp+4},32,parameter);else fiber.context.reg(Gpr::Rcx)=parameter;
                    memory.store(GuestAddress{handle},bits,parameter);
                    try{threads_.fibers.emplace(handle,std::move(fiber));}catch(...){(void)free_heap(memory,stack,state);(void)free_heap(memory,handle,state);throw;}result=handle;
                }
            }
        }
    }else if(symbol=="SwitchToFiber"){
        const auto target=threads_.fibers.find(arg(0));
        if(!thread.fiber)last_error_=1281;
        else if(target==threads_.fibers.end() || target->second.deleting)last_error_=87;
        else if(target->second.active_thread)last_error_=170;
        else if(target->second.last_thread && target->second.last_thread!=threads_.current && (!target->second.exceptions.empty() || std::any_of(target->second.callbacks.begin(),target->second.callbacks.end(),[](const auto& frame){return frame.loader_notification_lock;})))last_error_=50;
        else {
            auto& source=threads_.fibers.at(thread.fiber);auto& destination=target->second;arg.return_to(state,0);
            source.context=state;source.callbacks=std::move(runtime_.callbacks);source.sorts=std::move(runtime_.sorts);source.exceptions=std::move(exceptions_.stack);source.fls_values=runtime_.fls_values;
            if(x86_)source.exception_list=memory.load(GuestAddress{thread.teb},32);source.active_thread=0;
            const auto previous=state;state=destination.context;thread_state(state,previous);if(x86_ && !destination.float_switch)floating_state(state,previous);
            runtime_.callbacks=std::move(destination.callbacks);runtime_.sorts=std::move(destination.sorts);exceptions_.stack=std::move(destination.exceptions);runtime_.fls_values=destination.fls_values;sync_fls_mirror(memory,thread.teb,runtime_.fls_values);
            for(auto& frame:runtime_.callbacks)thread_state(frame.saved,previous);
            destination.active_thread=threads_.current;destination.last_thread=threads_.current;thread.fiber=target->first;
            memory.store(GuestAddress{fiber_cell},bits,thread.fiber);memory.store(GuestAddress{thread.teb+unit},bits,destination.stack_base+destination.stack_size);memory.store(GuestAddress{thread.teb+unit*2},bits,destination.stack_guard.limit(destination.stack_base));
            memory.store(GuestAddress{thread.teb+(x86_?0xf78U:0x1748U)},32,destination.stack_guard.guarantee);
            if(x86_)memory.store(GuestAddress{thread.teb},32,destination.exception_list);
            notify_concurrency_changed();return std::nullopt;
        }
    }else if(symbol=="DeleteFiber"){
        const auto found=threads_.fibers.find(arg(0));
        if(found==threads_.fibers.end() || found->second.deleting)last_error_=87;
        else if(thread.fiber==arg(0))return exit_thread(state,memory,0);
        else if(found->second.active_thread)last_error_=170;
        else if(std::any_of(found->second.callbacks.begin(),found->second.callbacks.end(),[](const auto& frame){return frame.loader_notification_lock;}))last_error_=50;
        else {
            auto& fiber=found->second;fiber.deleting=true;std::vector<GuestCallback> callbacks;
            for(std::size_t i=0;i<fiber.fls_values.size();++i){if(runtime_.fls_used[i] && runtime_.fls_callbacks[i] && fiber.fls_values[i])callbacks.push_back({runtime_.fls_callbacks[i],{fiber.fls_values[i]}});fiber.fls_values[i]=0;}
            if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::FiberDelete,arg(0));return std::nullopt;}
            destroy_fiber(state,memory,arg(0));
        }
    }
    arg.return_to(state,result);return std::nullopt;
}
}
