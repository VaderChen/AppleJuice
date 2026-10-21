#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
void WindowsServices::cancel_gui_cbt_action(std::uint64_t id,GuestMemory& memory,const GuestState& state){
    const auto found=gui_.cbt_actions.find(id);if(found==gui_.cbt_actions.end())return;
    release_gui_scratch(memory,state,found->second.scratch);gui_.cbt_actions.erase(found);
}
bool WindowsServices::intercept_gui_cbt(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto bypass=gui_.cbt_replay.find(threads_.current);
    if(bypass!=gui_.cbt_replay.end() && bypass->second==std::pair{state.rip.value,std::string(service.symbol)}){gui_.cbt_replay.erase(bypass);return false;}
    const auto symbol=service.symbol;
    const bool moving=symbol=="MoveWindow" || symbol=="SetWindowPos";
    if(symbol!="SetFocus" && symbol!="SetActiveWindow" && symbol!="SetForegroundWindow" && symbol!="DestroyWindow" && symbol!="ShowWindow" && !moving && !symbol.starts_with("DefWindowProc"))return false;
    if(std::none_of(gui_.hooks.begin(),gui_.hooks.end(),[&](const auto& item){return item.second.type==5 && (!item.second.thread || item.second.thread==threads_.current);}))return false;
    const WindowsCall arg(state,memory,service);const auto found=gui_.windows.find(arg(0));
    if(found==gui_.windows.end() && !(symbol=="SetFocus" && !arg(0)))return false;
    if(found!=gui_.windows.end() && (found->second.thread!=threads_.current || found->second.destroying))return false;
    GuiCbtAction action;action.symbol=std::string(symbol);action.arguments=service.arguments;action.thread=threads_.current;
    std::int32_t code=-1;std::uint64_t wp=arg(0),lp=0;
    if(symbol=="SetFocus"){if(gui_.focus==arg(0))return false;code=9;lp=gui_.focus;}
    else if(symbol=="SetActiveWindow" || symbol=="SetForegroundWindow"){if(gui_.active==arg(0))return false;code=5;}
    else if(symbol=="DestroyWindow" || (symbol.starts_with("DefWindowProc") && arg(1)==0x10))code=4;
    else if(symbol=="ShowWindow"){if(arg(1)!=9)return false;code=1;lp=arg(1);action.veto_result=found->second.visible?1:0;}
    else if(symbol.starts_with("DefWindowProc") && arg(1)==0x112){code=8;wp=arg(2);lp=arg(3);}
    else if(moving){
        const auto flags=symbol=="MoveWindow"?4U:static_cast<std::uint32_t>(arg(6));if((flags&3)==3)return false;
        const auto& window=found->second;const auto outer=gui_outer_size(window);const bool move=symbol=="MoveWindow";
        const auto x=(flags&2)?window.x:std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(move?1:2)));
        const auto y=(flags&2)?window.y:std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(arg(move?2:3)));
        const auto width=(flags&1)?outer[0]:static_cast<std::uint32_t>(arg(move?3:4)),height=(flags&1)?outer[1]:static_cast<std::uint32_t>(arg(move?4:5));
        if(width>8192 || height>8192)return false;
        action.rectangle={static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y),static_cast<std::uint32_t>(x)+width,static_cast<std::uint32_t>(y)+height};code=0;
    }
    if(code<0)return false;
    const auto id=gui_.next_object++,failure=action.veto_result;
    try{
        auto [operation,inserted]=gui_.cbt_actions.emplace(id,std::move(action));(void)inserted;
        auto& stored=operation->second;
        if(code==5){lp=allocate_gui_scratch(memory,stored.scratch,{},x86_?8U:16U);memory.store(GuestAddress{lp+(x86_?4U:8U)},x86_?32:64,gui_.active);}
        else if(code==0){lp=allocate_gui_scratch(memory,stored.scratch,{},16);stored.data=lp;for(unsigned i=0;i<4;++i)memory.store(GuestAddress{lp+i*4},32,stored.rectangle[i]);}
        if(start_gui_hooks(state,memory,5,code,wp,lp,Continuation::GuiCbtAction,id))return true;
        cancel_gui_cbt_action(id,memory,state);return false;
    }catch(const std::bad_alloc&){cancel_gui_cbt_action(id,memory,state);last_error_=8;arg.return_to(state,failure);return true;}
    catch(...){cancel_gui_cbt_action(id,memory,state);throw;}
}
std::optional<std::uint32_t> WindowsServices::resume_gui_cbt_action(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t result)try{
    const auto found=gui_.cbt_actions.find(frame.value);if(found==gui_.cbt_actions.end()){complete_call(state,memory,0);return std::nullopt;}
    auto& action=found->second;
    if(result){const auto value=action.veto_result;cancel_gui_cbt_action(frame.value,memory,state);complete_call(state,memory,value);return std::nullopt;}
    if(action.symbol=="SetFocus" && !action.focus_activation){
        action.focus_activation=true;
        const WindowsCall call(state,memory,1);const auto window=gui_.windows.find(call(0));
        if(window!=gui_.windows.end() && window->second.enabled){
            const auto root=gui_root_window(call(0));
            if(root && root!=gui_.active){
                // HCBT_SETFOCUS 的巢狀操作結束後重新讀取 active，再讓 HCBT_ACTIVATE
                // 決定是否允許啟用。不能直接跳過 activation hook 修改 focus。
                const auto data=allocate_gui_scratch(memory,action.scratch,{},x86_?8U:16U);
                memory.store(GuestAddress{data+(x86_?4U:8U)},x86_?32:64,gui_.active);
                if(start_gui_hooks(state,memory,5,5,root,data,Continuation::GuiCbtAction,frame.value))return {};
            }
        }
    }
    if(action.data){
        std::array<std::uint32_t,4> rect{};for(unsigned i=0;i<4;++i)rect[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{action.data+i*4},32));
        const auto set=[&](unsigned index,std::uint64_t value){if(x86_){memory.store(GuestAddress{state.reg(Gpr::Rsp)+(index+1)*4},32,value);return;}constexpr std::array regs{Gpr::Rcx,Gpr::Rdx,Gpr::R8,Gpr::R9};if(index<4)state.reg(regs[index])=value;else memory.store(GuestAddress{state.reg(Gpr::Rsp)+0x28+(index-4)*8},64,value);};
        const bool move=action.symbol=="MoveWindow";set(move?1:2,rect[0]);set(move?2:3,rect[1]);set(move?3:4,rect[2]-rect[0]);set(move?4:5,rect[3]-rect[1]);
        if(!move){const WindowsCall arg(state,memory,7);auto flags=arg(6);if(rect[0]!=action.rectangle[0] || rect[1]!=action.rectangle[1])flags&=~UINT64_C(2);if(rect[2]-rect[0]!=action.rectangle[2]-action.rectangle[0] || rect[3]-rect[1]!=action.rectangle[3]-action.rectangle[1])flags&=~UINT64_C(1);set(6,flags);}
    }
    const auto symbol=action.symbol;const auto arguments=action.arguments;
    gui_.cbt_replay[action.thread]={state.rip.value,symbol};cancel_gui_cbt_action(frame.value,memory,state);
    const WindowsServiceDescriptor service{"user32.dll",symbol,arguments};
    if(symbol=="SetFocus" || symbol=="SetActiveWindow" || symbol=="SetForegroundWindow" || symbol=="MoveWindow" || symbol=="SetWindowPos")return invoke_gui_support(state,memory,service);
    return invoke_gui(state,memory,service);
}catch(const std::bad_alloc&){cancel_gui_cbt_action(frame.value,memory,state);last_error_=8;complete_call(state,memory,0);return {};}
catch(...){cancel_gui_cbt_action(frame.value,memory,state);throw;}
}
