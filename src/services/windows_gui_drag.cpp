#include "x64arm/windows_services.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
std::span<const WindowsServiceDescriptor> gui_drag_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        // POINT 是值參數：Win32 佔兩個 DWORD，Win64 佔一個 64 位元暫存器。
        {"user32.dll","DragDetect",2,2},
        {"internal-gui-drag","ReadMessage",4}};
    return table;
}
bool WindowsServices::gui_drag_interrupted(std::uint64_t id) const {
    const auto found=gui_.drag_detections.find(id);if(found==gui_.drag_detections.end())return true;
    const auto& operation=found->second;
    if(!gui_.windows.contains(operation.window) || gui_.capture!=operation.window || gui_.capture_generation!=operation.capture_generation)return true;
    // 先處理已排入的輸入快照；不能以較晚的實體 button-up 取消較早的有效移動。
    const bool queued_mouse=std::any_of(gui_.mouse_inputs.begin(),gui_.mouse_inputs.end(),[&](const auto& item){return item.second.thread==operation.thread && !item.second.native_activation;}) || std::any_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& message){
        return message.message>=0x200 && message.message<=0x20e && gui_message_matches(message,operation.thread);
    });
    return !queued_mouse && !(gui_.async_keyboard[1]&0x80U);
}
void WindowsServices::discard_gui_drag(GuestMemory& memory,const GuestState& state,std::uint64_t id){
    const auto found=gui_.drag_detections.find(id);if(found==gui_.drag_detections.end())return;
    const auto operation=found->second;gui_.drag_detections.erase(found);
    if(gui_.capture==operation.window && gui_.capture_generation==operation.capture_generation){gui_.capture=0;++gui_.capture_generation;}
    if(operation.message_buffer)free_heap(memory,operation.message_buffer,state);
}
std::optional<std::uint32_t> WindowsServices::invoke_gui_drag(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    WindowsCall arg(state,memory,service);const auto fail=[&](std::uint32_t error){last_error_=error;arg.return_to(state,0);};
    if(service.family=="internal-gui-drag"){
        if(runtime_.callbacks.empty() || runtime_.callbacks.back().continuation!=Continuation::GuiDragDetection){last_error_=87;arg.return_to(state,UINT32_MAX);return std::nullopt;}
        const auto id=runtime_.callbacks.back().value;const auto found=gui_.drag_detections.find(id);
        if(found==gui_.drag_detections.end() || found->second.thread!=threads_.current || found->second.stage!=GuiDragDetection::Stage::Read || gui_drag_interrupted(id)){
            arg.return_to(state,UINT32_MAX);return std::nullopt;
        }
        // 保留共用 GetMessage 的 sent-message、輸入快照、hook、排程與客體 callback 契約。
        return invoke_gui(state,memory,{"user32.dll","GetMessageW",4});
    }
    const auto window=gui_.windows.find(arg(0));
    if(window==gui_.windows.end()){fail(1400);return std::nullopt;}
    if(window->second.thread!=threads_.current){fail(5);return std::nullopt;}
    if(!(gui_.keyboard[1]&0x80U)){arg.return_to(state,0);return std::nullopt;}
    if(gui_.drag_detections.size()>=4096){fail(1816);return std::nullopt;}
    const auto buffer=allocate_heap(memory,x86_?28:48);if(!buffer){fail(8);return std::nullopt;}
    GuiDragDetection operation;operation.window=arg(0);operation.thread=threads_.current;operation.message_buffer=buffer;
    const auto point=arg(1);
    operation.detection=mouse_drag::Detection{{std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(point)),std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(point>>32))},gui_.mouse_drag_width,gui_.mouse_drag_height};
    operation.capture_generation=gui_.capture_generation+(gui_.capture==operation.window?0U:1U);
    const auto id=gui_.next_object++;gui_.drag_detections.emplace(id,operation);
    start_callbacks(state,memory,{{resolve("user32.dll","SetCapture").value,{operation.window}}},Continuation::GuiDragDetection,id);
    return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::resume_gui_drag(GuestState& state,GuestMemory& memory,std::uint64_t id,std::uint64_t value){
    const auto found=gui_.drag_detections.find(id);
    if(found==gui_.drag_detections.end()){complete_call(state,memory,0);return std::nullopt;}
    auto& operation=found->second;using Stage=GuiDragDetection::Stage;using Result=mouse_drag::Detection::Result;
    const auto callback=[&](Stage stage,std::vector<GuestCallback> calls){operation.stage=stage;start_callbacks(state,memory,std::move(calls),Continuation::GuiDragDetection,id);};
    const auto finish=[&](){const auto result=operation.detection.result==Result::Dragged?1U:0U;const auto buffer=operation.message_buffer;gui_.drag_detections.erase(id);free_heap(memory,buffer,state);complete_call(state,memory,result);};
    if(operation.stage==Stage::RepostQuit){finish();return std::nullopt;}
    if(operation.stage==Stage::Release){
        if(operation.quit)callback(Stage::RepostQuit,{{resolve("user32.dll","PostQuitMessage").value,{operation.quit_code}}});
        else finish();
        return std::nullopt;
    }
    if(operation.stage==Stage::Read){
        if(static_cast<std::uint32_t>(value)==UINT32_MAX)operation.detection.cancel();
        else {
            const auto buffer=operation.message_buffer;const std::uint8_t bits=x86_?32:64;
            const auto message=memory.load(GuestAddress{buffer+(x86_?4:8)},32),key=memory.load(GuestAddress{buffer+(x86_?8:16)},bits);
            if(!value){operation.quit=true;operation.quit_code=static_cast<std::uint32_t>(key);operation.detection.cancel();}
            else if(message==0x200){
                const auto offset=x86_?20U:36U;
                operation.detection.move({std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{buffer+offset},32))),std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{buffer+offset+4},32)))},key&1U);
            }else if(message==0x202 || ((message==0x100 || message==0x104) && key==27) || ((message==0x1f || message==0x215) && memory.load(GuestAddress{buffer},bits)==operation.window))operation.detection.cancel();
            else {
                callback(Stage::Dispatch,{{resolve("user32.dll","TranslateMessage").value,{buffer}},{resolve("user32.dll","DispatchMessageW").value,{buffer}}});
                return std::nullopt;
            }
        }
    }
    if(operation.detection.result==Result::Tracking && gui_drag_interrupted(id))operation.detection.cancel();
    if(operation.detection.result==Result::Tracking){
        callback(Stage::Read,{{resolve("internal-gui-drag","ReadMessage").value,{operation.message_buffer,0,0,0}}});
    }else if(gui_.capture==operation.window && gui_.capture_generation==operation.capture_generation){
        callback(Stage::Release,{{resolve("user32.dll","ReleaseCapture").value,{}}});
    }else if(operation.quit){
        callback(Stage::RepostQuit,{{resolve("user32.dll","PostQuitMessage").value,{operation.quit_code}}});
    }else finish();
    return std::nullopt;
}
}
