#include "x64arm/windows_services.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::size_t send_limit=8192;
bool pointer_message(std::uint32_t message,std::uint64_t wparam) {
    // WM_USER 以下的系統訊息與標準控制項訊息由 Windows 定義指標生命週期。
    // 判斷訊息契約，不把碰巧落在客體位址範圍的整數當成指標。
    if(message==0x219)return (wparam&0x8000)!=0; // WM_DEVICECHANGE
    if(message==0x218)return wparam==0x8013; // PBT_POWERSETTINGCHANGE
    switch(message){
        case 1:case 12:case 13:case 0x1a:case 0x1b: // CREATE／文字／設定
        case 0x24:case 0x2b:case 0x2c:case 0x2d:case 0x39: // owner draw
        case 0x46:case 0x47:case 0x4a:case 0x4e:case 0x53:case 0x7c:case 0x7d:
        case 0x81:case 0x83:case 0x87: // nonclient／MSG
        case 0xb0:case 0xb2:case 0xb3:case 0xb4:case 0xc2:case 0xc4:case 0xcb: // Edit
        case 0xe3:case 0xe9:case 0xea:case 0xeb: // Scrollbar
        case 0x140:case 0x143:case 0x145:case 0x148:case 0x14a:case 0x14c:case 0x14d:case 0x152:case 0x158:case 0x164: // ComboBox
        case 0x180:case 0x181:case 0x189:case 0x18c:case 0x18d:case 0x18f:case 0x191:case 0x192:case 0x196:case 0x198:case 0x1a2: // ListBox
        case 0x213:case 0x214:case 0x216:case 0x220:case 0x229:
        case 0x22a:case 0x22b:case 0x22c:case 0x22d:case 0x22e:
        case 0x2e0:case 0x2e4:case 0x30c:case 0x33f:return true;
        default:return false;
    }
}
}

void WindowsServices::queue_gui_send_callback(const GuiSentMessage& request,std::uint64_t result) {
    const auto sender=threads_.items.find(request.sender);
    if(request.kind==GuiSentMessage::Kind::Callback && request.callback && sender!=threads_.items.end() && sender->second.status!=ThreadStatus::Exited)
        gui_.send_callbacks.push_back({request.sender,request.callback,request.message.window,request.callback_data,result,request.message.message});
}
void WindowsServices::finish_gui_send(GuestMemory& memory,std::uint64_t token,std::uint64_t result) {
    const auto found=gui_.sent_messages.find(token);if(found==gui_.sent_messages.end())return;
    auto& request=found->second;
    if(request.device_owner)finish_device_notification(threads_.items.at(threads_.current).context,memory,request.device_owner);
    if(request.kind!=GuiSentMessage::Kind::Synchronous){
        queue_gui_send_callback(request,result);
        gui_.sent_messages.erase(found);return;
    }
    if(!request.done)request.result=result;request.done=true;request.active=false;
    if(!request.sender){if(!request.abandoned && !request.rpc)finish_gui_control_notify(memory,request.message,result);gui_.sent_messages.erase(found);}
}

std::optional<std::uint32_t> WindowsServices::invoke_gui_async(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall call(state,memory,service);
    if(service.symbol=="WaitMessage"){
        pump_gui(memory,false);
        const bool pending=pending_gui_send(threads_.current);
        if(dispatch_gui_send(state,memory,0,true,true))return std::nullopt;
        bool fresh=false;
        for(auto& message:gui_.queue)if(gui_message_matches(message,threads_.current)){fresh|=!message.queue_seen;message.queue_seen=true;}
        if(pending || fresh){call.return_to(state,1);return std::nullopt;}
        ThreadWait wait;wait.gui_new_only=true;wait_gui_input(state,memory,std::move(wait));return std::nullopt;
    }
    const auto message=static_cast<std::uint32_t>(call(1));
    if(pointer_message(message,call(2))){last_error_=1159;call.return_to(state,0);return std::nullopt;}
    GuiAsyncSend operation;operation.message={call(0),call(2),call(3),message};operation.wide=service.symbol.ends_with('W');operation.thread=threads_.current;
    if(service.symbol.starts_with("SendMessageCallback")){operation.kind=GuiSentMessage::Kind::Callback;operation.callback=call(4);operation.data=call(5);}
    if(operation.callback && !handles(GuestAddress{operation.callback}))memory.check(GuestAddress{operation.callback},1,Access::Execute);
    if(call(0)==0xffff){
        const auto desktop=thread_desktop_object(threads_.current);
        for(const auto& [handle,window]:gui_.windows)if(!(window.style&0x40000000U) && window.desktop_object==desktop && !window.destroying)operation.windows.push_back(handle);
    }else if(gui_.windows.contains(call(0)))operation.windows.push_back(call(0));
    else {last_error_=1400;call.return_to(state,0);return std::nullopt;}
    if(gui_.sent_messages.size()+gui_.send_callbacks.size()+gui_.async_sends.size()+operation.windows.size()>send_limit){last_error_=1816;call.return_to(state,0);return std::nullopt;}
    const auto token=gui_.next_object++;gui_.async_sends.emplace(token,std::move(operation));
    return advance_gui_async(state,memory,token);
}

std::optional<std::uint32_t> WindowsServices::advance_gui_async(GuestState& state,GuestMemory& memory,std::uint64_t token) {
    const auto found=gui_.async_sends.find(token);
    if(found==gui_.async_sends.end()){complete_call(state,memory,0);return std::nullopt;}
    auto& operation=found->second;
    while(operation.index<operation.windows.size()){
        const auto handle=operation.windows[operation.index++];const auto target=gui_.windows.find(handle);
        if(target==gui_.windows.end())continue;
        auto message=operation.message;message.window=handle;
        if(target->second.thread==threads_.current){
            // 借用共用 SendMessage，使 stock control、DefWindowProc 與真正客體 WNDPROC 走相同路徑。
            start_callbacks(state,memory,{{resolve("user32.dll",operation.wide?"SendMessageW":"SendMessageA").value,{handle,message.message,message.wparam,message.lparam}}},Continuation::GuiAsyncSend,token);return std::nullopt;
        }
        const auto receiver=threads_.items.find(target->second.thread);
        if(receiver==threads_.items.end() || receiver->second.status==ThreadStatus::Exited)continue;
        if(gui_.sent_messages.size()+gui_.send_callbacks.size()+gui_.async_sends.size()>=send_limit){gui_.async_sends.erase(found);last_error_=1816;complete_call(state,memory,0);return std::nullopt;}
        GuiSentMessage request;request.message=message;request.sender=threads_.current;request.receiver=target->second.thread;request.wide=operation.wide;
        request.kind=operation.kind;request.callback=operation.callback;request.callback_data=operation.data;
        gui_.sent_messages.emplace(gui_.next_object++,std::move(request));
    }
    gui_.async_sends.erase(found);complete_call(state,memory,1);return std::nullopt;
}

std::optional<std::uint32_t> WindowsServices::resume_gui_async(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t result) {
    const auto found=gui_.async_sends.find(frame.value);
    if(found==gui_.async_sends.end()){complete_call(state,memory,0);return std::nullopt;}
    const auto& operation=found->second;
    if(frame.continuation==Continuation::GuiAsyncSend && operation.callback){
        const auto handle=operation.windows[operation.index-1];
        start_callbacks(state,memory,{{operation.callback,{handle,operation.message.message,operation.data,result}}},Continuation::GuiAsyncCallback,frame.value);return std::nullopt;
    }
    return advance_gui_async(state,memory,frame.value);
}
}
