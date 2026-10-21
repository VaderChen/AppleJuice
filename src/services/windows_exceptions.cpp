#include "x64arm/windows_services.hpp"
#include "x64arm/interpreter.hpp"
#include "x64arm/windows_unwind.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace x64arm {
namespace {
constexpr std::array jump_registers{Gpr::Rbx, Gpr::Rsp, Gpr::Rbp, Gpr::Rsi, Gpr::Rdi, Gpr::R12, Gpr::R13, Gpr::R14, Gpr::R15};
GuestState read_jump_buffer(const GuestMemory& memory, std::uint64_t address, const GuestState& state, std::uint32_t value) {
    memory.check(GuestAddress{address}, 256, Access::Read);
    if(address%16)throw HostCallFault("Windows x64 jmp_buf 必須對齊 16 bytes");
    auto restored=state;
    for(std::size_t i=0;i<jump_registers.size();++i)restored.reg(jump_registers[i])=memory.load(GuestAddress{address+8+i*8},64);
    restored.rip=GuestAddress{memory.load(GuestAddress{address+80},64)};
    restored.mxcsr=static_cast<std::uint32_t>(memory.load(GuestAddress{address+88},32));
    if(restored.mxcsr&0xffff0000U)throw HostCallFault("jmp_buf 包含無效 MXCSR 保留位元");
    restored.x87_control=static_cast<std::uint16_t>(memory.load(GuestAddress{address+92},16));
    restored.x87_status=static_cast<std::uint16_t>(restored.x87_status&~0x80ffU);
    for(std::size_t i=6;i<16;++i)memory.read(GuestAddress{address+96+(i-6)*16},restored.xmm[i]);
    restored.reg(Gpr::Rax)=value?value:1;
    memory.check(restored.rip,1,Access::Execute);
    const auto sp=restored.reg(Gpr::Rsp);
    if(sp%16 || sp<memory.load(GuestAddress{state.gs_base+16},64) || sp>=memory.load(GuestAddress{state.gs_base+8},64))throw HostCallFault("longjmp 目標不在目前執行緒堆疊內");
    memory.check(GuestAddress{sp},8,Access::Read);
    return restored;
}
}
bool WindowsServices::dispatch_cpu_trap(GuestState& state,GuestMemory& memory,std::uint32_t code,unsigned length){
    // CPU 保存 trap 後的 RIP；Windows breakpoint 的 EXCEPTION_RECORD／CONTEXT 指向 CC 本身。
    if(code==0x80000003U && length)state.rip.value=(state.rip.value-1)&(x86_?UINT32_MAX:UINT64_MAX);
    return dispatch_cpu_exception(state,memory,code);
}
bool WindowsServices::dispatch_cpu_exception(GuestState& state, GuestMemory& memory, std::uint32_t code) {
    // 核心驅動映像內的特權 I/O 指令由虛擬平台模擬後續跑；未模擬者仍依 #GP 交給例外處理。
    if(code==0xc0000096U && emulate_kernel_instruction(state,memory))return true;
    prune_x86_exception_callbacks(state,memory);
    if(emit_debug_event(state,memory,debugger::Exception{code,0,0,state.rip.value,{},true})==debugger::handled)return true;
    const bool module_functions=std::any_of(runtime_.loaded_modules.begin(),runtime_.loaded_modules.end(),[](const auto& entry){return !entry.second.image.functions.empty();}) ||
        std::any_of(runtime_.dynamic_function_tables.begin(),runtime_.dynamic_function_tables.end(),[](const auto& entry){return entry.count!=0;});
    const bool etw_callback=std::any_of(runtime_.callbacks.begin(),runtime_.callbacks.end(),[](const auto& frame){return frame.continuation==Continuation::EtwProcess;});
    if (!runtime_.image.base || (!etw_callback && runtime_.image.functions.empty() && !module_functions && (!x86_ || memory.load(GuestAddress{state.fs_base},32)==UINT32_MAX) && exceptions_.vectors.empty() && !runtime_.unhandled_filter)) {
        if(!debugger_attached())return false;
        // 仍走既有例外終止 continuation，最後機會遭拒後必須交付退出事件並結束 guest，
        // 不可只回到 decoder 的 InvalidInstruction 停止原因。
        begin_exception(state,memory,code,0,{});exceptions_.stack.back().stage=ExceptionStage::Unhandled;return true;
    }
    begin_exception(state, memory, code, 0, {}); return true;
}
bool WindowsServices::dispatch_fault(GuestState& state, GuestMemory& memory, const MemoryFault& fault) {
    struct Refresh { WindowsServices& self;GuestMemory& memory;~Refresh(){self.refresh_guard_table(memory);} } refresh{*this,memory};
    prune_x86_exception_callbacks(state,memory);
    if (!runtime_.image.base) return false;
    if(std::getenv("X64ARM_TRACE_FAULTS"))std::cerr<<"[fault] rip=0x"<<std::hex<<state.rip.value<<" address=0x"<<fault.address.value<<" access="<<static_cast<unsigned>(fault.access)<<std::dec<<'\n';
    if(release_stack_guard(state,memory,fault.address.value)){
        constexpr std::uint32_t overflow=0xc00000fdU;
        if(emit_debug_event(state,memory,debugger::Exception{overflow,0,0,state.rip.value,{},true})==debugger::handled)return true;
        begin_exception(state,memory,overflow,0,{});return true;
    }
    const std::array<std::uint64_t, 2> parameters{fault.access == Access::Write ? 1U : fault.access == Access::Execute ? 8U : 0U, fault.address.value};
    auto code=0xc0000005U;const auto page=fault.address.value&~UINT64_C(4095);
    if(auto protection=runtime_.virtual_page_protections.find(page);protection!=runtime_.virtual_page_protections.end() && (protection->second.mode&0x100U)){
        const auto mode=protection->second.mode&~0x100U;
        const Permissions access=mode==2?1:mode==4 || mode==8?3:mode==16?4:mode==32?5:7;
        if(mode==8 || mode==128)memory.make_copy_on_write(GuestAddress{page},4096,access);else memory.protect(GuestAddress{page},4096,access);
        protection->second.mode=mode;code=0x80000001U;
    }
    if(emit_debug_event(state,memory,debugger::Exception{code,0,0,state.rip.value,{parameters.begin(),parameters.end()},true})==debugger::handled)return true;
    begin_exception(state, memory, code, 0, parameters); return true;
}
void WindowsServices::begin_exception(GuestState& state, GuestMemory& memory, std::uint32_t code, std::uint32_t flags_value, std::span<const std::uint64_t> parameters) {
    if(std::getenv("X64ARM_TRACE_FAULTS"))std::cerr<<"[exception] thread="<<threads_.current<<" code=0x"<<std::hex<<code<<" rip=0x"<<state.rip.value<<" rsp=0x"<<state.reg(Gpr::Rsp)<<std::dec<<'\n';
    if(std::getenv("X64ARM_TRACE_EXCEPTION_STACK"))for(unsigned slot=0;slot<4096;++slot){
        const auto stack=state.reg(Gpr::Rsp);const auto unit=x86_?4U:8U;if(stack>UINT64_MAX-slot*unit)break;
        try{const auto target=memory.load(GuestAddress{stack+slot*unit},static_cast<std::uint8_t>(unit*8));const auto mapping=memory.query(GuestAddress{target});
            if(mapping && (mapping->permissions&permission(Access::Execute)) && module_image(target,true))std::cerr<<"[exception_stack_candidate] offset="<<slot*unit<<" code=0x"<<std::hex<<target<<std::dec<<'\n';
        }catch(const MemoryFault&){break;}
    }
    if (exceptions_.stack.size() >= 16 || parameters.size() > 15) throw HostCallFault("例外巢狀深度或參數數目超過上限");
    auto area=exception_base_.value;
    if(!threads_.items.empty()){
        auto& thread=threads_.items.at(threads_.current);area=thread.exception_area;
        if(thread.fiber){
            auto& fiber=threads_.fibers.at(thread.fiber);
            if(!fiber.msvc_exception_area){fiber.msvc_exception_area=allocate_heap(memory,0x20000);if(!fiber.msvc_exception_area)throw std::bad_alloc{};}
            area=fiber.msvc_exception_area;
        }else if(thread.msvc_exception_area)area=thread.msvc_exception_area;
    }
    // 重拋時物件可能仍位於已離開 catch 的 arena；依實際紀錄／物件引用選擇空槽。
    std::array<bool,16> occupied{};const auto pin=[&](std::uint64_t address){if(address>=area && address-area<0x20000)occupied[(address-area)/8192]=true;};
    for(const auto& item:exceptions_.stack){pin(item.record);pin(item.context);if(item.code==0xe06d7363U && memory.load(GuestAddress{item.record+(x86_?16U:24U)},32)>=3)pin(memory.load(GuestAddress{item.record+(x86_?24U:40U)},x86_?32:64));}
    if(code==0xe06d7363U && parameters.size()>=3)pin(parameters[1]);
    const auto available=std::find(occupied.begin(),occupied.end(),false);if(available==occupied.end())throw HostCallFault("作用中例外物件已用完 arena");
    const auto base=area+static_cast<std::uint64_t>(available-occupied.begin())*8192;
    ExceptionDispatch dispatch;
    dispatch.original = state; dispatch.walk = state; dispatch.code = code; dispatch.flags = flags_value;
    dispatch.record = base; dispatch.context = base + 0x100; dispatch.pointers = base + 0x700;
    dispatch.dispatcher = base + 0x800; dispatch.walk_context = base + 0x1000;
    dispatch.callback_depth = runtime_.callbacks.size();
    if(x86_)dispatch.x86_next=static_cast<std::uint32_t>(memory.load(GuestAddress{state.fs_base},32));
    dispatch.walk_callback_depth=dispatch.callback_depth;dispatch.walk_exception_depth=exceptions_.stack.size();
    for (const auto& item : exceptions_.vectors) dispatch.vectors.push_back(item.second);
    memory.write(GuestAddress{base}, std::array<std::uint8_t, 8192>{});
    memory.store(GuestAddress{base}, 32, code); memory.store(GuestAddress{base + 4}, 32, flags_value);
    const std::uint8_t bits=x86_?32:64;const unsigned unit=bits/8;
    memory.store(GuestAddress{base+(x86_?12:16)},bits,state.rip.value);memory.store(GuestAddress{base+(x86_?16:24)},32,parameters.size());
    for(std::size_t i=0;i<parameters.size();++i)memory.store(GuestAddress{base+(x86_?20:32)+i*unit},bits,parameters[i]);
    write_context(memory, dispatch.context, state);
    memory.store(GuestAddress{dispatch.pointers},bits,dispatch.record);memory.store(GuestAddress{dispatch.pointers+unit},bits,dispatch.context);
    exceptions_.stack.reserve(16);
    if(code==0xe06d7363U && parameters.size()>=3)for(auto& outer:exceptions_.stack)if(outer.msvc_owned_object && outer.msvc_owned_object==parameters[1]){dispatch.msvc_owned_object=outer.msvc_owned_object;outer.msvc_owned_object=0;break;}
    exceptions_.stack.push_back(std::move(dispatch)); state.rip = exception_entry_;
}
bool WindowsServices::append_exception_frame(GuestMemory& memory, ExceptionDispatch& dispatch) {
    while(handles(dispatch.walk.rip)) {
        if(dispatch.walk.rip.value==callback_return_.value+0xe0){
            const auto& calls=threads_.items.at(threads_.current).system_calls;const auto found=std::find_if(calls.rbegin(),calls.rend(),[&](const auto& frame){return dispatch.walk.reg(Gpr::Rsp)==frame.stack+8;});
            if(found==calls.rend())throw HostCallFault("NT 系統呼叫例外框架不符");dispatch.walk.rip=GuestAddress{found->caller};continue;
        }
        if(dispatch.walk.rip==thread_return_) {dispatch.walk.rip=return_sentinel;break;}
        if(dispatch.walk.rip!=callback_return_ || !dispatch.walk_callback_depth)throw HostCallFault("例外堆疊包含無對應框架的主機入口");
        const auto& callback=runtime_.callbacks.at(--dispatch.walk_callback_depth);
        const auto expected=(callback.saved.reg(Gpr::Rsp)-256)&~UINT64_C(15);
        if(dispatch.walk.reg(Gpr::Rsp)!=expected){
            if(std::getenv("X64ARM_TRACE_FAULTS"))std::cerr<<"[exception_callback] rsp=0x"<<std::hex<<dispatch.walk.reg(Gpr::Rsp)<<" expected=0x"<<expected<<" saved_rsp=0x"<<callback.saved.reg(Gpr::Rsp)<<" saved_rip=0x"<<callback.saved.rip.value<<" target=0x"<<(callback.index<callback.callbacks.size()?callback.callbacks[callback.index].target:0)<<std::dec<<" depth="<<dispatch.walk_callback_depth<<" continuation="<<static_cast<unsigned>(callback.continuation)<<'\n';
            throw HostCallFault("例外 callback 堆疊位置不符");
        }
        if(callback.continuation==Continuation::UnwindConsolidate && callback.unwind_target){
            // catch 內再次拋出例外時，跨過已經完成清理的框架。
            dispatch.walk=*callback.unwind_target;dispatch.walk_exception_depth=static_cast<std::size_t>(callback.value);
            dispatch.unwound=true;continue;
        }
        if(callback.continuation==Continuation::EtwProcess){dispatch.etw_thread=callback.value;dispatch.etw_callback_depth=dispatch.walk_callback_depth;
            dispatch.etw_exception_depth=callback.exception_depth;return false;}
        if(callback_protects_exception(callback,dispatch.code)){dispatch.protected_callback_depth=dispatch.walk_callback_depth;return false;}
        if((runtime_.user_mode_exception_policy&1U) && callback.index<callback.callbacks.size() && callback.callbacks[callback.index].user_mode_callback){
            dispatch.user_callback_depth=dispatch.walk_callback_depth;return false;
        }
        const bool tls_boundary=callback.index<callback.callbacks.size() && callback.callbacks[callback.index].tls_initializer;
        const bool dll_boundary=callback.index<callback.callbacks.size() && callback.callbacks[callback.index].dll_initializer;
        if(callback.continuation==Continuation::ModuleOperation || tls_boundary || dll_boundary) {
            const auto operation=runtime_.module_operations.find(callback.value);
            const bool attach=callback.continuation==Continuation::ModuleOperation && operation!=runtime_.module_operations.end() &&
                operation->second.index<operation->second.steps.size() && operation->second.steps[operation->second.index].check_result;
            if(tls_boundary || dll_boundary || attach) {
                // DLL／TLS 通知的載入器邊界不能搜尋到外層 LoadLibrary／FreeLibrary caller。
                dispatch.module_operation=callback.continuation==Continuation::ModuleOperation?callback.value:0;
                dispatch.module_continue=!attach;
                dispatch.module_callback_depth=dispatch.walk_callback_depth;
                dispatch.module_exception_depth=callback.exception_depth;
                dispatch.module_context=callback.saved;
                return false;
            }
        }
        if(callback.continuation==Continuation::MsvcCopy || callback.continuation==Continuation::MsvcUnwind || callback.continuation==Continuation::MsvcDestroy){
            // 清理函式內自行捕捉仍合法；只有逸出清理／catch 複製邊界才 terminate。
            dispatch.code=3;dispatch.unhandled_called=true;dispatch.walk.rip=return_sentinel;return false;
        }
        if(callback.continuation==Continuation::MsvcCatch && callback.exception_depth){
            const auto& parent=exceptions_.stack.at(callback.exception_depth-1);
            const auto& active=parent.frames.at(parent.target_frame);
            dispatch.walk=active.context;dispatch.walk_callback_depth=active.callback_depth;dispatch.walk_exception_depth=active.exception_depth;
            dispatch.msvc.parent=parent.msvc.parent;dispatch.msvc.function_info=parent.msvc.function_info;dispatch.msvc.rethrow_state=parent.msvc.catch_state;
            dispatch.unwound=false;
        } else if((callback.continuation==Continuation::Exception || callback.continuation==Continuation::SehFilter) && callback.exception_depth) {
            const auto& parent=exceptions_.stack.at(callback.exception_depth-1);
            if(parent.frames.empty())throw HostCallFault("例外 callback 沒有作用中 unwind 框架");
            const auto& active=parent.frames.at(parent.stage==ExceptionStage::Unwind?parent.index-1:parent.frames.size()-1);
            dispatch.walk=active.context;dispatch.walk_callback_depth=active.callback_depth;dispatch.walk_exception_depth=active.exception_depth;
            dispatch.unwound=false;
        } else {
            dispatch.walk=callback.saved;dispatch.walk_exception_depth=callback.exception_depth;
            const auto operation=callback.continuation==Continuation::ModuleOperation?runtime_.module_operations.find(callback.value):runtime_.module_operations.end();
            const bool module_startup=operation!=runtime_.module_operations.end() && operation->second.startup;
            if(module_startup || callback.continuation==Continuation::Startup || callback.continuation==Continuation::Exit || callback.continuation==Continuation::ThreadExit)dispatch.walk.rip=return_sentinel;
            else WindowsCall(dispatch.walk,memory,0).return_to(dispatch.walk,0);
            dispatch.unwound=true;
        }
    }
    if(!dispatch.walk.rip.value || dispatch.walk.rip==return_sentinel)return false;
    if(dispatch.frames.size()>=256)throw HostCallFault("例外堆疊超過 256 frames");
    if(dispatch.walk.reg(Gpr::Rsp)<memory.load(GuestAddress{dispatch.walk.gs_base+16},64) || dispatch.walk.reg(Gpr::Rsp)>=memory.load(GuestAddress{dispatch.walk.gs_base+8},64))throw HostCallFault("例外堆疊超出 TEB StackLimit／StackBase");
    const auto pc=dispatch.walk.rip.value-(dispatch.unwound?1U:0U);
    const auto* image=unwind_image(memory,pc);
    // 未登錄的 leaf code 仍可由 return slot 展開，但不可套用其他映像的 metadata。
    static const RuntimeImage leaf_image;
    const auto unwind=virtual_unwind(memory,image?*image:leaf_image,pc,dispatch.walk,3);
    if(unwind.caller.reg(Gpr::Rsp)<=dispatch.walk.reg(Gpr::Rsp))throw HostCallFault("例外 unwind 未向上推進堆疊");
    dispatch.frames.push_back({dispatch.walk,pc,unwind,dispatch.walk_callback_depth,dispatch.walk_exception_depth});return true;
}
void WindowsServices::prepare_handler(GuestMemory& memory, ExceptionDispatch& dispatch, const ExceptionWalkFrame& frame, bool target) {
    const auto* image=unwind_image(memory,frame.pc,frame.unwind.function_entry);
    if(!image)throw HostCallFault("例外 handler 所屬映像已不存在");
    memory.write(GuestAddress{dispatch.dispatcher}, std::array<std::uint8_t, 80>{});
    const std::array<std::uint64_t, 9> fields{frame.pc, image->base, frame.unwind.function_entry, frame.unwind.establisher,
        dispatch.stage == ExceptionStage::Unwind ? dispatch.target_ip : 0, dispatch.walk_context, frame.unwind.handler, frame.unwind.handler_data, 0};
    for (std::size_t i = 0; i < fields.size(); ++i) memory.store(GuestAddress{dispatch.dispatcher + i * 8}, 64, fields[i]);
    write_context(memory, dispatch.walk_context, frame.unwind.caller);
    memory.store(GuestAddress{dispatch.record + 4}, 32, dispatch.flags | (dispatch.stage == ExceptionStage::Unwind ? 2U | (target ? 32U : 0U) : 0));
}
std::optional<std::uint32_t> WindowsServices::finish_user_callback_exception(GuestState& state,GuestMemory& memory){
    const auto depth=exceptions_.stack.back().user_callback_depth;
    if(depth>=runtime_.callbacks.size())throw HostCallFault("GUI 例外邊界沒有 callback 框架");
    const auto saved=runtime_.callbacks[depth].saved;const auto exception_depth=runtime_.callbacks[depth].exception_depth;
    // Legacy filter 直接回到 callback 邊界，不跨邊界執行外層 SEH，也不假裝客體已清理資源。
    discard_callbacks(depth+1,state,memory);exceptions_.stack.resize(exception_depth);
    state=saved;state.reg(Gpr::Rax)=0;return resume_callback(state,memory);
}
bool WindowsServices::callback_protects_exception(const CallbackFrame& frame,std::uint32_t code)const{
    if(frame.index>=frame.callbacks.size())return false;const auto policy=frame.callbacks[frame.index].exception_policy;
    return policy==GuestExceptionPolicy::CaptureAll || (code==0xe06d7363U && (policy==GuestExceptionPolicy::CaptureCpp || policy==GuestExceptionPolicy::NoexceptCpp));
}
std::optional<std::uint32_t> WindowsServices::finish_protected_callback_exception(GuestState& state,GuestMemory& memory){
    const auto depth=exceptions_.stack.back().protected_callback_depth;if(depth>=runtime_.callbacks.size())throw HostCallFault("例外邊界的 callback 已不存在");const auto& frame=runtime_.callbacks[depth];
    if(frame.callbacks.at(frame.index).exception_policy==GuestExceptionPolicy::NoexceptCpp)return 3;
    if(frame.continuation==Continuation::Msvcp100Operation)return finish_msvcp_callback_exception(state,memory,depth);
    throw HostCallFault("例外邊界沒有對應 continuation");
}
std::optional<std::uint32_t> WindowsServices::advance_exception(GuestState& state, GuestMemory& memory) {
    if (exceptions_.stack.empty()) throw HostCallFault("例外派送入口沒有對應狀態");
    auto& dispatch = exceptions_.stack.back();
    const auto call = [&](std::uint64_t handler, std::vector<std::uint64_t> arguments) {
        state = dispatch.consolidate_record?dispatch.consolidate_origin:dispatch.original;
        start_callbacks(state, memory, {{handler, std::move(arguments)}}, Continuation::Exception);
    };
    if (dispatch.stage == ExceptionStage::Vectored) {
        if (dispatch.index < dispatch.vectors.size()) { call(dispatch.vectors[dispatch.index++], {dispatch.pointers}); return std::nullopt; }
        // RoOriginateError 在 API 內 raise 並 catch；VEH 仍看得到真實 EXCEPTION_RECORD。
        // 僅標記我們建立的 frame，不能吞掉 guest 自己 RaiseException 的同碼例外。
        if(dispatch.winrt_origination){state=dispatch.original;exceptions_.stack.pop_back();return std::nullopt;}
        dispatch.stage = ExceptionStage::Search; dispatch.index = 0;
    }
    if(x86_ && (dispatch.stage==ExceptionStage::Search || dispatch.stage==ExceptionStage::Unwind))return advance_x86_exception(state,memory);
    if (dispatch.stage == ExceptionStage::Search) {
        while (dispatch.frames.size() < 256) {
            if(!append_exception_frame(memory,dispatch)) {
                if(dispatch.user_callback_depth!=SIZE_MAX)return finish_user_callback_exception(state,memory);
                if(dispatch.module_operation || dispatch.module_continue || dispatch.etw_thread || dispatch.protected_callback_depth!=SIZE_MAX) {
                    // 目標是主機載入器，不屬任何 guest frame，因此不設 TARGET_UNWIND。
                    dispatch.stage=ExceptionStage::Unwind;dispatch.index=0;
                    dispatch.target_frame=dispatch.frames.size();dispatch.target_ip=0;
                    discard_callbacks(dispatch.callback_depth,state,memory);
                } else dispatch.stage=ExceptionStage::Unhandled;
                break;
            }
            const auto& unwind=dispatch.frames.back().unwind;
            if (unwind.handler && (unwind.handler_flags & 1)) {
                prepare_handler(memory, dispatch, dispatch.frames.back(), false);
                call(unwind.handler, {dispatch.record, unwind.establisher, dispatch.context, dispatch.dispatcher}); return std::nullopt;
            }
            dispatch.walk = unwind.caller; dispatch.unwound = true;
        }
        if (dispatch.stage == ExceptionStage::Search) throw HostCallFault("例外堆疊超過 256 frames");
    }
    if (dispatch.stage == ExceptionStage::Unwind) {
        while (dispatch.index < dispatch.frames.size() && dispatch.index <= dispatch.target_frame) {
            const auto index = dispatch.index++; const auto& frame = dispatch.frames[index];
            if (frame.unwind.handler && (frame.unwind.handler_flags & 2)) {
                prepare_handler(memory, dispatch, frame, index == dispatch.target_frame);
                call(frame.unwind.handler, {dispatch.record, frame.unwind.establisher, dispatch.context, dispatch.dispatcher}); return std::nullopt;
            }
        }
        if(dispatch.module_operation || dispatch.module_continue) {
            const auto operation=dispatch.module_operation;
            const auto depth=dispatch.module_exception_depth;
            const auto callbacks=dispatch.module_callback_depth;
            const bool proceed=dispatch.module_continue;
            state=dispatch.module_context;
            discard_callbacks(callbacks+(proceed?1U:0U),state,memory);
            exceptions_.stack.resize(depth);
            if(proceed)return resume_loader_notification_exception(state,memory,callbacks);
            return fail_module_initialization(state,memory,operation);
        }
        if(dispatch.etw_thread)return finish_etw_exception(state,memory);
        if(dispatch.protected_callback_depth!=SIZE_MAX)return finish_protected_callback_exception(state,memory);
        if(dispatch.msvc.active)return start_msvc_catch(state,memory);
        state = dispatch.target_context;
        if(!dispatch.restore_jump)state.reg(Gpr::Rsp) = dispatch.frames[dispatch.target_frame].unwind.establisher;
        state.rip = GuestAddress{dispatch.target_ip}; state.reg(Gpr::Rax) = dispatch.return_value;
        const auto& target=dispatch.frames[dispatch.target_frame];
        discard_callbacks(target.callback_depth,state,memory);
        const auto depth=target.exception_depth;
        if(dispatch.consolidate_record){
            const auto record=dispatch.consolidate_record;
            const auto callback=memory.load(GuestAddress{record+32},64);
            // 真正的回傳 PC 由 consolidation callback 決定；保留已展開的非揮發暫存器。
            state.rip=dispatch.frames[dispatch.target_frame].context.rip;
            const auto resumed=state;
            if(dispatch.consolidate_context)write_context(memory,dispatch.consolidate_context,resumed);
            state=dispatch.consolidate_origin;
            // 在 RtlUnwindEx 的原堆疊下方呼叫，避免覆寫 guest 的 EXCEPTION_RECORD／CONTEXT。
            start_callbacks(state,memory,{{callback,{record}}},Continuation::UnwindConsolidate,depth);
            runtime_.callbacks.back().unwind_target=resumed;return std::nullopt;
        }
        exceptions_.stack.resize(depth);return std::nullopt;
    }
    if (dispatch.stage == ExceptionStage::Unhandled) {
        if(dispatch.protected_callback_depth!=SIZE_MAX)return finish_protected_callback_exception(state,memory);
        if(!dispatch.etw_thread)for(std::size_t i=runtime_.callbacks.size();i-->0;)if(runtime_.callbacks[i].continuation==Continuation::EtwProcess){
            dispatch.etw_thread=runtime_.callbacks[i].value;dispatch.etw_callback_depth=i;dispatch.etw_exception_depth=runtime_.callbacks[i].exception_depth;
            dispatch.etw_seh_boundary=runtime_.callbacks[i].etw_seh_boundary;break;}
        if(dispatch.etw_thread)return finish_etw_exception(state,memory);
        if (runtime_.unhandled_filter && !dispatch.unhandled_called) { dispatch.unhandled_called = true; call(runtime_.unhandled_filter, {dispatch.pointers}); return std::nullopt; }
        const auto code = dispatch.code;state=dispatch.original;auto disposition=debugger::not_handled;
        if(debugger_attached()){
            debugger::Exception event{code,dispatch.flags,0,state.rip.value,{},false};
            const auto count=memory.load(GuestAddress{dispatch.record+(x86_?16:24)},32);
            if(count>15)throw HostCallFault("最後機會例外參數數目超過上限");
            for(std::uint64_t i=0;i<count;++i)event.parameters.push_back(memory.load(GuestAddress{dispatch.record+(x86_?20:32)+i*(x86_?4:8)},x86_?32:64));
            disposition=emit_debug_event(state,memory,std::move(event));
        }
        exceptions_.stack.pop_back();
        if(disposition==debugger::handled)return std::nullopt;
        (void)emit_debug_event(state,memory,debugger::ExitProcess{code==0xe06d7363U?3U:code});return code==0xe06d7363U?3U:code;
    }
    throw HostCallFault("例外派送狀態不合法");
}
std::optional<std::uint32_t> WindowsServices::resume_exception(GuestState& state, GuestMemory& memory, std::uint64_t value) {
    auto& dispatch = exceptions_.stack.back();
    const auto result = static_cast<std::uint32_t>(value);
    if(x86_)leave_x86_exception_handler(memory,dispatch);
    const bool resume = dispatch.stage == ExceptionStage::Search ? result == 0 :
        dispatch.stage != ExceptionStage::Unwind && result == UINT32_MAX;
    if (resume) {
        if (dispatch.flags & 1) throw HostCallFault("handler 嘗試繼續不可繼續的例外");
        state = read_context(memory, dispatch.context, dispatch.original);
        std::vector<GuestCallback> callbacks;
        for (const auto& handler : exceptions_.continue_vectors) callbacks.push_back({handler.second,{dispatch.pointers}});
        if (!callbacks.empty()) start_callbacks(state,memory,std::move(callbacks),Continuation::ContinueException);
        else exceptions_.stack.pop_back();
        return std::nullopt;
    }
    if(x86_ && (dispatch.stage==ExceptionStage::Search || dispatch.stage==ExceptionStage::Unwind)){
        if(dispatch.stage==ExceptionStage::Unwind){
            if(result==3){
                const auto collided=static_cast<std::uint32_t>(memory.load(GuestAddress{dispatch.dispatcher},32));
                if(collided<dispatch.x86_frame || collided==UINT32_MAX)throw HostCallFault("Win32 collided unwind 目標不合法");
                const auto low=memory.load(GuestAddress{dispatch.original.fs_base+8},32),high=memory.load(GuestAddress{dispatch.original.fs_base+4},32);
                if((collided&3U) || collided<low || collided>high || high-collided<8)throw HostCallFault("Win32 collided unwind 框架越界");
                dispatch.x86_next=static_cast<std::uint32_t>(memory.load(GuestAddress{collided},32));
                if(dispatch.x86_next!=UINT32_MAX && dispatch.x86_next<=collided)throw HostCallFault("Win32 collided unwind 未向上推進");
                memory.store(GuestAddress{dispatch.original.fs_base},32,dispatch.x86_next);
            }else if(result==1)memory.store(GuestAddress{dispatch.original.fs_base},32,dispatch.x86_next);
            else throw HostCallFault("Win32 unwind handler disposition 不合法");
        }else{
            if(dispatch.x86_frame==dispatch.x86_nested){dispatch.x86_nested=0;dispatch.flags&=~0x10U;}
            if(result==2){const auto nested=static_cast<std::uint32_t>(memory.load(GuestAddress{dispatch.dispatcher},32));if(nested<dispatch.x86_frame || nested==UINT32_MAX)throw HostCallFault("Win32 nested exception 目標不合法");dispatch.x86_nested=std::max(dispatch.x86_nested,nested);dispatch.flags|=0x10;}
            else if(result!=1)throw HostCallFault("Win32 exception handler disposition 不合法");
            memory.store(GuestAddress{dispatch.record+4},32,dispatch.flags);
        }
        state.rip=exception_entry_;return std::nullopt;
    }
    if (dispatch.stage == ExceptionStage::Search) {
        if (result != 1) throw HostCallFault("尚未支援 nested／collided unwind disposition");
        dispatch.walk = dispatch.frames.back().unwind.caller; dispatch.unwound = true;
    } else if (dispatch.stage == ExceptionStage::Vectored && result != 0) throw HostCallFault("vectored handler 回傳值不合法");
    else if (dispatch.stage == ExceptionStage::Unwind) {
        if(result!=1)throw HostCallFault("unwind handler 回傳值不合法");
        if(dispatch.index-1==dispatch.target_frame && !dispatch.restore_jump) {
            const auto& baseline=dispatch.frames[dispatch.target_frame].unwind.caller;
            const auto changed=read_context(memory,dispatch.walk_context,baseline);
            // 保留目標框架的暫存器；套用 personality handler 對 CONTEXT 的顯式修改。
            for(std::size_t i=0;i<changed.gpr.size();++i)if(changed.gpr[i]!=baseline.gpr[i])dispatch.target_context.gpr[i]=changed.gpr[i];
            for(std::size_t i=0;i<changed.xmm.size();++i)if(changed.xmm[i]!=baseline.xmm[i])dispatch.target_context.xmm[i]=changed.xmm[i];
        }
    }
    state.rip = exception_entry_; return std::nullopt;
}
void WindowsServices::begin_unwind(GuestState& state, GuestMemory& memory, std::size_t target, std::uint64_t ip, std::uint64_t result) {
    auto& dispatch = exceptions_.stack.back();
    if(!dispatch.consolidate_record)memory.check(GuestAddress{ip}, 1, Access::Execute);
    if (target >= dispatch.frames.size()) throw HostCallFault("unwind target frame 不合法");
    dispatch.target_frame = target; dispatch.target_ip = ip; dispatch.return_value = result;
    dispatch.target_context=dispatch.frames[target].context;
    dispatch.stage = ExceptionStage::Unwind; dispatch.index = 0;
    discard_callbacks(dispatch.callback_depth,state,memory);
    state = dispatch.consolidate_record?dispatch.consolidate_origin:dispatch.original; state.rip = exception_entry_;
}
std::optional<std::uint32_t> WindowsServices::seh_filter_result(GuestState& state, GuestMemory& memory, std::uint64_t value) {
    auto& dispatch = exceptions_.stack.back();
    const auto& frame=dispatch.frames.back();const auto* image=unwind_image(memory,frame.pc,frame.unwind.function_entry);
    if(!image)throw HostCallFault("SEH filter 所屬映像已不存在");
    const auto result = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
    if (result < 0) { complete_call(state,memory,0); return std::nullopt; }
    if (result > 0) {
        begin_unwind(state, memory, dispatch.frames.size() - 1, image->base + dispatch.scopes.at(dispatch.scope_index - 1).target, dispatch.code);
        return std::nullopt;
    }
    while (dispatch.scope_index < dispatch.scopes.size()) {
        const auto& scope = dispatch.scopes[dispatch.scope_index++];
        const auto offset = frame.pc - image->base;
        if (!scope.target || offset < scope.begin || offset >= scope.end || !scope.filter) continue;
        if (scope.filter == 1) {
            begin_unwind(state, memory, dispatch.frames.size() - 1, image->base + scope.target, dispatch.code); return std::nullopt;
        }
        const auto handler = image->base + scope.filter;
        memory.check(GuestAddress{handler}, 1, Access::Execute);
        start_callbacks(state, memory, {{handler, {dispatch.pointers, frame.unwind.establisher}}}, Continuation::SehFilter); return std::nullopt;
    }
    complete_call(state,memory,1); return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_exception(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state, memory, service); const auto symbol = service.symbol;
    if(x86_ && (symbol.find("setjmp")!=std::string_view::npos || symbol=="longjmp" || symbol=="_longjmpex"))return invoke_x86_jump(state,memory,service);
    if(!x86_ && symbol=="_setjmp3")throw HostCallFault("_setjmp3 僅適用 Win32 CRT ABI");
    if(symbol=="RtlAddFunctionTable" || symbol=="RtlDeleteFunctionTable" || symbol=="RtlAddGrowableFunctionTable" || symbol=="RtlGrowFunctionTable" || symbol=="RtlDeleteGrowableFunctionTable")return invoke_function_table(state,memory,service);
    std::uint64_t result = 0;
    auto& vectors=symbol.find("Continue")!=std::string_view::npos ? exceptions_.continue_vectors : exceptions_.vectors;
    if(symbol=="DebugBreak"){
        const auto address=state.rip.value;auto caller=state;arg.return_to(caller,state.reg(Gpr::Rax));
        if(emit_debug_event(caller,memory,debugger::Exception{0x80000003U,0,0,address,{},true})==debugger::handled){state=caller;return std::nullopt;}
        begin_exception(caller,memory,0x80000003U,0,{});
        memory.store(GuestAddress{exceptions_.stack.back().record+(x86_?12:16)},x86_?32:64,address);state=caller;return std::nullopt;
    }else if(symbol=="UnhandledExceptionFilter" || symbol=="__crtUnhandledException"){
        const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
        const auto record=memory.load(GuestAddress{arg(0)},bits),context=memory.load(GuestAddress{arg(0)+unit},bits);
        memory.check(GuestAddress{record},x86_?80:152,Access::Read);memory.check(GuestAddress{context},x86_?716:1232,Access::Read);
        if(runtime_.unhandled_filter){start_callbacks(state,memory,{{runtime_.unhandled_filter,{arg(0)}}},Continuation::UnhandledFilter);return std::nullopt;}
        result=1;
    }else if(x86_ && symbol=="RtlUnwind"){begin_x86_unwind(state,memory,service);return std::nullopt;
    }else if (symbol.find("setjmp") != std::string_view::npos) {
        const auto buffer=arg(0);
        memory.check(GuestAddress{buffer},256,Access::Write);
        if(buffer%16)throw HostCallFault("Windows x64 jmp_buf 必須對齊 16 bytes");
        auto caller=state;arg.return_to(caller,0);
        memory.store(GuestAddress{buffer},64,(symbol.ends_with("ex") || symbol=="_setjmp")?arg(1):0);
        for(std::size_t i=0;i<jump_registers.size();++i)memory.store(GuestAddress{buffer+8+i*8},64,caller.reg(jump_registers[i]));
        memory.store(GuestAddress{buffer+80},64,caller.rip.value);
        memory.store(GuestAddress{buffer+88},32,caller.mxcsr);memory.store(GuestAddress{buffer+92},16,caller.x87_control);
        for(std::size_t i=6;i<16;++i)memory.write(GuestAddress{buffer+96+(i-6)*16},caller.xmm[i]);
    } else if(symbol=="longjmp") {
        const auto buffer=arg(0),target=memory.load(GuestAddress{buffer},64);
        const auto restored=read_jump_buffer(memory,buffer,state,static_cast<std::uint32_t>(arg(1)));
        if(const auto* image=module_image(restored.rip.value,true);image && image->guard_long_jump &&
           !std::binary_search(image->guard_long_jump_targets.begin(),image->guard_long_jump_targets.end(),static_cast<std::uint32_t>(restored.rip.value-image->base)))return 0xc0000409U;
        if(!target) {
            // 無 SEH 的形式仍須丟棄被跨越的主機 callback，避免日後返回過期框架。
            while(!runtime_.callbacks.empty() && restored.reg(Gpr::Rsp)>=((runtime_.callbacks.back().saved.reg(Gpr::Rsp)-256)&~UINT64_C(15))) {
                exceptions_.stack.resize(runtime_.callbacks.back().exception_depth);discard_callbacks(runtime_.callbacks.size()-1,state,memory);
            }
            state=restored;return std::nullopt;
        }
        auto caller=state;arg.return_to(caller,0);
        const std::array<std::uint64_t,1> parameters{buffer};begin_exception(caller,memory,0x80000026U,0,parameters);
        auto& dispatch=exceptions_.stack.back();dispatch.unwound=true;
        while(append_exception_frame(memory,dispatch)) {
            const auto& frame=dispatch.frames.back();
            if(frame.unwind.establisher==target)break;
            if(frame.unwind.establisher>target)throw HostCallFault("longjmp 目標堆疊順序不合法");
            dispatch.walk=frame.unwind.caller;dispatch.unwound=true;
        }
        if(dispatch.frames.empty() || dispatch.frames.back().unwind.establisher!=target)throw HostCallFault("longjmp 目標框架不在目前堆疊內");
        begin_unwind(state,memory,dispatch.frames.size()-1,restored.rip.value,restored.reg(Gpr::Rax));
        dispatch.target_context=restored;dispatch.restore_jump=true;return std::nullopt;
    } else if (symbol == "AddVectoredExceptionHandler" || symbol == "AddVectoredContinueHandler") {
        memory.check(GuestAddress{arg(1)}, 1, Access::Execute);
        if (vectors.size() >= 128) result = 0;
        else {
            result = exceptions_.next_handle++;
            vectors.insert(static_cast<std::uint32_t>(arg(0)) ? vectors.begin() : vectors.end(), {result, arg(1)});
        }
    } else if (symbol == "RemoveVectoredExceptionHandler" || symbol == "RemoveVectoredContinueHandler") {
        const auto found = std::find_if(vectors.begin(), vectors.end(), [&](const auto& item) { return item.first == arg(0); });
        if (found != vectors.end()) { vectors.erase(found); result = 1; }
    } else if (symbol == "RaiseFailFastException") {
        const auto record=arg(0),context=arg(1);const auto flags_value=static_cast<std::uint32_t>(arg(2));
        const unsigned size=x86_?80:152,offset=x86_?12:16;const std::uint8_t bits=x86_?32:64;
        auto captured=state;arg.return_to(captured,state.reg(Gpr::Rax));
        const auto address=captured.rip.value;
        if(context)captured=read_context(memory,context,captured);
        std::uint32_t code=0xc0000602U;std::uint64_t fault_address=(flags_value&1U)?address:0;
        if(record) {
            memory.check(GuestAddress{record},size,Access::Read);code=static_cast<std::uint32_t>(memory.load(GuestAddress{record},32));
            if(memory.load(GuestAddress{record+(x86_?16:24)},32)>15)throw HostCallFault("FailFast 例外參數過多");
            fault_address=memory.load(GuestAddress{record+offset},bits);
            if((flags_value&1U) && !fault_address)fault_address=address;
        }
        // 第二次機會致命例外不進入 vectored／frame／UnhandledExceptionFilter。
        std::ostringstream message;message<<"FailFast exception 0x"<<std::hex<<code<<" at 0x"<<fault_address<<" (context PC 0x"<<captured.rip.value<<")\n";
        const auto text=message.str();(void)write_standard_output(2,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});state=captured;return code;
    } else if (symbol == "RaiseException") {
        const auto code = static_cast<std::uint32_t>(arg(0)) & ~0x10000000U, flags_value = static_cast<std::uint32_t>(arg(1)), count = static_cast<std::uint32_t>(arg(2));
        if (count > 15 || (flags_value & ~1U)) throw HostCallFault("RaiseException 參數或旗標不合法");
        std::vector<std::uint64_t> parameters;
        for (unsigned i = 0; i < count; ++i) parameters.push_back(memory.load(GuestAddress{arg(3)+i*(x86_?4U:8U)},x86_?32:64));
        auto caller = state; arg.return_to(caller, 0);
        if(emit_debug_event(caller,memory,debugger::Exception{code,flags_value,0,caller.rip.value,parameters,true})!=debugger::handled)
            begin_exception(caller, memory, code, flags_value, parameters);
        state = caller; return std::nullopt;
    } else if (symbol == "RtlCaptureContext" || symbol=="__crtCapturePreviousContext") {
        auto caller = state; arg.return_to(caller,state.reg(Gpr::Rax));
        // 共用入口已彈出 CRT wrapper；再依實際 unwind metadata 取得上一層框架。
        if(symbol=="__crtCapturePreviousContext" && !x86_) {
            if(const auto* image=unwind_image(memory,caller.rip.value);image && lookup_function(*image,caller.rip.value))
                caller=virtual_unwind(memory,*image,caller.rip.value,caller,0).caller;
        }
        write_context(memory,arg(0),caller);
    } else if (symbol == "RtlPcToFileHeader") {
        if(const auto* image=module_image(arg(0),true))result=image->base;
        memory.store(GuestAddress{arg(1)},x86_?32:64,result);
    } else if (symbol == "RtlLookupFunctionEntry") {
        if(arg(2))memory.check(GuestAddress{arg(2)},216,Access::Write);
        if (const auto* image=unwind_image(memory,arg(0));image) {
            if(const auto* function=lookup_function(*image,arg(0))) {
                memory.store(GuestAddress{arg(1)}, 64, image->base);
                result = image->function_table + static_cast<std::uint64_t>(function - image->functions.data()) * 12;
            }
        }
    } else if (symbol == "RtlVirtualUnwind") {
        RuntimeImage leaf;leaf.base=arg(1);leaf.size=UINT64_MAX-leaf.base;
        const auto* image=arg(3)?unwind_image(memory,arg(2),arg(3)):&leaf;
        if (!image || image->base!=arg(1) || arg(0) > 3) throw HostCallFault("RtlVirtualUnwind 的 image／flags 尚未支援");
        auto context = read_context(memory, arg(4), state);
        const auto unwind = virtual_unwind(memory, *image, arg(2), context, static_cast<unsigned>(arg(0)), arg(3));
        if (unwind.function_entry != arg(3)) throw HostCallFault("RtlVirtualUnwind function entry 不符合 ControlPc");
        memory.check(GuestAddress{arg(4)}, 1232, Access::Write); memory.check(GuestAddress{arg(5)}, 8, Access::Write); memory.check(GuestAddress{arg(6)}, 8, Access::Write);
        if(arg(7))memory.check(GuestAddress{arg(7)},256,Access::Write);
        write_context(memory, arg(4), unwind.caller); memory.store(GuestAddress{arg(5)}, 64, unwind.handler_data); memory.store(GuestAddress{arg(6)}, 64, unwind.establisher); result = unwind.handler;
        // 未保存的暫存器保留傳入欄位；已保存者指向真實客體堆疊，不是複製的 CONTEXT。
        if(arg(7))for(unsigned i=0;i<16;++i){
            if(unwind.floating_sources[i])memory.store(GuestAddress{arg(7)+i*8},64,unwind.floating_sources[i]);
            if(unwind.integer_sources[i])memory.store(GuestAddress{arg(7)+128+i*8},64,unwind.integer_sources[i]);
        }
    } else if (symbol == "RtlUnwindEx" || symbol == "RtlUnwind") {
        if(x86_)throw HostCallFault("RtlUnwindEx 僅支援 AMD64；Win32 請使用 RtlUnwind");
        const auto target=arg(0),ip=arg(1),record=arg(2),value=arg(3);
        if(record && memory.load(GuestAddress{record},32)==0x80000026U){
            if(const auto* image=module_image(ip,true);image && image->guard_long_jump &&
               !std::binary_search(image->guard_long_jump_targets.begin(),image->guard_long_jump_targets.end(),static_cast<std::uint32_t>(ip-image->base)))return 0xc0000409U;
        }
        if(symbol=="RtlUnwindEx" && arg(5))memory.check(GuestAddress{arg(5)},216,Access::Write);
        const bool in_search=!exceptions_.stack.empty() && exceptions_.stack.back().stage==ExceptionStage::Search &&
            std::any_of(exceptions_.stack.back().frames.begin(),exceptions_.stack.back().frames.end(),[&](const auto& f){return f.unwind.establisher==target;});
        if(!in_search) {
            std::vector<std::uint64_t> parameters;std::uint32_t code=0xc0000027,flags_value=0;
            if(record) {
                code=static_cast<std::uint32_t>(memory.load(GuestAddress{record},32));flags_value=static_cast<std::uint32_t>(memory.load(GuestAddress{record+4},32))&1U;
                const auto count=memory.load(GuestAddress{record+24},32);if(count>15)throw HostCallFault("RtlUnwindEx 例外參數過多");
                for(std::uint64_t i=0;i<count;++i)parameters.push_back(memory.load(GuestAddress{record+32+i*8},64));
            }
            auto caller=state;arg.return_to(caller,0);begin_exception(caller,memory,code,flags_value,parameters);
            auto& walking=exceptions_.stack.back();walking.unwound=true;
            while(append_exception_frame(memory,walking)) {
                const auto& frame=walking.frames.back();if(frame.unwind.establisher==target)break;
                if(frame.unwind.establisher>target)throw HostCallFault("RtlUnwindEx 目標堆疊順序不合法");
                walking.walk=frame.unwind.caller;walking.unwound=true;
            }
        }
        auto& dispatch = exceptions_.stack.back();
        const auto frame = std::find_if(dispatch.frames.begin(), dispatch.frames.end(), [&](const auto& f) { return f.unwind.establisher == target; });
        if (frame == dispatch.frames.end()) throw HostCallFault("RtlUnwindEx 目標框架不在搜尋鏈內");
        if(record && memory.load(GuestAddress{record},32)==0x80000029U){
            const auto count=memory.load(GuestAddress{record+24},32);
            if(!count || count>15)throw HostCallFault("STATUS_UNWIND_CONSOLIDATE 參數數目不合法");
            memory.check(GuestAddress{record},32+static_cast<std::size_t>(count)*8,Access::Read);
            dispatch.consolidate_record=record;dispatch.consolidate_context=symbol=="RtlUnwindEx"?arg(4):0;
            dispatch.consolidate_origin=state;dispatch.record=record;dispatch.code=0x80000029U;
            dispatch.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{record+4},32))&1U;
        }
        begin_unwind(state, memory, static_cast<std::size_t>(frame - dispatch.frames.begin()),ip,value);return std::nullopt;
    } else if (symbol == "__C_specific_handler") {
        if (exceptions_.stack.empty()) throw HostCallFault("C specific handler 沒有例外派送狀態");
        auto& dispatch = exceptions_.stack.back();
        if (arg(0) != dispatch.record || arg(3) != dispatch.dispatcher) throw HostCallFault("C specific handler 的 DispatcherContext 不符");
        const auto* image=unwind_image(memory,memory.load(GuestAddress{dispatch.dispatcher},64),memory.load(GuestAddress{dispatch.dispatcher+16},64));
        if(!image)throw HostCallFault("C specific handler 所屬映像已不存在");
        const auto data = memory.load(GuestAddress{dispatch.dispatcher + 56}, 64), count = memory.load(GuestAddress{data}, 32);
        if (count > 4096) throw HostCallFault("SEH scope table 過大");
        const auto pc = memory.load(GuestAddress{dispatch.dispatcher}, 64) - image->base;
        const auto frame = memory.load(GuestAddress{dispatch.dispatcher + 24}, 64);
        dispatch.scopes.clear(); dispatch.scope_index = 0;
        std::vector<GuestCallback> finalizers;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto at = data + 4 + i * 16;
            SehScope scope{static_cast<std::uint32_t>(memory.load(GuestAddress{at}, 32)), static_cast<std::uint32_t>(memory.load(GuestAddress{at + 4}, 32)),
                static_cast<std::uint32_t>(memory.load(GuestAddress{at + 8}, 32)), static_cast<std::uint32_t>(memory.load(GuestAddress{at + 12}, 32))};
            if (scope.begin >= scope.end || scope.end > image->size || scope.target >= image->size || (scope.filter > 1 && scope.filter >= image->size)) throw HostCallFault("SEH scope RVA 不合法");
            dispatch.scopes.push_back(scope);
            if (dispatch.stage == ExceptionStage::Unwind && !scope.target && pc >= scope.begin && pc < scope.end) {
                const auto target = dispatch.target_ip - image->base;
                const bool target_frame=dispatch.index-1==dispatch.target_frame;
                if (!target_frame || !(target >= scope.begin && target < scope.end)) finalizers.push_back({image->base + scope.filter, {1, frame}});
            }
        }
        if (dispatch.stage == ExceptionStage::Unwind) { start_callbacks(state, memory, std::move(finalizers), Continuation::Return, 1); return std::nullopt; }
        return seh_filter_result(state, memory, 0);
    }
    arg.return_to(state, result); return std::nullopt;
}
#include "crt_x86_jump_handlers.inc"
}
