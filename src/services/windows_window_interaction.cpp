#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
namespace {
std::uint64_t xy(std::int64_t x,std::int64_t y){return (static_cast<std::uint64_t>(x)&65535U)|((static_cast<std::uint64_t>(y)&65535U)<<16);}
std::int32_t integer(std::uint64_t value){return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));}
bool button_down(unsigned message){return message==0x201 || message==0x203 || message==0x204 || message==0x206 || message==0x207 || message==0x209 || message==0x20b || message==0x20d;}
}
std::span<const WindowsServiceDescriptor> gui_move_size_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{{"internal-window-interaction","ReadMessage",4}};return table;
}
std::uint64_t WindowsServices::gui_root_window(std::uint64_t handle)const {
    for(unsigned depth=0;depth<64;++depth){const auto window=gui_.windows.find(handle);if(window==gui_.windows.end())return 0;
        if(!window->second.parent)return handle;handle=window->second.parent;}
    return 0;
}
void WindowsServices::raise_gui_window(std::uint64_t handle){
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())return;
    std::vector<std::uint64_t> group;
    // 同一 owner 群組仍保持 popup 在 owner 上方，不能把 owner 蓋到 popup 上。
    const auto owned=[&](std::uint64_t item){for(unsigned n=0;n<64 && item;++n){if(item==handle)return true;const auto entry=gui_.windows.find(item);if(entry==gui_.windows.end() || entry->second.parent)break;item=entry->second.owner;}return false;};
    for(const auto item:gui_.z_order)if(owned(item))group.push_back(item);
    for(const auto item:group)std::erase(gui_.z_order,item);
    auto position=gui_.z_order.begin();
    if(!(found->second.ex_style&8U))while(position!=gui_.z_order.end()){
        const auto item=gui_.windows.find(*position);if(item!=gui_.windows.end() && !item->second.parent && !(item->second.ex_style&8U))break;++position;}
    gui_.z_order.insert(position,group.begin(),group.end());
    if(native_gui_)for(auto it=group.rbegin();it!=group.rend();++it)native_gui_->reorder(*it,0);
}
std::vector<GuestCallback> WindowsServices::activate_gui_window(std::uint64_t handle,bool mouse,bool native){
    std::vector<GuestCallback> callbacks;const auto previous=gui_.active;
    gui_.active=handle;poll_desktop_notifications();
    for(auto owner=handle,depth=UINT64_C(0);owner && depth<64;++depth){const auto entry=gui_.windows.find(owner);if(entry==gui_.windows.end() || entry->second.parent)break;entry->second.last_active_popup=handle;owner=entry->second.owner;}
    const auto notify=[&](std::uint64_t target,unsigned message,std::uint64_t wp,std::uint64_t lp){
        const auto found=gui_.windows.find(target);if(found!=gui_.windows.end())callbacks.push_back({resolve("user32.dll",found->second.unicode?"SendMessageW":"SendMessageA").value,{target,message,wp,lp}});
    };
    if(previous!=handle){
        const auto old=gui_.windows.find(previous),current=gui_.windows.find(handle);
        notify(previous,0x86,0,handle);
        notify(previous,6,old!=gui_.windows.end() && old->second.show_command==2?0x10000U:0U,handle);
        notify(handle,0x86,1,previous);
        notify(handle,6,(mouse?2U:1U)|(current!=gui_.windows.end() && current->second.show_command==2?0x10000U:0U),previous);
    }
    if(handle){raise_gui_window(handle);if(native && native_gui_){native_gui_->activate(handle);if(gui_root_window(gui_.focus)==handle)native_gui_->focus(gui_.focus);}}
    return callbacks;
}
void WindowsServices::queue_gui_mouse(GuiMessage message,bool native_activation){
    const auto target=gui_.windows.find(message.window);if(target==gui_.windows.end())return;
    if(gui_.mouse_inputs.size()>=10000)throw HostCallFault("待處理滑鼠輸入超過上限");
    if(message.queue_priority==1 && !message.input_order)message.input_order=gui_.next_input_order++;
    if(!message.injected)message.queued_time=static_cast<std::uint32_t>(runtime_.clock_ticks/10000);
    if(!native_activation){
        if(!message.keyboard_snapshot){
            auto snapshot=gui_.input_keyboard;
            for(const auto [key,flag]:std::array<std::pair<unsigned,unsigned>,5>{{{1,1},{2,2},{4,16},{5,32},{6,64}}})snapshot[key]=static_cast<std::uint8_t>((snapshot[key]&1U)|((message.wparam&flag)?0x80U:0U));
            for(const auto [key,flag]:std::array<std::pair<unsigned,unsigned>,3>{{{16,1},{17,2},{18,4}}})snapshot[key]=static_cast<std::uint8_t>((snapshot[key]&1U)|((message.modifiers&flag)?0x80U:0U));
            gui_.input_keyboard=snapshot;message.keyboard_snapshot=snapshot;
        }
        if(!message.screen_point){const bool screen=message.message==0x20a || message.message==0x20e || (message.message>=0xa0 && message.message<=0xad);
            auto point=message.has_point?message.point:std::array<std::int32_t,2>{static_cast<std::int16_t>(message.lparam),static_cast<std::int16_t>(message.lparam>>16)};
            const auto origin=screen?std::array<std::int64_t,2>{}:gui_client_origin(gui_,message.window);
            for(unsigned i=0;i<2;++i)message.point[i]=static_cast<std::int32_t>(std::clamp<std::int64_t>(origin[i]+point[i],INT32_MIN,INT32_MAX));
        }
        message.has_point=true;message.screen_point=true;message.queued_cursor=message.point;gui_.pointer_position=message.point;
    }
    GuiMouseInput operation;operation.message=std::move(message);operation.thread=target->second.thread;operation.native_activation=native_activation;
    gui_.mouse_inputs.emplace(gui_.next_object++,std::move(operation));
}
bool WindowsServices::dispatch_gui_mouse(GuestState& state,GuestMemory& memory,std::uint64_t modal,bool wait_message){
    // 先讓程式處理較早的按下事件；其中的 SetCapture／SC_MOVE 必須影響後續移動。
    if(std::any_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& message){return message.queue_priority==1 && ((message.message>=0x200 && message.message<=0x20e) || (message.message>=0xa0 && message.message<=0xad)) && gui_message_matches(message,threads_.current);}))return false;
    for(auto& [id,input]:gui_.mouse_inputs)if(input.thread==threads_.current && !input.active){
        input.active=true;input.modal=modal;input.wait_message=wait_message;
        if(input.native_activation){input.stage=GuiMouseInput::Stage::Commit;input.root=gui_root_window(input.message.window);
            auto calls=activate_gui_window(input.root,false,false);start_callbacks(state,memory,std::move(calls),Continuation::GuiMouseInput,id);return true;}
        if(gui_.capture && gui_.windows.contains(gui_.capture))input.message.window=gui_.capture;
        const auto target=gui_.windows.find(input.message.window);
        if(target==gui_.windows.end()){input.stage=GuiMouseInput::Stage::Commit;input.eat=true;start_callbacks(state,memory,{},Continuation::GuiMouseInput,id);return true;}
        input.tested.push_back(input.message.window);input.root=gui_root_window(input.message.window);
        const auto send=resolve("user32.dll",target->second.unicode?"SendMessageW":"SendMessageA").value;
        start_callbacks(state,memory,{{send,{input.message.window,0x84,0,xy(input.message.point[0],input.message.point[1])}}},Continuation::GuiMouseInput,id);return true;
    }
    return false;
}
std::optional<std::uint32_t> WindowsServices::resume_gui_mouse(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t value){
    const auto found=gui_.mouse_inputs.find(id);if(found==gui_.mouse_inputs.end())return std::nullopt;
    auto& input=found->second;auto& message=input.message;
    const auto callback=[&](GuiMouseInput::Stage stage,std::vector<GuestCallback> calls){input.stage=stage;start_callbacks(state,memory,std::move(calls),Continuation::GuiMouseInput,id);};
    const auto finish=[&](){const auto modal=input.modal;const bool wait=input.wait_message;gui_.mouse_inputs.erase(id);if(modal)return advance_gui_modal(state,memory,modal);if(wait)complete_call(state,memory,1);return std::optional<std::uint32_t>{};};
    if(!gui_.windows.contains(message.window))return finish();
    if(input.stage==GuiMouseInput::Stage::Hit){
        input.hit=gui_.capture==message.window?1:integer(value);
        if(input.hit==-1){ // HTTRANSPARENT 只向同一執行緒的底層兄弟／父視窗轉送。
            const auto& current=gui_.windows.at(message.window);bool below=false;
            for(const auto candidate:gui_.z_order){if(candidate==message.window){below=true;continue;}if(!below)continue;
                const auto target=gui_.windows.find(candidate);if(target==gui_.windows.end() || target->second.parent!=current.parent || target->second.thread!=input.thread || !target->second.visible || !target->second.enabled || std::find(input.tested.begin(),input.tested.end(),candidate)!=input.tested.end() || !gui_window_hit(gui_,candidate,message.point[0],message.point[1]))continue;
                message.window=candidate;input.root=gui_root_window(candidate);input.tested.push_back(candidate);
                callback(GuiMouseInput::Stage::Hit,{{resolve("user32.dll",target->second.unicode?"SendMessageW":"SendMessageA").value,{candidate,0x84,0,xy(message.point[0],message.point[1])}}});return std::nullopt;
            }
            if(current.parent && gui_.windows.contains(current.parent) && std::find(input.tested.begin(),input.tested.end(),current.parent)==input.tested.end()){
                message.window=current.parent;input.tested.push_back(message.window);callback(GuiMouseInput::Stage::Hit,{{resolve("user32.dll","SendMessageW").value,{message.window,0x84,0,xy(message.point[0],message.point[1])}}});return std::nullopt;}
        }
        if(input.hit<=0)return finish();
        if(button_down(message.message) && (gui_.active!=input.root || (native_gui_ && !native_gui_->input_active())) && !gui_.capture){
            callback(GuiMouseInput::Stage::Activate,{{resolve("user32.dll",gui_.windows.at(message.window).unicode?"SendMessageW":"SendMessageA").value,{message.window,0x21,input.root,static_cast<std::uint32_t>(input.hit)|(std::uint64_t{message.message}<<16)}}});return std::nullopt;
        }
    }else if(input.stage==GuiMouseInput::Stage::Activate){
        const auto response=static_cast<std::uint32_t>(value);input.eat=response==2 || response==4;
        if((response==1 || response==2) && gui_.windows.contains(input.root) && !(gui_.windows.at(input.root).ex_style&0x08000000U)){
            callback(GuiMouseInput::Stage::Commit,activate_gui_window(input.root,true));return std::nullopt;
        }
    }
    if(!input.eat && !input.native_activation){
        const bool wheel=message.message==0x20a || message.message==0x20e;
        if(input.hit!=1 && !wheel){message.message-=0x160U;message.wparam=static_cast<std::uint32_t>(input.hit)|(message.wparam&0xffff0000U);message.lparam=xy(message.point[0],message.point[1]);}
        else {const auto origin=wheel?std::array<std::int64_t,2>{}:gui_client_origin(gui_,message.window);message.lparam=xy(message.point[0]-origin[0],message.point[1]-origin[1]);}
        enqueue_gui_message(message);
    }
    return finish();
}
bool WindowsServices::gui_move_size_interrupted(std::uint64_t id)const {
    const auto found=gui_.move_sizes.find(id);if(found==gui_.move_sizes.end())return true;
    return !gui_.windows.contains(found->second.window) || gui_.capture!=found->second.window || gui_.capture_generation!=found->second.capture_generation;
}
void WindowsServices::discard_gui_move_size(GuestMemory& memory,const GuestState& state,std::uint64_t id){
    const auto found=gui_.move_sizes.find(id);if(found==gui_.move_sizes.end())return;const auto operation=found->second;gui_.move_sizes.erase(found);
    if(gui_.capture==operation.window && gui_.capture_generation==operation.capture_generation){gui_.capture=0;++gui_.capture_generation;}
    if(operation.buffer)free_heap(memory,operation.buffer,state);
}
std::optional<std::uint32_t> WindowsServices::invoke_gui_move_size(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall call(state,memory,service);
    if(runtime_.callbacks.empty() || runtime_.callbacks.back().continuation!=Continuation::GuiMoveSize || gui_move_size_interrupted(runtime_.callbacks.back().value)){call.return_to(state,UINT32_MAX);return std::nullopt;}
    return invoke_gui(state,memory,{"user32.dll","GetMessageW",4});
}
std::optional<std::uint32_t> WindowsServices::begin_gui_move_size(GuestState& state,GuestMemory& memory,std::uint64_t handle,std::uint32_t command,std::uint64_t point){
    const auto window=gui_.windows.find(handle);const auto edge=(command&0xfff0U)==0xf000U?((command&15U)?command&15U:8U):0U;
    if(window==gui_.windows.end() || !window->second.enabled || window->second.show_command!=1 || (edge && !(window->second.style&0x40000U)) || edge>8){complete_call(state,memory,0);return std::nullopt;}
    if(std::any_of(gui_.move_sizes.begin(),gui_.move_sizes.end(),[&](const auto& item){return item.second.thread==threads_.current;})){complete_call(state,memory,0);return std::nullopt;}
    const auto buffer=allocate_heap(memory,104);if(!buffer){last_error_=8;complete_call(state,memory,0);return std::nullopt;}
    GuiMoveSize operation;operation.window=handle;operation.thread=threads_.current;operation.buffer=buffer;operation.edge=edge;operation.keyboard=!(command&15U);
    const auto outer=gui_outer_size(window->second);const auto origin=gui_client_origin(gui_,handle);const auto inset=gui_frame_insets(window->second);
    operation.original={static_cast<std::int32_t>(origin[0]-inset.left),static_cast<std::int32_t>(origin[1]-inset.top),static_cast<std::int32_t>(origin[0]-inset.left+outer[0]),static_cast<std::int32_t>(origin[1]-inset.top+outer[1])};
    operation.start=operation.point=point?std::array<std::int32_t,2>{static_cast<std::int16_t>(point),static_cast<std::int16_t>(point>>16)}:gui_.pointer_position;
    operation.capture_generation=gui_.capture_generation+(gui_.capture==handle?0U:1U);
    // MINMAXINFO 與 RECT 均在可重入操作自己的客體記憶體內。
    for(unsigned i=0;i<10;++i)memory.store(GuestAddress{buffer+64+i*4},32,i==6 || i==7?1U:i==8 || i==9?8192U:0U);
    const auto id=gui_.next_object++;gui_.move_sizes.emplace(id,operation);
    const auto send=resolve("user32.dll",window->second.unicode?"SendMessageW":"SendMessageA").value;
    start_callbacks(state,memory,{{send,{handle,0x24,0,buffer+64}},{resolve("user32.dll","SetCapture").value,{handle}},{send,{handle,0x231,0,0}}},Continuation::GuiMoveSize,id);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::resume_gui_move_size(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t value){
    const auto found=gui_.move_sizes.find(id);if(found==gui_.move_sizes.end()){complete_call(state,memory,0);return std::nullopt;}
    auto& operation=found->second;using Stage=GuiMoveSize::Stage;const auto buffer=operation.buffer,handle=operation.window;
    const auto send=resolve("user32.dll",gui_.windows.contains(handle) && !gui_.windows.at(handle).unicode?"SendMessageA":"SendMessageW").value;
    const auto callback=[&](Stage stage,std::vector<GuestCallback> calls){operation.stage=stage;start_callbacks(state,memory,std::move(calls),Continuation::GuiMoveSize,id);};
    const auto finish=[&](){const auto quit=operation.quit;const auto code=operation.quit_code;discard_gui_move_size(memory,state,id);if(quit)start_callbacks(state,memory,{{resolve("user32.dll","PostQuitMessage").value,{code}}},Continuation::Return,0);else complete_call(state,memory,0);};
    const auto position=[&](Stage stage,const std::array<std::int32_t,4>& rect){
        const auto parent=gui_.windows.at(handle).parent;const auto origin=parent?gui_client_origin(gui_,parent):std::array<std::int64_t,2>{};
        callback(stage,{{resolve("user32.dll","SetWindowPos").value,{handle,0,static_cast<std::uint32_t>(rect[0]-origin[0]),static_cast<std::uint32_t>(rect[1]-origin[1]),static_cast<std::uint32_t>(rect[2]-rect[0]),static_cast<std::uint32_t>(rect[3]-rect[1]),0x14U|(!operation.edge?1U:0U)}}});
    };
    if(operation.stage==Stage::Exit){finish();return std::nullopt;}
    if(operation.stage==Stage::Release){if(gui_.windows.contains(handle))callback(Stage::Exit,{{send,{handle,0x232,0,0}}});else finish();return std::nullopt;}
    bool end=operation.stage==Stage::Restore || gui_move_size_interrupted(id);
    bool moved=false;
    if(operation.stage==Stage::Read && !end){
        const auto message=static_cast<unsigned>(memory.load(GuestAddress{buffer+(x86_?4:8)},32));const auto wp=memory.load(GuestAddress{buffer+(x86_?8:16)},x86_?32:64);
        if(static_cast<std::uint32_t>(value)==UINT32_MAX)end=true;
        else if(!value){end=true;operation.quit=true;operation.quit_code=static_cast<std::uint32_t>(wp);}
        else if(message==0x202 || message==0xa2 || message==0x1f || message==0x215)end=true;
        else if(message==0x100 || message==0x104){
            if(wp==27){end=true;operation.cancel=true;}
            else if(wp==13)end=true;
            else if(wp>=37 && wp<=40){const auto step=(gui_.keyboard[17]&0x80U)?1:10;operation.point[wp==37 || wp==39?0:1]+=wp==37 || wp==38?-step:step;moved=true;}
        }else if(message==0x200 || message==0xa0){
            for(unsigned i=0;i<2;++i)operation.point[i]=integer(memory.load(GuestAddress{buffer+(x86_?20:36)+i*4},32));moved=true;
        }else {callback(Stage::Dispatch,{{resolve("user32.dll","TranslateMessage").value,{buffer}},{resolve("user32.dll","DispatchMessageW").value,{buffer}}});return std::nullopt;}
    }
    if(end){
        if(operation.cancel && operation.stage!=Stage::Restore && gui_.windows.contains(handle)){position(Stage::Restore,operation.original);return std::nullopt;}
        if(gui_.capture==handle && gui_.capture_generation==operation.capture_generation)callback(Stage::Release,{{resolve("user32.dll","ReleaseCapture").value,{}}});
        else if(gui_.windows.contains(handle))callback(Stage::Exit,{{send,{handle,0x232,0,0}}});else finish();return std::nullopt;
    }
    if(operation.stage==Stage::Rect){std::array<std::int32_t,4> rect{};for(unsigned i=0;i<4;++i)rect[i]=integer(memory.load(GuestAddress{buffer+48+i*4},32));position(Stage::Position,rect);return std::nullopt;}
    if(moved){
        auto rect=operation.original;const auto dx=std::int64_t{operation.point[0]}-operation.start[0],dy=std::int64_t{operation.point[1]}-operation.start[1];const auto edge=operation.edge;
        const auto change=[&](unsigned index,std::int64_t delta){rect[index]=static_cast<std::int32_t>(std::clamp<std::int64_t>(std::int64_t{rect[index]}+delta,INT32_MIN,INT32_MAX));};
        if(!edge){change(0,dx);change(2,dx);change(1,dy);change(3,dy);}
        else {
            const bool left=edge==1 || edge==4 || edge==7,right=edge==2 || edge==5 || edge==8,top=edge>=3 && edge<=5,bottom=edge>=6;
            if(left)change(0,dx);if(right)change(2,dx);if(top)change(1,dy);if(bottom)change(3,dy);
            for(unsigned axis=0;axis<2;++axis){const auto minimum=std::clamp(integer(memory.load(GuestAddress{buffer+88+axis*4},32)),1,8192),maximum=std::clamp(integer(memory.load(GuestAddress{buffer+96+axis*4},32)),minimum,8192);
                const auto size=std::clamp<std::int64_t>(std::int64_t{rect[axis+2]}-rect[axis],minimum,maximum);if(axis?top:left)rect[axis]=integer(static_cast<std::uint32_t>(rect[axis+2]-size));else rect[axis+2]=integer(static_cast<std::uint32_t>(rect[axis]+size));}
        }
        for(unsigned i=0;i<4;++i)memory.store(GuestAddress{buffer+48+i*4},32,static_cast<std::uint32_t>(rect[i]));
        callback(Stage::Rect,{{send,{handle,edge?0x214U:0x216U,edge,buffer+48}}});return std::nullopt;
    }
    callback(Stage::Read,{{resolve("internal-window-interaction","ReadMessage").value,{buffer,0,0,0}}});return std::nullopt;
}
}
