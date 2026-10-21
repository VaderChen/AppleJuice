#include "x64arm/windows_services.hpp"
#include <algorithm>

namespace x64arm {
bool WindowsServices::gui_message_matches(const GuiMessage& message,std::uint64_t thread,std::uint64_t window,std::uint32_t minimum,std::uint32_t maximum) const {
    if(x86_ && window==UINT32_MAX)window=UINT64_MAX;
    const auto target=gui_.windows.find(message.window);
    const auto owner=message.window?(target==gui_.windows.end()?0:target->second.thread):(message.thread?message.thread:1);
    if(owner!=thread)return false;
    if(message.message==18)return true;
    if(window==UINT64_MAX && message.window)return false;
    if(window && window!=UINT64_MAX){
        auto child=message.window;bool belongs=false;
        for(unsigned depth=0;child && depth<64;++depth){
            if(child==window){belongs=true;break;}
            const auto entry=gui_.windows.find(child);if(entry==gui_.windows.end())break;child=entry->second.parent;
        }
        if(!belongs)return false;
    }
    return (!minimum && !maximum) || (message.message>=minimum && message.message<=maximum);
}
bool WindowsServices::pending_gui_send(std::uint64_t thread,bool callbacks) const {
    if(std::any_of(gui_.mouse_inputs.begin(),gui_.mouse_inputs.end(),[&](const auto& item){return item.second.thread==thread && !item.second.active;}))return true;
    if(callbacks && std::any_of(gui_.send_callbacks.begin(),gui_.send_callbacks.end(),[&](const auto& event){return event.thread==thread;}))return true;
    if(std::any_of(gui_.event_queue.begin(),gui_.event_queue.end(),[&](const auto& event){const auto hook=gui_.event_hooks.find(event.hook);return hook!=gui_.event_hooks.end() && hook->second.owner==thread;}))return true;
    return std::any_of(gui_.sent_messages.begin(),gui_.sent_messages.end(),[&](const auto& entry){return entry.second.receiver==thread && !entry.second.active && !entry.second.done;});
}
void WindowsServices::cancel_gui_send(std::uint64_t token,std::uint32_t error) {
    const auto found=gui_.sent_messages.find(token);if(found==gui_.sent_messages.end())return;
    if(found->second.done){found->second.active=false;if(!found->second.sender)gui_.sent_messages.erase(found);return;}
    if(found->second.kind!=GuiSentMessage::Kind::Synchronous){queue_gui_send_callback(found->second,0);gui_.sent_messages.erase(found);return;}
    if(!found->second.sender && !found->second.abandoned && !found->second.rpc)discard_gui_control_notify(found->second.message);
    found->second.error=error;found->second.result=0;found->second.done=true;found->second.active=false;
}
void WindowsServices::cancel_gui_thread(std::uint64_t thread,GuestMemory& memory,const GuestState& state) {
    std::vector<std::uint64_t> text_operations;
    for(const auto& [token,operation]:gui_text_thunks_)if(operation.thread==thread)text_operations.push_back(token);
    for(const auto token:text_operations)discard_gui_text_thunk(memory,state,token);
    std::erase_if(gui_.deferred_positions,[&](const auto& item){return item.second.thread==thread;});
    std::vector<std::uint64_t> positions;
    for(const auto& [id,operation]:gui_.position_operations)if(operation.thread==thread)positions.push_back(id);
    for(const auto id:positions)discard_gui_window_pos(memory,state,id);
    std::vector<std::uint64_t> drags;
    for(const auto& [id,operation]:gui_.drag_detections)if(operation.thread==thread)drags.push_back(id);
    for(const auto id:drags)discard_gui_drag(memory,state,id);
    std::vector<std::uint64_t> moves;for(const auto& [id,operation]:gui_.move_sizes)if(operation.thread==thread)moves.push_back(id);
    for(const auto id:moves)discard_gui_move_size(memory,state,id);
    std::erase_if(gui_.mouse_inputs,[&](const auto& item){return item.second.thread==thread;});
    release_raw_input(0,0,thread);
    std::erase_if(gui_.hotkeys,[&](const auto& entry){return entry.thread==thread;});
    gui_.keyboard_translation.erase(thread);
    std::erase_if(gui_.send_callbacks,[&](const auto& entry){return entry.thread==thread;});
    std::erase_if(gui_.async_sends,[&](const auto& entry){return entry.second.thread==thread;});
    std::vector<std::uint64_t> creations;
    for(const auto& [id,operation]:gui_.creation_operations)if(operation.thread==thread)creations.push_back(id);
    for(const auto id:creations)cancel_gui_creation(id,memory,state);
    std::vector<std::uint64_t> actions;
    for(const auto& [id,action]:gui_.cbt_actions)if(action.thread==thread)actions.push_back(id);
    for(const auto id:actions)cancel_gui_cbt_action(id,memory,state);
    gui_.cbt_replay.erase(thread);
    gui_.input_defaults.erase(thread);
    gui_.input_windows.erase(thread);gui_.input_window_targets.erase(thread);
    for(const auto& [handle,context]:gui_.input_contexts)if(context.thread==thread)imm_release_context_storage(memory,state,handle);
    std::erase_if(gui_.input_contexts,[&](const auto& entry){return entry.second.thread==thread;});
    std::erase_if(gui_.input_associations,[&](const auto& entry){return entry.second && !gui_.input_contexts.contains(entry.second);});
    std::erase_if(gui_.input_borrows,[&](const auto& entry){return !gui_.input_contexts.contains(entry.first.second);});
    std::erase_if(gui_.event_hooks,[&](const auto& entry){return entry.second.owner==thread;});
    std::erase_if(gui_.event_queue,[&](const auto& event){return !gui_.event_hooks.contains(event.hook);});
    std::erase_if(gui_.timers,[&](const auto& entry){return entry.second.thread==thread;});
    std::vector<std::uint64_t> fonts;
    for(const auto& [token,operation]:font_enumerations_)if(operation.thread==thread)fonts.push_back(token);
    for(const auto token:fonts)discard_font_enumeration(token,memory,state);
    runtime_.activation_stacks.erase(thread);
    std::erase_if(gui_.hooks,[&](const auto& entry){return entry.second.owner==thread || entry.second.thread==thread;});
    for(auto it=gui_.sent_messages.begin();it!=gui_.sent_messages.end();){
        auto& request=it->second;
        if(request.kind!=GuiSentMessage::Kind::Synchronous){
            if(request.receiver==thread){queue_gui_send_callback(request,0);it=gui_.sent_messages.erase(it);continue;}
            // 通知已交給接收者；發送執行緒結束只撤掉它的回呼。
            if(request.sender==thread){request.sender=0;request.callback=0;}
            ++it;continue;
        }
        if(request.sender==thread){
            if(request.active && request.receiver!=thread){request.sender=0;if(request.timed || request.rpc)request.abandoned=true;++it;}
            else {if(!request.sender && !request.rpc)discard_gui_control_notify(request.message);it=gui_.sent_messages.erase(it);}
        }else {if(request.receiver==thread){
            const auto token=it->first;const auto next=std::next(it);const bool abandoned=request.abandoned;
            cancel_gui_send(token,1400);
            if(!gui_.sent_messages.contains(token)){it=next;continue;}
            if(abandoned){it=gui_.sent_messages.erase(it);continue;}
        }++it;}
    }
    std::erase_if(gui_.message_wait_deadlines,[&](const auto& entry){return entry.first.first==thread;});
    cancel_power_notifications(0,thread);
    std::erase_if(gui_.queue,[&](const auto& message){return !message.window && message.thread==thread;});
    std::vector<std::uint64_t> windows;
    for(const auto& [handle,window]:gui_.windows)if(window.thread==thread)windows.push_back(handle);
    for(const auto handle:windows)if(gui_.windows.contains(handle))erase_window(handle);
}
std::uint64_t WindowsServices::gui_send_deadline(const GuiSentMessage& request) const {
    auto deadline=request.deadline;if(!request.timed)return deadline;
    const auto receiver=threads_.items.find(request.receiver);if(receiver==threads_.items.end())return deadline;
    const auto active_until=receiver->second.gui_pump_time>UINT64_MAX-50000000?UINT64_MAX:receiver->second.gui_pump_time+50000000;
    if(request.timeout_flags&8U)deadline=std::max(deadline,active_until);
    if(request.timeout_flags&2U)deadline=std::min(deadline,active_until);
    return deadline;
}
bool WindowsServices::route_gui_send(GuestState& state,GuestMemory& memory,const GuiMessage& message,bool wide,bool timed,std::uint32_t flags,std::uint32_t timeout,std::uint64_t result_pointer) {
    // 用實際 API frame 辨識重試；收到反向 SendMessage 的 callback 仍可再發出巢狀 send。
    auto found=std::find_if(gui_.sent_messages.begin(),gui_.sent_messages.end(),[&](const auto& entry){
        const auto& request=entry.second;
        return !request.rpc && request.kind==GuiSentMessage::Kind::Synchronous && request.sender==threads_.current && request.sender_stack==state.reg(Gpr::Rsp) && request.sender_rip==state.rip.value;
    });
    if(found==gui_.sent_messages.end()){
        const auto target=gui_.windows.find(message.window);
        if(target==gui_.windows.end() || target->second.thread==threads_.current)return false;
        const auto receiver=threads_.items.find(target->second.thread);
        if(receiver==threads_.items.end() || receiver->second.status==ThreadStatus::Exited){last_error_=1400;complete_call(state,memory,0);return true;}
        if(gui_.sent_messages.size()>=8192){last_error_=1816;complete_call(state,memory,0);return true;}
        GuiSentMessage request;request.message=message;request.sender=threads_.current;request.receiver=target->second.thread;
        request.sender_stack=state.reg(Gpr::Rsp);request.sender_rip=state.rip.value;request.wide=wide;
        request.timed=timed;request.timeout_flags=flags;request.result_pointer=result_pointer;
        if(timed && timeout)request.deadline=runtime_.clock_ticks>UINT64_MAX-static_cast<std::uint64_t>(timeout)*10000?UINT64_MAX:runtime_.clock_ticks+static_cast<std::uint64_t>(timeout)*10000;
        found=gui_.sent_messages.emplace(gui_.next_object++,std::move(request)).first;
    }
    if(found->second.done){
        const auto result=found->second.result;const auto error=found->second.error;
        if(error)last_error_=error;
        if(found->second.timed && found->second.result_pointer)memory.store(GuestAddress{found->second.result_pointer},x86_?32:64,error?0:result);
        const auto returned=found->second.timed?(error?0:1):result;
        if(found->second.active){found->second.sender=0;found->second.abandoned=true;found->second.result_pointer=0;}
        else gui_.sent_messages.erase(found);
        threads_.items.at(threads_.current).wait={};
        complete_call(state,memory,returned);return true;
    }
    const auto deadline=gui_send_deadline(found->second);
    if(found->second.timed && deadline<=runtime_.clock_ticks){
        if(found->second.result_pointer)memory.store(GuestAddress{found->second.result_pointer},x86_?32:64,0);
        if(found->second.active){found->second.sender=0;found->second.abandoned=true;}else gui_.sent_messages.erase(found);
        threads_.items.at(threads_.current).wait={};last_error_=1460;complete_call(state,memory,0);return true;
    }
    if(!(found->second.timeout_flags&1U) && dispatch_gui_send(state,memory,0,false))return true;
    auto& sender=threads_.items.at(threads_.current);
    sender.wait={};sender.wait.kind=ThreadWaitKind::GuiSend;sender.wait.handles={found->first};sender.wait.deadline=deadline;sender.status=ThreadStatus::Waiting;
    schedule(state,memory);return true;
}
bool WindowsServices::dispatch_gui_send(GuestState& state,GuestMemory& memory,std::uint64_t modal,bool callbacks,bool wait_message) {
    if(const auto thread=threads_.items.find(threads_.current);thread!=threads_.items.end())thread->second.gui_pump_time=runtime_.clock_ticks;
    if(callbacks)for(auto it=gui_.send_callbacks.begin();it!=gui_.send_callbacks.end();){
        const auto thread=threads_.items.find(it->thread);
        if(thread==threads_.items.end() || thread->second.status==ThreadStatus::Exited){it=gui_.send_callbacks.erase(it);continue;}
        if(it->thread!=threads_.current){++it;continue;}
        const auto event=*it;gui_.send_callbacks.erase(it);
        start_callbacks(state,memory,{{event.procedure,{event.window,event.message,event.data,event.result}}},wait_message?Continuation::GuiWaitMessage:Continuation::GuiWinEvent,modal);return true;
    }
    for(auto it=gui_.event_queue.begin();it!=gui_.event_queue.end();){
        const auto hook=gui_.event_hooks.find(it->hook);if(hook==gui_.event_hooks.end()){it=gui_.event_queue.erase(it);continue;}
        if(hook->second.owner!=threads_.current){++it;continue;}
        const auto event=*it;const auto procedure=hook->second.procedure;gui_.event_queue.erase(it);
        start_callbacks(state,memory,{{procedure,{event.hook,event.event,event.window,event.object,event.child,event.thread,event.time}}},wait_message?Continuation::GuiWaitMessage:Continuation::GuiWinEvent,modal);return true;
    }
    for(auto it=gui_.sent_messages.begin();it!=gui_.sent_messages.end();){
        const auto token=it->first;auto& request=it->second;++it;
        if(request.receiver!=threads_.current || request.active || request.done)continue;
        if(request.rpc){
            request.active=true;request.resume_modal=modal;request.resume_wait=wait_message;
            try {start_callbacks(state,memory,{*request.rpc},Continuation::GuiSentMessage,token);}
            catch(...){cancel_gui_send(token,995);throw;}
            return true;
        }
        const auto target=gui_.windows.find(request.message.window);
        if(target==gui_.windows.end() || target->second.thread!=threads_.current){cancel_gui_send(token,1400);continue;}
        const auto message=request.message;
        request.active=true;request.resume_modal=modal;request.resume_wait=wait_message;
        if(begin_gui_text_thunk(state,memory,message,request.wide,Continuation::GuiSentMessage,token))return true;
        request.active=false;
        if(const auto result=default_gui_control(memory,message,request.wide)){finish_gui_send(memory,token,*result);continue;}
        const auto procedure=target->second.procedure;
        if(!procedure){finish_gui_send(memory,token,0);continue;}
        ++gui_.dispatched;request.active=true;request.resume_modal=modal;request.resume_wait=wait_message;
        start_callbacks(state,memory,{{procedure,{message.window,message.message,message.wparam,message.lparam}}},Continuation::GuiSentMessage,token);
        return true;
    }
    return dispatch_gui_mouse(state,memory,modal,wait_message);
}
std::optional<std::uint32_t> WindowsServices::resume_gui_send(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t result) {
    const auto found=gui_.sent_messages.find(frame.value);std::uint64_t modal=0;bool wait_message=false;
    if(found!=gui_.sent_messages.end()){
        auto& request=found->second;modal=request.resume_modal;wait_message=request.resume_wait;
        if(!request.done && (request.timeout_flags&0x20U) && !gui_.windows.contains(request.message.window)){request.error=1400;result=0;}
        finish_gui_send(memory,frame.value,result);
    }
    // Get/Peek/MsgWait 與 blocked Send 會重新執行原 API；模態迴圈直接續接，不能重建 DialogBox。
    if(modal)return advance_gui_modal(state,memory,modal);
    if(wait_message)complete_call(state,memory,1);
    return std::nullopt;
}
void WindowsServices::wait_gui_input(GuestState& state,GuestMemory& memory,ThreadWait wait,std::uint64_t modal) {
    if(modal)start_callbacks(state,memory,{},Continuation::GuiModalDispatch,modal);
    auto& thread=threads_.items.at(threads_.current);wait.kind=ThreadWaitKind::GuiInput;
    if(!runtime_.callbacks.empty() && state.rip==resolve("internal-gui-drag","ReadMessage")){
        const auto& frame=runtime_.callbacks.back();
        if(frame.continuation==Continuation::GuiDragDetection)wait.gui_drag_detection=frame.value;
    }
    if(!runtime_.callbacks.empty() && state.rip==resolve("internal-window-interaction","ReadMessage") && runtime_.callbacks.back().continuation==Continuation::GuiMoveSize)wait.gui_move_size=runtime_.callbacks.back().value;
    thread.wait=std::move(wait);thread.status=ThreadStatus::Waiting;schedule(state,memory);
}
}
