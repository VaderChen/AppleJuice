#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <bit>
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> hook_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","NotifyWinEvent",4},{"user32.dll","SetWinEventHook",7},{"user32.dll","UnhookWinEvent",1},{"user32.dll","IsWinEventHookInstalled",1},
        {"user32.dll","SetWindowsHookExA",4},{"user32.dll","SetWindowsHookExW",4},
        {"user32.dll","UnhookWindowsHookEx",1},{"user32.dll","CallNextHookEx",4},
        {"user32.dll","CallMsgFilterA",2},{"user32.dll","CallMsgFilterW",2}
    };
    return table;
}
bool WindowsServices::enter_gui_hook(GuestState& state,GuestMemory& memory,GuiHookFrame frame,
                                    std::int32_t code,std::uint64_t wparam,std::uint64_t lparam) {
    // 快照保留鏈的順序；callback 途中解除的 hook 不可再執行。
    while(frame.index<frame.handles.size() && !gui_.hooks.contains(frame.handles[frame.index]))++frame.index;
    if(frame.index==frame.handles.size())return false;
    const auto& hook=gui_.hooks.at(frame.handles[frame.index]);const auto procedure=hook.procedure;
    try{
    if(frame.type==5 && code==3 && hook.wide!=frame.wide){
        const auto source=memory.load(GuestAddress{lparam},64);std::array<std::uint8_t,80> data{};memory.read(GuestAddress{source},data);
        const auto target=allocate_gui_scratch(memory,frame.scratch,data);
        for(const auto offset:{56U,64U}){const auto pointer=memory.load(GuestAddress{source+offset},64);if(pointer<=65535)continue;
            const auto text=frame.wide?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer));std::uint64_t encoded;
            if(hook.wide)encoded=allocate_gui_scratch(memory,frame.scratch,utf16_bytes(text));else {const auto utf8=utf16_to_utf8(text);std::vector<std::uint8_t> bytes(utf8.begin(),utf8.end());bytes.push_back(0);encoded=allocate_gui_scratch(memory,frame.scratch,bytes);}
            memory.store(GuestAddress{target+offset},64,encoded);
        }
        frame.cbt_source=lparam;frame.cbt_marshaled=allocate_gui_scratch(memory,frame.scratch,{},16);memory.store(GuestAddress{frame.cbt_marshaled},64,target);memory.store(GuestAddress{frame.cbt_marshaled+8},64,memory.load(GuestAddress{lparam+8},64));lparam=frame.cbt_marshaled;
    }
    frame.wide=hook.wide;
    start_callbacks(state,memory,{{procedure,{static_cast<std::uint32_t>(code),wparam,lparam}}},Continuation::GuiHook);
    runtime_.callbacks.back().hook=std::move(frame);
    return true;
    }catch(...){release_gui_scratch(memory,state,frame.scratch);throw;}
}
bool WindowsServices::start_gui_hooks(GuestState& state,GuestMemory& memory,std::int32_t type,std::int32_t code,
                                     std::uint64_t wparam,std::uint64_t lparam,Continuation completion,std::uint64_t value,bool wide) {
    GuiHookFrame frame;frame.continuation=completion;frame.value=value;frame.type=type;frame.wide=wide;
    // Thread hooks 先於 desktop hooks，同類型依最後安裝的優先。
    for(const bool global:{false,true})for(auto it=gui_.hooks.rbegin();it!=gui_.hooks.rend();++it) {
        const auto& hook=it->second;
        if(hook.type==type && thread_desktop_object(hook.owner)==thread_desktop_object(threads_.current) && (global?hook.thread==0:hook.thread==threads_.current))frame.handles.push_back(it->first);
    }
    return enter_gui_hook(state,memory,std::move(frame),code,wparam,lparam);
}
std::optional<std::uint32_t> WindowsServices::resume_gui_hook(GuestState& state,GuestMemory& memory,
                                                            const CallbackFrame& finished,std::uint64_t result) {
    if(finished.continuation==Continuation::GuiMsgFilterSystem) {
        const WindowsCall arg(state,memory,2);
        if(!finished.value && !result && start_gui_hooks(state,memory,-1,std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(1))),0,arg(0),Continuation::GuiMsgFilterSystem,1))return std::nullopt;
        arg.return_to(state,result?1U:0U);return std::nullopt;
    }
    try{if(finished.hook.cbt_source){
        const auto source=memory.load(GuestAddress{finished.hook.cbt_marshaled},64),target=memory.load(GuestAddress{finished.hook.cbt_source},64);
        for(const auto offset:{32U,36U,40U,44U})memory.store(GuestAddress{target+offset},32,memory.load(GuestAddress{source+offset},32));
        memory.store(GuestAddress{finished.hook.cbt_source+8},64,memory.load(GuestAddress{finished.hook.cbt_marshaled+8},64));
    }
    }catch(...){release_gui_scratch(memory,state,finished.hook.scratch);
        if(finished.hook.continuation==Continuation::GuiCbtCreate)cancel_gui_creation(finished.hook.value,memory,state);
        if(finished.hook.continuation==Continuation::GuiCbtAction)cancel_gui_cbt_action(finished.hook.value,memory,state);
        throw;
    }
    release_gui_scratch(memory,state,finished.hook.scratch);
    if(finished.hook.continuation==Continuation::ReturnCallback) {
        complete_call(state,memory,result);return std::nullopt;
    }
    // 後續 modal/menu continuation 必須取得 hook 的 LRESULT，且保留 API 的
    // 原始呼叫框架；不可將其當成一般 callback queue 自動呼叫下一個 hook。
    auto continuation=finished;continuation.continuation=finished.hook.continuation;
    continuation.value=finished.hook.value;continuation.callbacks.clear();continuation.index=0;continuation.hook={};
    runtime_.callbacks.push_back(std::move(continuation));state.rip=callback_return_;state.reg(Gpr::Rax)=result;
    return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_hooks(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto dword=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    std::uint64_t result=0;const auto fail=[&](std::uint32_t error){last_error_=error;};
    if(symbol=="SetWinEventHook"){
        const auto minimum=dword(0),maximum=dword(1),flags=dword(6),identifier=dword(5);const auto thread=thread_from_identifier(identifier);
        if(!minimum || minimum>maximum || maximum>0x7fffffffU || !arg(3) || (flags&~7U))fail(87);
        else if(identifier && !thread)fail(1444);
        else if((flags&4U) || (dword(4)!=process_id() && !(dword(4)==0 && thread && threads_.items.contains(thread))))fail(50); // 僅目前程序的 out-of-context 通知；不假裝具有 desktop／DLL injection 後端。
        else if(thread && (!threads_.items.contains(thread) || threads_.items.at(thread).status==ThreadStatus::Exited))fail(1444);
        else if(gui_.event_hooks.size()>=4096)fail(8);
        else {memory.check(GuestAddress{arg(3)},1,Access::Execute);result=gui_.next_object++;gui_.event_hooks.emplace(result,GuiWinEventHook{arg(3),threads_.current,thread,minimum,maximum,flags});}
    }else if(symbol=="UnhookWinEvent"){
        const auto hook=gui_.event_hooks.find(arg(0));if(hook==gui_.event_hooks.end())fail(1404);else if(hook->second.owner!=threads_.current)fail(5);
        else {gui_.event_hooks.erase(hook);std::erase_if(gui_.event_queue,[&](const auto& event){return event.hook==arg(0);});result=1;}
    }else if(symbol=="IsWinEventHookInstalled"){
        result=std::any_of(gui_.event_hooks.begin(),gui_.event_hooks.end(),[&](const auto& entry){return dword(0)>=entry.second.minimum && dword(0)<=entry.second.maximum;});
    }else if(symbol=="NotifyWinEvent"){
        const auto event=dword(0);
        for(const auto& [handle,hook]:gui_.event_hooks){
            if(event<hook.minimum || event>hook.maximum || (hook.thread && hook.thread!=threads_.current) || (hook.flags&2U) || ((hook.flags&1U) && hook.owner==threads_.current))continue;
            if(gui_.event_queue.size()>=10000)throw HostCallFault("WinEvent 通知佇列超過上限");
            gui_.event_queue.push_back({handle,arg(1),thread_identifier(threads_.current),event,dword(2),dword(3),static_cast<std::uint32_t>(runtime_.clock_ticks/10000)});
        }
    }else if(symbol.starts_with("SetWindowsHookEx")) {
        const auto type=std::bit_cast<std::int32_t>(dword(0));const auto identifier=dword(3);const auto thread=thread_from_identifier(identifier);
        if(type < -1 || type>14 || type==8)fail(1426);
        else if(!arg(1))fail(1427);
        else if(identifier && !thread)fail(1444);
        else if(thread && thread!=threads_.current && (!threads_.items.contains(thread) || threads_.items.at(thread).status==ThreadStatus::Exited))fail(1444);
        else if(!thread && !arg(2))fail(1428);
        else if(arg(2) && !module_image(arg(2)))fail(126);
        else if(type==6 && thread)fail(1429);
        else if(type!=-1 && type!=6 && type!=5)fail(50); // 尚無事件來源的 hook 不宣稱安裝成功。
        else if(gui_.hooks.size()>=4096)fail(8);
        else {
            memory.check(GuestAddress{arg(1)},1,Access::Execute);
            result=gui_.next_object++;
            gui_.hooks.emplace(result,GuiHook{type,arg(1),arg(2),thread,threads_.current,symbol.ends_with('W')});
        }
    } else if(symbol=="UnhookWindowsHookEx") {
        if(gui_.hooks.erase(arg(0)))result=1;else fail(1404);
    } else if(symbol=="CallNextHookEx") {
        // hhk 依 Win32 契約不參與查找；鏈由此執行緒最內層 hook callback 決定。
        for(auto it=runtime_.callbacks.rbegin();it!=runtime_.callbacks.rend();++it)if(it->continuation==Continuation::GuiHook) {
            auto frame=it->hook;frame.scratch.clear();++frame.index;frame.continuation=Continuation::ReturnCallback;frame.value=0;frame.cbt_source=0;frame.cbt_marshaled=0;
            if(enter_gui_hook(state,memory,std::move(frame),std::bit_cast<std::int32_t>(dword(1)),arg(2),arg(3)))return std::nullopt;
            break;
        }
    } else {
        if(!arg(0))fail(87);
        else {
            memory.check(GuestAddress{arg(0)},x86_?28:48,Access::Read);
            const auto code=std::bit_cast<std::int32_t>(dword(1));
            if(start_gui_hooks(state,memory,6,code,0,arg(0),Continuation::GuiMsgFilterSystem) ||
               start_gui_hooks(state,memory,-1,code,0,arg(0),Continuation::GuiMsgFilterSystem,1))return std::nullopt;
        }
    }
    arg.return_to(state,result);return std::nullopt;
}
}
