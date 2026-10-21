#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
namespace {
constexpr std::uint32_t invalid=0x80070057U,unexpected=0x8000ffffU,disconnected=0x80010108U,no_memory=0x8007000eU;
constexpr unsigned effect_offset=48,error_offset=52,class_offset=64;
std::uint64_t drag_method(const GuestMemory& memory,std::uint64_t object,unsigned slot){
    const unsigned unit=memory.execution_mode()==ExecutionMode::X86?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(!object)throw HostCallFault("OLE 拖放物件為空");const auto table=memory.load(GuestAddress{object},bits);
    if(!table || table>UINT64_MAX-slot*unit)throw HostCallFault("OLE 拖放 vtable 位址不合法");
    const auto target=memory.load(GuestAddress{table+slot*unit},bits);if(!target)throw HostCallFault("OLE 拖放方法為空");return target;
}
bool live_target(const GuiState& gui,const ole_drag::Target& target){
    const auto window=gui.windows.find(target.window);
    const auto registration=gui.drop_targets.find(target.window);
    return window!=gui.windows.end() && !window->second.destroying && registration!=gui.drop_targets.end() &&
        registration->second.object==target.object && registration->second.thread==target.thread && registration->second.generation==target.generation;
}
}
std::span<const WindowsServiceDescriptor> ole_drag_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ole32.dll","DoDragDrop",4,0,0,4},{"internal-ole-drag","Run",4,0,0,4},{"internal-ole-drag","Read",5},
        {"internal-ole-drag","Wait",1},{"internal-ole-drag","TargetCall",1},{"internal-ole-drag","TargetDispatch",1}};
    return table;
}
ole_drag::Input WindowsServices::ole_drag_input(const OleDragOperation& op,bool physical) const {
    ole_drag::Input input;input.point=physical?gui_.pointer_position:op.input.point;
    const auto& keys=physical?gui_.async_keyboard:gui_.keyboard;input.keys=ole_drag::key_state(keys);
    input.escape=(keys[27]&0x80U) || gui_.escape_presses!=op.escape_presses;
    auto handle=gui_window_from_point(gui_,input.point[0],input.point[1],thread_desktop_object(op.thread));
    for(unsigned depth=0;handle && depth<64;++depth){
        const auto window=gui_.windows.find(handle);if(window==gui_.windows.end() || window->second.destroying)break;
        if(const auto registered=gui_.drop_targets.find(handle);registered!=gui_.drop_targets.end()){
            const auto& target=registered->second;input.target={handle,target.object,target.thread,target.generation};break;
        }handle=window->second.parent;
    }return input;
}
std::uint64_t WindowsServices::ole_drag_cursor(std::uint32_t effect){
    effect&=ole_drag::effects;effect=effect&ole_drag::copy?ole_drag::copy:effect&ole_drag::move?ole_drag::move:effect&ole_drag::link?ole_drag::link:0;
    if(const auto found=gui_.ole_drag_cursors.find(effect);found!=gui_.ole_drag_cursors.end())return found->second;
    GuiIcon icon;icon.width=icon.height=32;icon.is_icon=false;icon.alpha=icon.shared=true;icon.rgba.resize(32*32);
    const auto pixel=[&](unsigned x,unsigned y,std::uint32_t color){icon.rgba[y*32+x]=color;};
    // RGBA 位元組沿用 GuiIcon／NSCursor 的共用通道順序。
    for(unsigned y=1;y<23;++y)for(unsigned x=1;x<=std::min(y/2+1,13U);++x)
        pixel(x,y,x==1 || x==y/2+1 || y==22?0xff000000U:0xffffffffU);
    for(unsigned y=14;y<26;++y)for(unsigned x=7;x<11;++x)pixel(x,y,x==7 || x==10 || y==25?0xff000000U:0xffffffffU);
    for(unsigned y=17;y<31;++y)for(unsigned x=17;x<31;++x){
        const int dx=static_cast<int>(x)-24,dy=static_cast<int>(y)-24;
        if(!effect){const auto radius=dx*dx+dy*dy;if((radius>=25 && radius<=49) || (std::abs(dx+dy)<=1 && radius<=49))pixel(x,y,0xff2020d0U);}
        else if(x==17 || x==30 || y==17 || y==30)pixel(x,y,0xff000000U);
        else {pixel(x,y,0xffffffffU);if(effect==ole_drag::copy && (x==23 || x==24 || y==23 || y==24))pixel(x,y,0xff000000U);
            if(effect==ole_drag::link && (y==22 || y==25) && x>=20 && x<=27)pixel(x,y,0xff000000U);
            if(effect==ole_drag::move && ((x>=20 && x<=27 && y==24) || (x>=25 && std::abs(dy)==static_cast<int>(27-x))))pixel(x,y,0xff000000U);}
    }
    const auto handle=gui_.next_object++;gui_.icons.emplace(handle,std::move(icon));gui_.ole_drag_cursors.emplace(effect,handle);return handle;
}
void WindowsServices::discard_ole_drag(GuestState& state,GuestMemory& memory,std::uint64_t id,bool target){
    const auto found=gui_.ole_drags.find(id);if(found==gui_.ole_drags.end())return;auto& op=found->second;
    if(target){op.target_active=false;op.target_error=disconnected;
        if(const auto request=gui_.sent_messages.find(op.target_rpc);request!=gui_.sent_messages.end()){request->second.error=1400;request->second.done=true;request->second.active=false;}
        if(!op.abandoned)return;
    }else {op.abandoned=true;if(op.target_active)return;}
    const auto release=[&](com_proxy::Origin origin){const auto retirement=release_com_proxy_owned(state,memory,origin);if(retirement)queue_com_proxy_retirement(memory,retirement);};
    if(op.target_ref)release({op.target_request.target.thread,op.target_request.target.object});
    if(op.data_proxy)release({op.proxy_thread,op.data_proxy});
    if(op.source_ref)release({op.thread,op.source});if(op.data_ref)release({op.thread,op.data});
    if(gui_.capture==op.capture && gui_.capture_generation==op.capture_generation){gui_.capture=0;++gui_.capture_generation;}
    if(op.default_cursor && gui_.cursor==op.default_cursor){
        gui_.cursor=op.original_cursor;
        if(native_gui_ && interactive_station()){
            const auto selected=gui_.icons.find(gui_.cursor);GuiIcon system;system.is_icon=false;
            if(selected!=gui_.icons.end())(void)native_gui_->set_cursor(&selected->second,gui_.cursor_count>=0);
            else {system.system=static_cast<std::uint32_t>(gui_.cursor-0xd000);(void)native_gui_->set_cursor(gui_.cursor?&system:nullptr,gui_.cursor_count>=0 && gui_.cursor!=0);}
        }
    }
    if(gui_.windows.contains(op.capture))erase_window(op.capture);
    // 跨執行緒 callback 尚未開始時可取消；開始後保留 buffer 到接收者完成。
    gui_.sent_messages.erase(op.target_rpc);const auto buffer=op.buffer;gui_.ole_drags.erase(found);(void)free_heap(memory,buffer,state);
}
std::vector<GuestCallback> WindowsServices::retire_ole_drag_thread(GuestState& state,GuestMemory& memory,std::uint64_t thread,bool callbacks){
    std::vector<GuestCallback> releases;
    for(auto& [id,op]:gui_.ole_drags){(void)id;
        if(op.thread==thread){op.abandoned=true;
            if(callbacks && op.source_ref)releases.push_back({drag_method(memory,op.source,2),{op.source}});
            if(callbacks && op.data_ref)releases.push_back({drag_method(memory,op.data,2),{op.data}});
            op.source_ref=op.data_ref=false;
        }
        if(op.proxy_thread==thread && op.data_proxy){
            if(runtime_.com_proxies.objects.get(op.data_proxy)){
                const auto retirement=release_com_proxy_owned(state,memory,{thread,op.data_proxy});if(retirement)queue_com_proxy_retirement(memory,retirement);
            }else if(callbacks)releases.push_back({drag_method(memory,op.data_proxy,2),{op.data_proxy}});
            op.data_proxy=0;
        }
        if(op.target_request.target.thread==thread){
            if(callbacks && op.target_ref)releases.push_back({drag_method(memory,op.target_request.target.object,2),{op.target_request.target.object}});
            op.target_ref=false;op.target_error=disconnected;
        }
    }return releases;
}
std::optional<std::uint32_t> WindowsServices::invoke_ole_drag(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using Action=ole_drag::Action;WindowsCall call(state,memory,service);
    if(service.symbol=="Read")return invoke_gui(state,memory,{"user32.dll","PeekMessageW",5});
    if(service.symbol=="Wait"){
        const auto found=gui_.ole_drags.find(call(0));if(found==gui_.ole_drags.end()){call.return_to(state,0);return std::nullopt;}const auto& op=found->second;
        auto& thread=threads_.items.at(threads_.current);const bool retried=thread.wait.kind==ThreadWaitKind::GuiInput;
        if(dispatch_gui_send(state,memory,0,false))return std::nullopt;
        if(retried || op.abandoned || gui_.capture!=op.capture || gui_.capture_generation!=op.capture_generation || ole_drag_input(op,true)!=op.input ||
           std::any_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& msg){return gui_message_matches(msg,op.thread);})){thread.wait={};call.return_to(state,0);return std::nullopt;}
        ThreadWait wait;wait.deadline=runtime_.clock_ticks+500000;wait_gui_input(state,memory,std::move(wait));return std::nullopt;
    }
    if(service.symbol=="TargetCall" || service.symbol=="TargetDispatch"){
        const auto id=call(0);const auto found=gui_.ole_drags.find(id);if(found==gui_.ole_drags.end()){call.return_to(state,disconnected);return std::nullopt;}auto& op=found->second;
        if(service.symbol=="TargetDispatch"){
            const auto request=op.target_request;const auto object=request.target.object;
            if(op.target_active || threads_.current!=request.target.thread){call.return_to(state,disconnected);return std::nullopt;}
            if(op.abandoned){discard_ole_drag(state,memory,id,true);call.return_to(state,disconnected);return std::nullopt;}
            const auto action=request.action;
            if(action==Action::AcquireTarget && !live_target(gui_,request.target)){op.target_error=disconnected;call.return_to(state,0);return std::nullopt;}
            unsigned slot=action==Action::AcquireTarget?1:action==Action::ReleaseTarget?2:action==Action::Enter?3:action==Action::Over?4:action==Action::Leave?5:6;
            GuestCallback callback{drag_method(memory,object,slot),{object}};
            if(action==Action::Enter || action==Action::Drop){callback.arguments={object,op.data_proxy,request.input.keys,ole_drag::point_value(request.input.point),op.buffer+effect_offset};callback.wide_arguments=8;}
            else if(action==Action::Over){callback.arguments={object,request.input.keys,ole_drag::point_value(request.input.point),op.buffer+effect_offset};callback.wide_arguments=4;}
            op.target_active=true;
            try{start_callbacks(state,memory,{std::move(callback)},Continuation::OleDragTarget,id);}catch(...){op.target_active=false;op.target_error=0x800703e3U;throw;}
            return std::nullopt;
        }
        auto request=gui_.sent_messages.find(op.target_rpc);
        if(request==gui_.sent_messages.end()){
            const auto receiver=threads_.items.find(op.target_request.target.thread);
            if(receiver==threads_.items.end() || receiver->second.status==ThreadStatus::Exited || receiver->second.forced_stop){op.target_error=disconnected;call.return_to(state,0);return std::nullopt;}
            GuiSentMessage queued;queued.sender=threads_.current;queued.receiver=receiver->first;queued.sender_stack=state.reg(Gpr::Rsp);queued.sender_rip=state.rip.value;
            queued.rpc=GuestCallback{resolve("internal-ole-drag","TargetDispatch").value,{id}};
            op.target_rpc=gui_.next_object++;request=gui_.sent_messages.emplace(op.target_rpc,std::move(queued)).first;
        }
        if(request->second.done){if(request->second.error)op.target_error=request->second.error==1400?disconnected:0x80070000U|(request->second.error&65535U);
            gui_.sent_messages.erase(request);op.target_rpc=0;threads_.items.at(threads_.current).wait={};call.return_to(state,op.target_result);return std::nullopt;}
        if(dispatch_gui_send(state,memory,0,false))return std::nullopt;
        auto& sender=threads_.items.at(threads_.current);sender.wait={};sender.wait.kind=ThreadWaitKind::GuiSend;sender.wait.handles={op.target_rpc};sender.status=ThreadStatus::Waiting;schedule(state,memory);return std::nullopt;
    }
    if(!call(0) || !call(1) || !call(3)){call.return_to(state,invalid);return std::nullopt;}
    memory.check(GuestAddress{call(3)},4,Access::Write);memory.store(GuestAddress{call(3)},32,0);
    if(!gui_.ole_initializations[threads_.current]){call.return_to(state,0x800401f0U);return std::nullopt;}
    if(gui_.ole_drags.size()>=4096 || runtime_.callbacks.size()>=120){call.return_to(state,no_memory);return std::nullopt;}
    for(const auto object:{call(0),call(1)})for(unsigned slot=1;slot<=2;++slot){const auto method=drag_method(memory,object,slot);if(!handles(GuestAddress{method}))memory.check(GuestAddress{method},1,Access::Execute);}
    OleDragOperation op;op.thread=threads_.current;op.data=call(0);op.source=call(1);op.output=call(3);op.original_cursor=gui_.cursor;op.escape_presses=gui_.escape_presses;
    op.buffer=allocate_heap(memory,80);if(!op.buffer){call.return_to(state,no_memory);return std::nullopt;}
    memory.write(GuestAddress{op.buffer},std::vector<std::uint8_t>(80));
    memory.write(GuestAddress{op.buffer+class_offset},std::array<std::uint8_t,14>{'S',0,'T',0,'A',0,'T',0,'I',0,'C',0,0,0});
    op.input.point=gui_.pointer_position;op.input=ole_drag_input(op,false);op.session=ole_drag::Session{static_cast<std::uint32_t>(call(2))};
    const auto id=gui_.next_object++;gui_.ole_drags.emplace(id,std::move(op));
    try{start_callbacks(state,memory,{{drag_method(memory,call(0),1),{call(0)}}},Continuation::OleDragLoop,id);}catch(...){discard_ole_drag(state,memory,id);throw;}return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::resume_ole_drag_target(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t result){
    const auto found=gui_.ole_drags.find(id);if(found==gui_.ole_drags.end()){complete_call(state,memory,0);return std::nullopt;}auto& op=found->second;
    op.target_active=false;op.target_result=static_cast<std::uint32_t>(result);
    if(op.target_request.action==ole_drag::Action::AcquireTarget)op.target_ref=true;
    if(op.target_request.action==ole_drag::Action::ReleaseTarget)op.target_ref=false;
    if(op.abandoned)discard_ole_drag(state,memory,id,true);
    complete_call(state,memory,result);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::resume_ole_drag(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t value){
    using Stage=OleDragOperation::Stage;using Action=ole_drag::Action;
    const auto found=gui_.ole_drags.find(id);if(found==gui_.ole_drags.end()){complete_call(state,memory,unexpected);return std::nullopt;}auto& op=found->second;
    const auto callback=[&](Stage stage,GuestCallback call){op.stage=stage;start_callbacks(state,memory,{std::move(call)},Continuation::OleDragLoop,id);};
    switch(op.stage){
    case Stage::DataRef:op.data_ref=true;callback(Stage::SourceRef,{drag_method(memory,op.source,1),{op.source}});return std::nullopt;
    case Stage::SourceRef:op.source_ref=true;callback(Stage::CreateCapture,{resolve("user32.dll","CreateWindowExW").value,{0,op.buffer+class_offset,0,0x80000000U,0,0,1,1,0,0,0,0}});return std::nullopt;
    case Stage::CreateCapture:
        op.capture=value;if(!value){op.session.cancel(no_memory);break;}
        op.capture_generation=gui_.capture_generation+1;callback(Stage::Capture,{resolve("user32.dll","SetCapture").value,{value}});return std::nullopt;
    case Stage::Capture:op.input.escape|=gui_.escape_presses!=op.escape_presses;(void)op.session.begin(op.input);break;
    case Stage::Marshal:
        op.data_proxy=value;op.proxy_thread=op.session.request().target.thread;
        if(!value){(void)op.session.complete({0,0,static_cast<std::uint32_t>(memory.load(GuestAddress{op.buffer+error_offset},32))});}break;
    case Stage::Action:{
        const auto action=op.session.request().action;
        if(op.target_request.target && !live_target(gui_,op.target_request.target))op.session.invalidate(op.target_request.target);
        const auto effect=static_cast<std::uint32_t>(memory.load(GuestAddress{op.buffer+effect_offset},32));
        const auto transport=action==Action::AcquireTarget || action==Action::Enter || action==Action::Over || action==Action::Leave || action==Action::Drop || action==Action::ReleaseTarget?op.target_error:0;
        if(action==Action::ReleaseTarget && op.data_proxy){
            const auto retirement=release_com_proxy_owned(state,memory,{op.proxy_thread,op.data_proxy});op.data_proxy=0;
            if(retirement){op.target_result=static_cast<std::uint32_t>(value);op.target_error=transport;callback(Stage::ProxyRelease,{resolve("internal-com-proxy","Retire").value,{retirement}});return std::nullopt;}
        }
        if(action==Action::DefaultFeedback){
            const auto expected=ole_drag_cursor(op.session.request().effect);
            if(gui_.cursor==expected)op.default_cursor=expected;else op.session.cancel(0x80070032U);
        }
        (void)op.session.complete({action==Action::DefaultFeedback?(gui_.cursor==op.default_cursor?0U:0x80004005U):static_cast<std::uint32_t>(value),effect,transport});break;
    }
    case Stage::ProxyRelease:(void)op.session.complete({op.target_result,0,op.target_error});break;
    case Stage::Read:
        if(value){
            const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto buffer=op.buffer;
            const auto message=memory.load(GuestAddress{buffer+(x86_?4:8)},32),key=memory.load(GuestAddress{buffer+(x86_?8:16)},bits);
            if(message==18){op.quit=true;op.quit_code=static_cast<std::uint32_t>(key);op.session.cancel();}
            else if(message>=0x200 && message<=0x20e){
                const auto offset=x86_?20U:36U;op.input.point={std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{buffer+offset},32))),std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{buffer+offset+4},32)))};
                op.input=ole_drag_input(op,false);op.input.keys=static_cast<std::uint32_t>((key&31U)|(op.input.keys&32U));(void)op.session.begin(op.input);
            }else if(message==0x100 || message==0x101 || message==0x104 || message==0x105){op.input=ole_drag_input(op,false);op.input.escape|=(message==0x100 || message==0x104) && key==27;(void)op.session.begin(op.input);}
            else if(message==0x1f || (message==0x215 && memory.load(GuestAddress{buffer},bits)==op.capture))op.session.cancel();
            else {op.stage=Stage::Dispatch;start_callbacks(state,memory,{{resolve("user32.dll","TranslateMessage").value,{buffer}},{resolve("user32.dll","DispatchMessageW").value,{buffer}}},Continuation::OleDragLoop,id);return std::nullopt;}
        }else {
            const auto input=ole_drag_input(op,true);
            if(input!=op.input){op.input=input;(void)op.session.begin(input);}
            else {callback(Stage::Wait,{resolve("internal-ole-drag","Wait").value,{id}});return std::nullopt;}
        }break;
    case Stage::Wait:case Stage::Dispatch:break;
    case Stage::Cleanup:break;
    }
    return advance_ole_drag(state,memory,id);
}
std::optional<std::uint32_t> WindowsServices::advance_ole_drag(GuestState& state,GuestMemory& memory,std::uint64_t id){
    using Stage=OleDragOperation::Stage;using Action=ole_drag::Action;
    const auto found=gui_.ole_drags.find(id);if(found==gui_.ole_drags.end()){complete_call(state,memory,unexpected);return std::nullopt;}auto& op=found->second;
    const auto callback=[&](Stage stage,GuestCallback call){op.stage=stage;start_callbacks(state,memory,{std::move(call)},Continuation::OleDragLoop,id);};
    if(op.capture && (gui_.capture!=op.capture || gui_.capture_generation!=op.capture_generation || !gui_.windows.contains(op.capture)))op.session.cancel();
    if(op.target_request.target && !live_target(gui_,op.target_request.target))op.session.invalidate(op.target_request.target);
    for(;;){
        const auto request=op.session.request();const auto action=request.action;
        if(action==Action::Finish){
            op.stage=Stage::Cleanup;
            while(op.cleanup<7){switch(op.cleanup++){
            case 0:if(op.capture && gui_.capture==op.capture && gui_.capture_generation==op.capture_generation){callback(Stage::Cleanup,{resolve("user32.dll","ReleaseCapture").value,{}});return std::nullopt;}break;
            case 1:if(gui_.windows.contains(op.capture)){callback(Stage::Cleanup,{resolve("user32.dll","DestroyWindow").value,{op.capture}});return std::nullopt;}break;
            case 2:if(op.default_cursor && gui_.cursor==op.default_cursor){callback(Stage::Cleanup,{resolve("user32.dll","SetCursor").value,{op.original_cursor}});return std::nullopt;}break;
            case 3:if(op.source_ref){const auto retirement=release_com_proxy_owned(state,memory,{op.thread,op.source});op.source_ref=false;if(retirement){callback(Stage::Cleanup,{resolve("internal-com-proxy","Retire").value,{retirement}});return std::nullopt;}}break;
            case 4:if(op.data_ref){const auto retirement=release_com_proxy_owned(state,memory,{op.thread,op.data});op.data_ref=false;if(retirement){callback(Stage::Cleanup,{resolve("internal-com-proxy","Retire").value,{retirement}});return std::nullopt;}}break;
            case 5:if(op.quit){callback(Stage::Cleanup,{resolve("user32.dll","PostQuitMessage").value,{op.quit_code}});return std::nullopt;}break;
            case 6:break;
            }}
            const auto result=op.session.result();const auto buffer=op.buffer,output=op.output;gui_.ole_drags.erase(found);(void)free_heap(memory,buffer,state);
            memory.store(GuestAddress{output},32,result.effect);complete_call(state,memory,result.status);return std::nullopt;
        }
        if(action==Action::Wait){callback(Stage::Read,{resolve("internal-ole-drag","Read").value,{op.buffer,0,0,0,1}});return std::nullopt;}
        if(action==Action::QueryContinue){op.escape_presses=gui_.escape_presses;callback(Stage::Action,{drag_method(memory,op.source,3),{op.source,request.input.escape?1U:0U,request.input.keys}});return std::nullopt;}
        if(action==Action::Feedback){callback(Stage::Action,{drag_method(memory,op.source,4),{op.source,request.effect}});return std::nullopt;}
        if(action==Action::DefaultFeedback){const auto cursor=ole_drag_cursor(request.effect);callback(Stage::Action,{resolve("user32.dll","SetCursor").value,{cursor}});return std::nullopt;}
        if(action==Action::Enter && !op.data_proxy){
            callback(Stage::Marshal,{resolve("internal-com-proxy","Marshal").value,{op.data,op.thread,request.target.thread,static_cast<std::uint64_t>(com_proxy::Kind::DataObject),op.buffer+error_offset}});return std::nullopt;
        }
        op.target_request=request;op.target_error=0;memory.store(GuestAddress{op.buffer+effect_offset},32,request.effect);
        callback(Stage::Action,{resolve("internal-ole-drag","TargetCall").value,{id}});return std::nullopt;
    }
}
}
