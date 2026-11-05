#include "x64arm/windows_services.hpp"
#include "x64arm/process_identity.hpp"
#include "gui_message_contract.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t gui_message_family=3,send_operation=1,post_operation=2;
constexpr std::uint32_t wide_mode=0x100;
std::uint64_t signed_dword(std::uint64_t value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));
}
}

bool WindowsServices::invoke_remote_gui_message(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const auto symbol=service.symbol;
    const bool post=symbol=="PostMessageA" || symbol=="PostMessageW";
    if(!post && symbol!="SendMessageA" && symbol!="SendMessageW")return false;
    const WindowsCall call(state,memory,service);
    const GuiMessage message{call(0),call(2),call(3),static_cast<std::uint32_t>(call(1))};
    const auto pending=std::find_if(gui_.sent_messages.begin(),gui_.sent_messages.end(),[&](const auto& item){
        const auto& request=item.second;
        return request.remote_sequence && !request.remote_server && request.sender==threads_.current &&
            request.sender_stack==state.reg(Gpr::Rsp) && request.sender_rip==state.rip.value;
    });
    if(pending==gui_.sent_messages.end() && (!message.window || message.window==0xffff || gui_.windows.contains(message.window)))return false;
    // 本地服務不得在承諾不排程的路徑建立遠端工作；回到完整服務後才提交。
    if(local_plain_service_)throw LocalServiceRefused{};
    if(pending==gui_.sent_messages.end()){
        const auto fail=[&](std::uint32_t error){last_error_=error;call.return_to(state,0);return true;};
        const auto target=shared_gui_window(message.window,false);
        if(!target || target->process==process_id())return fail(1400);
        if(!allowed_gui_window(*target))return fail(5);
        // 系統指標訊息需要資料複製協定，不能把另一程序的位址交給 WNDPROC。
        if(gui_pointer_message(message.message,message.wparam))return fail(post?1159:50);
        if(gui_.sent_messages.size()>=8192)return fail(1816);
        const auto identity=process_identity::query(target->process);
        if(identity.error || identity.exited || !identity.identity)return fail(1400);
        GuiSentMessage request;request.message=message;request.sender=threads_.current;
        request.sender_stack=state.reg(Gpr::Rsp);request.sender_rip=state.rip.value;request.wide=symbol.ends_with('W');
        request.remote_process=target->process;request.remote_birth=identity.identity.birth;
        const auto error=begin_remote_family_request(request.remote_process,request.remote_birth,gui_message_family,
            post?post_operation:send_operation,{message.window,message.message,message.wparam,message.lparam,0},
            (x86_?32U:64U)|(request.wide?wide_mode:0),-1,-1,request.remote_sequence,0);
        if(error)return fail(error);
        gui_.sent_messages.emplace(gui_.next_object++,std::move(request));
    }
    // 使用相同的等待、反向 SendMessage 與執行緒切換契約。
    return route_gui_send(state,memory,message,symbol.ends_with('W'));
}

std::uint32_t WindowsServices::receive_remote_gui_message(std::uint32_t pid,std::uint64_t birth,std::uint64_t sequence,
    std::uint32_t operation,std::uint32_t timeout,const std::array<std::uint64_t,5>& values,std::uint32_t mode) {
    if((operation!=send_operation && operation!=post_operation) || timeout || values[1]>UINT32_MAX || values[4] ||
       ((mode&~wide_mode)!=32U && (mode&~wide_mode)!=64U))return 87;
    const auto target=gui_.windows.find(values[0]);
    if(target==gui_.windows.end() || target->second.destroying)return 1400;
    const auto thread=threads_.items.find(target->second.thread);
    if(thread==threads_.items.end() || thread->second.status==ThreadStatus::Exited)return 1400;
    const auto message=static_cast<std::uint32_t>(values[1]);
    if(gui_pointer_message(message,values[2]))return operation==post_operation?1159:50;
    for(const auto& [token,request]:gui_.sent_messages)
        if(request.remote_server && request.remote_process==pid && request.remote_birth==birth && request.remote_sequence==sequence)return 0;
    if(gui_.sent_messages.size()>=8192 || (operation==post_operation && gui_.queue.size()>=10000))return 1816;
    GuiSentMessage request;request.remote_server=true;request.remote_process=pid;request.remote_birth=birth;request.remote_sequence=sequence;
    request.receiver=target->second.thread;request.wide=(mode&wide_mode)!=0;
    const bool sender32=(mode&~wide_mode)==32;
    request.message={values[0],sender32||x86_?static_cast<std::uint32_t>(values[2]):values[2],
        x86_?static_cast<std::uint32_t>(values[3]):sender32?signed_dword(values[3]):values[3],message};
    if(operation==post_operation){enqueue_gui_message(request.message);request.done=true;request.result=1;}
    gui_.sent_messages.emplace(gui_.next_object++,std::move(request));
    return 0;
}

void WindowsServices::reply_remote_gui_message(std::uint64_t token) {
    const auto found=gui_.sent_messages.find(token);if(found==gui_.sent_messages.end())return;
    auto& request=found->second;
    if(!request.remote_server || !request.done)return;
    if(!request.remote_replied){
        const auto error=complete_remote_family_request(request.remote_process,request.remote_sequence,request.remote_birth,
            request.error,{request.result,0,0,0,0},x86_?32:64);
        if(!error || process_identity::query(request.remote_process).gone({request.remote_process,request.remote_birth}))request.remote_replied=true;
    }
    // ReplyMessage 可以早於 WNDPROC 結束，回呼 frame 仍須保留。
    if(request.remote_replied && !request.active)gui_.sent_messages.erase(found);
}

void WindowsServices::cancel_remote_gui_message(std::uint32_t pid,std::uint64_t birth,std::uint64_t sequence) {
    for(auto it=gui_.sent_messages.begin();it!=gui_.sent_messages.end();++it){
        auto& request=it->second;
        if(!request.remote_server || request.remote_process!=pid || request.remote_birth!=birth || request.remote_sequence!=sequence)continue;
        if(request.active){request.remote_replied=true;request.abandoned=true;}
        else gui_.sent_messages.erase(it);
        return;
    }
}

void WindowsServices::poll_remote_gui_messages(GuestMemory& memory) {
    // poll 會接收新請求或取消；快照 token，不保留跨 poll 的 map iterator。
    std::vector<std::uint64_t> tokens;
    for(const auto& [token,request]:gui_.sent_messages)if(request.remote_sequence)tokens.push_back(token);
    for(const auto token:tokens){
        auto found=gui_.sent_messages.find(token);if(found==gui_.sent_messages.end())continue;
        if(found->second.remote_server){
            const auto& request=found->second;
            if(process_identity::query(request.remote_process).gone({request.remote_process,request.remote_birth})){
                cancel_remote_gui_message(request.remote_process,request.remote_birth,request.remote_sequence);continue;
            }
            reply_remote_gui_message(token);continue;
        }
        if(found->second.done)continue;
        const auto result=poll_remote_family_request(memory,found->second.remote_process,found->second.remote_sequence);
        found=gui_.sent_messages.find(token);if(found==gui_.sent_messages.end() || !result.ready)continue;
        auto& request=found->second;request.done=true;request.error=result.error==87?1400:result.error;
        if(!request.error && result.mode!=32 && result.mode!=64)request.error=13;
        request.result=request.error?0:result.mode==32?signed_dword(result.values[0]):result.values[0];
    }
}

bool WindowsServices::remote_gui_messages_waiting()const {
    return std::any_of(gui_.sent_messages.begin(),gui_.sent_messages.end(),[](const auto& item){
        return item.second.remote_server && !item.second.remote_replied;
    });
}
}
