#include "gui_timing_layout.h"
#include "mouse_move_history.hpp"
#include "gui_cursor_bounds.hpp"
#include "windows_input_layout.hpp"
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_pointer.hpp"
#include "x64arm/windows_keyboard.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
std::span<const WindowsServiceDescriptor> input_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","SendInput",3},{"user32.dll","keybd_event",4},{"user32.dll","mouse_event",5},
        {"user32.dll","GetKeyNameTextA",3},{"user32.dll","GetKeyNameTextW",3},
        {"user32.dll","GetKeyboardType",1},
        {"user32.dll","ToUnicode",6},{"user32.dll","ToUnicodeEx",7},
        {"user32.dll","ToAscii",5},{"user32.dll","ToAsciiEx",6},
        {"user32.dll","VkKeyScanA",1},{"user32.dll","VkKeyScanW",1},
        {"user32.dll","VkKeyScanExA",2},{"user32.dll","VkKeyScanExW",2},
        {"user32.dll","ShellRegisterHotKey",5},{"user32.dll","RegisterHotKey",4},{"user32.dll","UnregisterHotKey",2},
        // 左右鍵對調（與 GetSystemMetrics(SM_SWAPBUTTON)、SPI_SETMOUSEBUTTONSWAP 共用狀態）與最近 64 筆滑鼠移動歷程。
        {"user32.dll","SwapMouseButton",1,0,0,0x1},{"user32.dll","GetMouseMovePointsEx",5,0,0,0x19}};
    return table;
}
namespace {
unsigned generic_key(unsigned key){return key==0xa0 || key==0xa1?16:key==0xa2 || key==0xa3?17:key==0xa4 || key==0xa5?18:key;}
unsigned physical_key(unsigned key,std::uint64_t parameter){
    if(key==16)return ((parameter>>16)&255U)==0x36?0xa1:0xa0;
    if(key==17)return parameter&0x1000000U?0xa3:0xa2;
    if(key==18)return parameter&0x1000000U?0xa5:0xa4;
    return key;
}
std::uint32_t modifiers(const std::array<std::uint8_t,256>& keys){return ((keys[16]&0x80)?1U:0)|((keys[17]&0x80)?2U:0)|((keys[18]&0x80)?4U:0);}
unsigned di_scan(unsigned scan){return (scan&0xff00U)==0xe100U?0xc5U:(scan&255U)|((scan&0xff00U)?0x80U:0);}
std::uint32_t mouse_keys(const std::array<std::uint8_t,256>& keys){
    std::uint32_t value{};for(const auto [vk,flag]:std::array<std::pair<unsigned,unsigned>,7>{{{1,1},{2,2},{16,4},{17,8},{4,16},{5,32},{6,64}}})if(keys[vk]&0x80)value|=flag;return value;
}
struct Injected {
    unsigned type{},key{},scan{};std::uint32_t flags{},time{},data{};std::int32_t x{},y{};std::uint64_t extra{};
};
}
bool WindowsServices::promote_pointer_message(std::uint64_t window,std::uint64_t wparam,std::uint32_t message){
    // 視窗未處理主要觸控／筆指標訊息時，DefWindowProc 依 Windows 契約轉成滑鼠左鍵輸入；
    // 額外資訊帶 MI_WP_SIGNATURE，觸控另設 0x80，供 GetMessageExtraInfo 辨識。
    if(!pointer_input_)return false;auto& bridge=*pointer_input_;
    const auto delivered=bridge.delivered.find(threads_.current);if(delivered==bridge.delivered.end())return false;
    const auto found=bridge.packets.find(delivered->second);if(found==bridge.packets.end())return false;
    const auto& packet=found->second;
    if(packet.id!=(wparam&0xffffU) || !(packet.flags&0x2000U) || packet.window!=window)return false;
    const auto device=bridge.devices.find(packet.device);if(device==bridge.devices.end() || device->second.device.type>3)return false;
    const bool touch=device->second.device.type==3;
    const std::array<std::int32_t,2> point{packet.pixel[0],packet.pixel[1]};
    gui_.pointer_position=point;
    auto [target,hit]=gui_mouse_hit(gui_,point[0],point[1],gui_.input_desktop);(void)hit;if(gui_.capture)target=gui_.capture;
    if(!target)return true;
    const auto send=[&](unsigned msg){
        const auto origin=gui_client_origin(gui_,target);
        const auto xy=(static_cast<std::uint64_t>(std::int64_t{point[0]}-origin[0])&65535U)|((static_cast<std::uint64_t>(std::int64_t{point[1]}-origin[1])&65535U)<<16);
        GuiMessage mouse{target,mouse_keys(gui_.input_keyboard),xy,msg};
        mouse.has_point=true;mouse.screen_point=true;mouse.point=point;mouse.modifiers=modifiers(gui_.input_keyboard);mouse.keyboard_snapshot=gui_.input_keyboard;
        mouse.injected=true;mouse.queued_time=packet.time;mouse.input_extra=0xff515700U|(touch?0x80U:0U)|(packet.id&0x7fU);mouse.queue_priority=1;
        mouse.source_device=static_cast<std::uint8_t>(touch?4U:8U);mouse.source_origin=static_cast<std::uint8_t>(device->second.injected?2U:1U);
        queue_gui_mouse(std::move(mouse));
    };
    const auto button=[&](bool down){auto& value=gui_.async_keyboard[1];if(down && !(value&0x80U))value|=1;value=static_cast<std::uint8_t>((value&1U)|(down?0x80U:0));gui_.input_keyboard[1]=down?0x80:0;};
    if(message==0x246){send(0x200);button(true);send(0x201);}
    else if(message==0x245){if(packet.flags&4U)send(0x200);}
    else {button(false);send(0x202);}
    gui_.last_input_ticks=std::uint64_t{packet.time}*10000;return true;
}
void WindowsServices::observe_gui_key(GuiMessage& message,bool injected){
    if(message.wparam>=256)return;
    const bool down=message.message==0x100 || message.message==0x104;
    const auto vk=physical_key(static_cast<unsigned>(message.wparam),message.lparam),generic=generic_key(vk);
    // 左右 Alt 的按下事件都會解除前景視窗鎖定。
    if(down && generic==0x12)gui_.foreground_locked=false;
    const auto update=[&](unsigned key,bool pressed,bool toggle){
        auto& input=gui_.input_keyboard[key];const bool before=input&0x80;
        input=static_cast<std::uint8_t>((input&1U)^(toggle && pressed && !before?1U:0U));if(pressed)input|=0x80;
        auto& async=gui_.async_keyboard[key];if(pressed && !(async&0x80))async|=1;
        async=static_cast<std::uint8_t>((async&1U)|(pressed?0x80U:0));
        gui_.injected_keys[key]=injected?(pressed?1:2):0;
    };
    if(!injected){for(const auto [key,flag]:std::array<std::pair<unsigned,unsigned>,3>{{{16,1},{17,2},{18,4}}}){
        if(gui_.injected_keys[key]!=1)update(key,message.modifiers&flag,false);
    }}
    if(vk==27 && down && !(gui_.input_keyboard[vk]&0x80U))++gui_.escape_presses;
    update(vk,down,vk==20 || vk==0x90 || vk==0x91);
    if(generic!=vk){const auto left=generic==16?0xa0U:generic==17?0xa2U:0xa4U;
        update(generic,(gui_.input_keyboard[left]|gui_.input_keyboard[left+1])&0x80U,false);
    }
    message.modifiers=modifiers(gui_.input_keyboard);message.keyboard_snapshot=gui_.input_keyboard;
    gui_.last_input_ticks=runtime_.clock_ticks;
}
void WindowsServices::consume_gui_input(const GuiMessage& message){
    // WNDPROC／timer 回呼可以重入 PeekMessage，先取走系統移動迴圈的輸入。
    // 記住實際移除的座標與結束訊號，待回呼返回後由同一個移動操作續接；
    // PM_NOREMOVE 不經此處，Read 階段則仍由原本的 MSG 緩衝區處理。
    for(auto& [id,move]:gui_.move_sizes){
        (void)id;using Stage=GuiMoveSize::Stage;
        if(move.thread!=threads_.current || gui_.capture!=move.window || gui_.capture_generation!=move.capture_generation ||
           (move.stage!=Stage::Initialize && move.stage!=Stage::Dispatch && move.stage!=Stage::Rect && move.stage!=Stage::Position))continue;
        const auto kind=message.message;
        if((kind==0x200 || kind==0xa0) && !move.nested_end)move.nested_point=message.queued_cursor;
        else if(kind==0x202 || kind==0xa2 || kind==0x1f || kind==0x215)move.nested_end=true;
        else if((kind==0x100 || kind==0x104) && !move.nested_end){
            if(message.wparam==27){move.cancel=true;move.nested_end=true;}
            else if(message.wparam==13)move.nested_end=true;
            else if(message.wparam>=37 && message.wparam<=40){
                auto point=move.nested_point.value_or(move.point);const auto key=message.wparam;
                point[key==37 || key==39?0:1]+=(key==37 || key==38?-1:1)*((message.modifiers&2U)?1:10);move.nested_point=point;
            }
        }else if(kind==18){move.quit=true;move.quit_code=static_cast<std::uint32_t>(message.wparam);move.nested_end=true;}
    }
    auto& queue=threads_.input_queue(threads_.current);
    auto& keyboard=queue.keys;
    if(message.keyboard_snapshot){
        // 只套用實體狀態的變化，保留 SetKeyboardState 對其他按鍵的佇列設定。
        const auto& physical=*message.keyboard_snapshot;
        for(unsigned i=0;i<physical.size();++i){
            const auto changed=static_cast<unsigned>(physical[i]^queue.last_physical[i]);
            if(changed&0x80U)keyboard[i]=(keyboard[i]&1U)|(physical[i]&0x80U);
            keyboard[i]^=changed&1U;
        }
        if((message.message==0x100 || message.message==0x101 || message.message==0x104 || message.message==0x105) && message.wparam<256){
            const auto vk=physical_key(static_cast<unsigned>(message.wparam),message.lparam);
            for(auto key:{vk,generic_key(vk)})keyboard[key]=(keyboard[key]&1U)|(physical[key]&0x80U);
        }
        queue.last_physical=physical;
        return;
    }
    const bool mouse=message.message>=0x200 && message.message<=0x20e;
    if((message.message>=0x100 && message.message<=0x107) || mouse){
        for(const auto [vk,flag]:std::array<std::pair<unsigned,unsigned>,3>{{{16,1},{17,2},{18,4}}})keyboard[vk]=static_cast<std::uint8_t>((keyboard[vk]&1U)|((message.modifiers&flag)?0x80U:0));
        if((message.message==0x100 || message.message==0x101 || message.message==0x104 || message.message==0x105) && message.wparam<256){
            const auto vk=physical_key(static_cast<unsigned>(message.wparam),message.lparam);const bool down=message.message==0x100 || message.message==0x104;
            auto& value=keyboard[vk];if(down && !(value&0x80) && (vk==20 || vk==0x90 || vk==0x91))value^=1;
            value=static_cast<std::uint8_t>((value&1U)|(down?0x80U:0));
            keyboard[generic_key(vk)]=static_cast<std::uint8_t>((keyboard[generic_key(vk)]&1U)|(down?0x80U:0));
        }
        if(mouse)for(const auto [vk,flag]:std::array<std::pair<unsigned,unsigned>,5>{{{1,1},{2,2},{4,16},{5,32},{6,64}}})keyboard[vk]=(message.wparam&flag)?0x80:0;
    }
}
bool WindowsServices::match_gui_hotkey(const GuiMessage& message){
    if(!raw_hotkeys_enabled())return false;
    if(message.wparam>=256)return false;const bool down=message.message==0x100 || message.message==0x104;
    const auto key=static_cast<unsigned>(message.wparam);const auto& keys=gui_.input_keyboard;
    const auto mods=((keys[18]&0x80)?1U:0)|((keys[17]&0x80)?2U:0)|((keys[16]&0x80)?4U:0)|(((keys[0x5b]|keys[0x5c])&0x80)?8U:0);
    bool consumed=false;
    for(auto& hotkey:gui_.hotkeys){
        if(hotkey.desktop!=gui_.input_desktop || hotkey.key!=key)continue;
        if(!down){hotkey.pressed=false;continue;}
        if((hotkey.modifiers&15U)!=mods)continue;consumed=true;
        if((!hotkey.pressed || (!(hotkey.modifiers&0x4000U) && (message.lparam&0x40000000U))) && gui_.queue.size()<10000){
            const auto recipient=hotkey.target_window?hotkey.target_window:hotkey.window;
            GuiMessage notify{recipient,hotkey.id,mods|(std::uint64_t{key}<<16),0x312};notify.thread=hotkey.target_window?gui_.windows.at(recipient).thread:hotkey.thread;
            notify.keyboard_snapshot=message.keyboard_snapshot;notify.input_extra=message.input_extra;notify.injected=message.injected;notify.queued_time=message.queued_time;
            notify.queued_cursor=gui_.pointer_position;enqueue_gui_message(std::move(notify));
            // WM_HOTKEY 在訊息佇列頭部；普通 posted/input 仍保留原來的優先序。
            std::rotate(gui_.queue.begin(),gui_.queue.end()-1,gui_.queue.end());
        }
        hotkey.pressed=true;
    }
    return consumed;
}
bool WindowsServices::translate_gui_key(const GuiMessage& message){
    if(message.message!=0x100 && message.message!=0x104 && message.message!=0x101 && message.message!=0x105)return false;
    std::u16string text;
    if(message.wparam==0xe7){
        if(message.message==0x100 || message.message==0x104)text.push_back(static_cast<char16_t>(message.lparam>>16));
    }else {
        const auto scan=static_cast<std::uint32_t>((message.lparam>>16)&255U)|((message.lparam&0x1000000U)?0x100U:0)|((message.message==0x101 || message.message==0x105)?0x8000U:0);
        text=windows_key_text(static_cast<std::uint32_t>(message.wparam),scan,threads_.input_queue(threads_.current).keys,0,gui_.keyboard_translation[threads_.current]);
    }
    // 字元訊息沿用剛取出之按鍵訊息的輸入來源（GetCurrentInputMessageSource）。
    const auto& translating=threads_.items.at(threads_.current);
    for(const auto character:text){GuiMessage output{message.window,character,message.wparam==0xe7?((message.lparam&65535U)|(message.message==0x104?0x20000000U:0)):message.lparam,message.message==0x104?0x106U:0x102U};output.thread=message.thread;
        if(translating.message_source_device==1){output.source_device=1;output.source_origin=translating.message_source_origin;}
        enqueue_gui_message(std::move(output));}
    // Win32 對四種 key message 回傳 TRUE，即使此鍵沒有產生字元。
    return true;
}
void WindowsServices::swap_gui_mouse_buttons(const GuestMemory& memory,GuiMessage& message) const {
    // SwapMouseButton：左右鍵訊息（含非用戶區）互換，MK_LBUTTON／MK_RBUTTON 互換；非同步按鍵狀態與 Raw Input 仍是實體按鍵。
    if(!gui_timing(memory,X64ARM_GUI_MOUSE_SWAP_OFFSET/4))return;
    const auto code=message.message;
    constexpr std::array<std::pair<std::uint32_t,std::uint32_t>,6> pairs{{{0x201,0x204},{0x202,0x205},{0x203,0x206},{0xa1,0xa4},{0xa2,0xa5},{0xa3,0xa6}}};
    for(const auto& [left,right]:pairs){if(code==left){message.message=right;break;}if(code==right){message.message=left;break;}}
    if((code>=0x200 && code<=0x20e) || code==0x10004)message.wparam=(message.wparam&~std::uint64_t{3})|((message.wparam&1U)<<1)|((message.wparam&2U)>>1);
}
void WindowsServices::record_mouse_move_point(std::int64_t x,std::int64_t y,std::uint32_t time,std::uint64_t extra){
    if(!interactive_station())return;
    const MouseMoveSample sample{x,y,time,std::bit_cast<std::int64_t>(extra)};
    initialize_shared_windows();
    if(shared_windows_){append_shared_mouse_move(*shared_windows_,sample);return;}
    auto& points=gui_.mouse_move_points;points.push_back(sample);
    if(points.size()>64)points.erase(points.begin(),points.begin()+static_cast<std::ptrdiff_t>(points.size()-64));
}
std::optional<std::uint32_t> WindowsServices::invoke_input_api(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    std::uint64_t result{};const auto fail=[&](unsigned error){last_error_=error;result=0;};
    try {
        if(name=="SwapMouseButton"){result=exchange_mouse_buttons(memory,d(0)!=0);}
        else if(name=="GetMouseMovePointsEx"){
            // MOUSEMOVEPOINT {int x, y; DWORD time; ULONG_PTR dwExtraInfo}。輸入點以 16 位元螢幕座標比對（time 非零時一併比對），
            // 由該點往前取最多 nBufPoints 筆；顯示座標回傳 16 位元值，高解析度座標換算成虛擬螢幕的 0～65535。
            const unsigned size=x86_?16U:24U;const auto count=static_cast<std::int32_t>(d(3));const auto mode=d(4);
            const auto failed=[&](std::uint32_t error){last_error_=error;result=UINT32_MAX;};
            if(d(0)!=size || count<0 || count>64)failed(87);
            else if(!arg(1) || (count && !arg(2)))failed(998);
            else if(mode!=1 && mode!=2)failed(1171);
            else if(!interactive_station())failed(5);
            else {
                const auto x=memory.load(GuestAddress{arg(1)},32)&0xffffU,y=memory.load(GuestAddress{arg(1)+4},32)&0xffffU;
                const auto time=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+8},32));
                initialize_shared_windows();
                const auto points=shared_windows_?shared_mouse_moves(*shared_windows_):gui_.mouse_move_points;auto found=points.rend();
                for(auto it=points.rbegin();it!=points.rend();++it)
                    if((static_cast<std::uint64_t>((*it)[0])&0xffffU)==x && (static_cast<std::uint64_t>((*it)[1])&0xffffU)==y && (!time || static_cast<std::uint32_t>((*it)[2])==time)){found=it;break;}
                if(found==points.rend())failed(1171); // ERROR_POINT_NOT_FOUND
                else {
                    const auto amount=std::min<std::size_t>(static_cast<unsigned>(count),static_cast<std::size_t>(points.rend()-found));
                    if(amount)memory.check(GuestAddress{arg(2)},amount*size,Access::Write);
                    const auto area=gui_virtual_screen_bounds(gui_monitors());
                    const auto normal=[&](std::int64_t value,unsigned axis){const auto span=std::max<std::int64_t>(1,std::int64_t{area[axis+2]}-area[axis]-1);
                        return static_cast<std::uint32_t>(std::clamp<std::int64_t>((value-area[axis])*65535/span,0,65535));};
                    std::uint32_t copied=0;
                    for(auto it=found;it!=points.rend() && copied<static_cast<std::uint32_t>(count);++it,++copied){
                        const auto at=arg(2)+std::uint64_t{copied}*size;const auto& point=*it;
                        memory.store(GuestAddress{at},32,mode==1?static_cast<std::uint64_t>(point[0])&0xffffU:normal(point[0],0));
                        memory.store(GuestAddress{at+4},32,mode==1?static_cast<std::uint64_t>(point[1])&0xffffU:normal(point[1],1));
                        memory.store(GuestAddress{at+8},32,static_cast<std::uint32_t>(point[2]));memory.store(GuestAddress{at+(x86_?12U:16U)},x86_?32:64,static_cast<std::uint64_t>(point[3]));
                    }
                    result=copied;
                }
            }
        }else if(name=="GetKeyboardType"){
            const auto& caps=windows_keyboard_capabilities;
            if(d(0)>2)fail(87);else result=d(0)==0?caps.type:d(0)==1?caps.subtype:caps.function_keys;
        }else if(name.starts_with("GetKeyNameText")){
            const auto capacity=std::bit_cast<std::int32_t>(d(2));const bool wide=name.ends_with('W');
            if(capacity<=0 || !arg(1))fail(87);
            else {const auto text=windows_key_name(d(0));bool replacement{};const auto bytes=wide?std::string{}:utf16_to_cp1252(text,'?',replacement);
                const auto count=std::min<std::size_t>(wide?text.size():bytes.size(),static_cast<unsigned>(capacity)-1);
                memory.check(GuestAddress{arg(1)},(count+1)*(wide?2:1),Access::Write);
                for(std::size_t i=0;i<=count;++i)memory.store(GuestAddress{arg(1)+i*(wide?2:1)},wide?16:8,i==count?0:wide?text[i]:static_cast<unsigned char>(bytes[i]));result=count;
            }
        }else if(name.starts_with("VkKeyScan")){
            result=UINT32_MAX;
            const bool wide=name.ends_with('W'),extended=name.starts_with("VkKeyScanEx");
            const auto layout=extended?arg(1):0;
            if(!windows_input_layout_supported(layout))last_error_=87;
            else {
                const char byte=static_cast<char>(d(0)&255U);
                const auto character=wide?static_cast<char16_t>(d(0)&65535U):cp1252_to_utf16(std::string_view(&byte,1)).front();
                // 反向使用同一鍵盤轉譯契約，避免 VkKeyScan 與 ToUnicode 的標點或修飾鍵不一致。
                bool matched=false;
                for(unsigned shift=0;shift<8 && !matched;++shift){
                    std::array<std::uint8_t,256> keys{};keys[16]=shift&1U?0x80:0;keys[17]=shift&2U?0x80:0;keys[18]=shift&4U?0x80:0;
                    for(unsigned key=1;key<255;++key){
                        if((key>=0x60 && key<=0x6f) || key==0xe7)continue; // VkKeyScan 不使用數字鍵盤或 Unicode 封包鍵。
                        GuiKeyboardTranslation translation{};
                        const auto output=windows_key_text(key,windows_map_virtual_key(key,4),keys,4,translation);
                        if(output.size()==1 && output.front()==character){result=key|(shift<<8);matched=true;break;}
                    }
                }
            }
        }else if(name.starts_with("ToUnicode") || name.starts_with("ToAscii")){
            const bool unicode=name.starts_with("ToUnicode"),extended=name.ends_with("Ex");
            const auto flags=d(unicode?5:4);const auto layout=extended?arg(unicode?6:5):0;
            const auto capacity=unicode?std::bit_cast<std::int32_t>(d(4)):2;
            if(!windows_input_layout_supported(layout))fail(87);
            else if(flags&~7U || d(0)>255 || capacity<=0 || !arg(3))fail(87);
            else if(unicode && !arg(2))result=0;
            else {
                std::array<std::uint8_t,256> keys=threads_.input_queue(threads_.current).keys;if(arg(2))memory.read(GuestAddress{arg(2)},keys);
                auto translation=gui_.keyboard_translation[threads_.current];const auto text=windows_key_text(d(0),d(1),keys,flags,translation);
                if(unicode){const auto count=std::min<std::size_t>(text.size(),static_cast<unsigned>(capacity));memory.check(GuestAddress{arg(3)},count*2,Access::Write);
                    for(std::size_t i=0;i<count;++i)memory.store(GuestAddress{arg(3)+i*2},16,text[i]);result=count;
                }else if(!text.empty()){
                    // ToAscii 的目的地是一個 WORD；依客體 ACP 輸出完整字元，回傳翻譯的字元數。
                    const auto translated=text.substr(0,2);
                    const auto bytes=utf16_to_utf8_lossy(translated);
                    if(bytes.size()>sizeof(std::uint16_t))fail(122);
                    else{
                        auto word=static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[0]));
                        if(bytes.size()==2)word|=static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[1]))<<8;
                        memory.store(GuestAddress{arg(3)},16,word);
                        result=translated.size();
                    }
                }
                gui_.keyboard_translation[threads_.current]=translation;
            }
        }else if(name=="RegisterHotKey" || name=="ShellRegisterHotKey" || name=="UnregisterHotKey"){
            const auto window=gui_.windows.find(arg(0));const auto desktop=thread_desktop_object(threads_.current);
            const auto target=name=="ShellRegisterHotKey"?arg(4):0;
            if((arg(0) && window==gui_.windows.end()) || (target && !gui_.windows.contains(target)))fail(1400);
            else if(target && thread_desktop_object(gui_.windows.at(target).thread)!=desktop)fail(5);
            else if(arg(0) && window->second.thread!=threads_.current)fail(1408);
            else if(d(1)>65535)fail(87);
            else if(name=="UnregisterHotKey"){
                const auto found=std::find_if(gui_.hotkeys.begin(),gui_.hotkeys.end(),[&](const auto& h){return h.window==arg(0) && h.id==d(1) && h.thread==threads_.current;});
                if(found==gui_.hotkeys.end())fail(1419);else {gui_.hotkeys.erase(found);result=1;}
            }else if(d(2)&~0x400fU || !d(3) || d(3)>254)fail(87);
            else if(d(3)==0x7b || std::any_of(gui_.hotkeys.begin(),gui_.hotkeys.end(),[&](const auto& h){return h.desktop==desktop && h.key==d(3) && (h.modifiers&15U)==(d(2)&15U);}))fail(1409);
            else if(gui_.hotkeys.size()>=4096)fail(8);
            else {prepare_input_backend();gui_.hotkeys.push_back({arg(0),threads_.current,desktop,d(1),d(2),d(3),false,target});result=1;}
        }else {
            std::vector<Injected> inputs;const auto tick=static_cast<std::uint32_t>(runtime_.clock_ticks/10000);
            if(name=="SendInput"){
                const unsigned size=x86_?28:40,offset=x86_?4:8;const auto count=d(0);
                if(d(2)!=size || count>4096 || (count && !arg(1))){fail(count>4096?8:87);arg.return_to(state,result);return std::nullopt;}
                if(!count){arg.return_to(state,0);return std::nullopt;}
                memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(size)*count,Access::Read);inputs.reserve(count);
                for(unsigned i=0;i<count;++i){const auto base=arg(1)+i*size,at=base+offset;
                    const auto load=[&](unsigned n,std::uint8_t bits=32){return memory.load(GuestAddress{at+n},bits);};Injected item;item.type=static_cast<unsigned>(memory.load(GuestAddress{base},32));
                    if(item.type==1){item.key=static_cast<unsigned>(load(0,16));item.scan=static_cast<unsigned>(load(2,16));item.flags=static_cast<std::uint32_t>(load(4));item.time=static_cast<std::uint32_t>(load(8));item.extra=load(x86_?12:16,x86_?32:64);}
                    else if(item.type==0){item.x=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(load(0)));item.y=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(load(4)));item.data=static_cast<std::uint32_t>(load(8));item.flags=static_cast<std::uint32_t>(load(12));item.time=static_cast<std::uint32_t>(load(16));item.extra=load(x86_?20:24,x86_?32:64);}
                    inputs.push_back(item);
                }
            }else if(name=="keybd_event")inputs.push_back({1,d(0)&255U,d(1)&255U,d(2),tick,0,0,0,arg(3)});
            else if(name=="mouse_event")inputs.push_back({0,0,0,d(0),tick,d(3),std::bit_cast<std::int32_t>(d(1)),std::bit_cast<std::int32_t>(d(2)),arg(4)});
            for(const auto& item:inputs){
                bool valid=item.type==0 || item.type==1;
                if(item.type==1){valid=!(item.flags&~15U) && ((item.flags&4U)?!item.key && !(item.flags&9U):((item.flags&8U)?item.scan>0 && item.scan<256 && windows_map_virtual_key(item.scan|((item.flags&1U)?0xe000U:0),3):item.key>0 && item.key<255));}
                if(item.type==0){valid=!(item.flags&~0xf9ffU) && !((item.flags&0x1800U)==0x1800U) && !((item.flags&0x1800U) && (item.flags&0x180U)) && (!(item.flags&0x180U) || (item.data && !(item.data&~3U)));}
                if(!valid){fail(item.type==2?50:87);arg.return_to(state,0);return std::nullopt;}
            }
            if(!interactive_station() || gui_.input_desktop!=thread_desktop_object(threads_.current)){fail(5);arg.return_to(state,0);return std::nullopt;}
            // 先消化原生事件，然後整批插入；不在 INPUT 陣列中間重入宿主事件迴圈。
            prepare_input_backend();pump_gui(memory,false);
            if(gui_.queue.size()+inputs.size()*8>10000){fail(1816);arg.return_to(state,0);return std::nullopt;}
            const auto enabled=[&](std::uint64_t handle){for(unsigned depth=0;handle && depth<64;++depth){const auto found=gui_.windows.find(handle);if(found==gui_.windows.end() || !found->second.enabled || found->second.desktop_object!=gui_.input_desktop)return false;handle=found->second.parent;}return true;};
            for(const auto& item:inputs){const auto time=item.time?item.time:tick;
                if(item.type==1){const bool up=item.flags&2U,packet=item.flags&4U;auto scan=item.scan,key=item.key;
                    if(!packet){if(item.flags&8U){scan=(scan&255U)|((item.flags&1U)?0xe000U:0);key=windows_map_virtual_key(scan,3);
                            if(!(scan&0xff00U) && (bool(gui_.input_keyboard[0x90]&1U)!=bool(gui_.input_keyboard[16]&0x80U))){constexpr std::array<unsigned,10> scans{0x52,0x4f,0x50,0x51,0x4b,0x4c,0x4d,0x47,0x48,0x49};const auto it=std::find(scans.begin(),scans.end(),scan);if(it!=scans.end())key=0x60+static_cast<unsigned>(it-scans.begin());if(scan==0x53)key=0x6e;}}
                        else {if(!scan)scan=windows_physical_scan(key);else scan=(scan&255U)|((item.flags&1U)?0xe000U:0);}
                        if(!key){fail(87);break;}
                    }else key=0xe7;
                    const auto target=gui_.focus?gui_.focus:gui_.active;const auto vk=physical_key(key,(std::uint64_t{scan&255U}<<16)|((scan&0xff00U)?0x1000000U:0));
                    const bool previous=gui_.input_keyboard[vk]&0x80U,alt=(gui_.input_keyboard[18]&0x80U) || generic_key(vk)==18;
                    const bool sys=!(gui_.input_keyboard[17]&0x80U) && (alt || key==0x79);
                    const auto parameter=packet?(UINT64_C(1)|(std::uint64_t{item.scan}<<16)):UINT64_C(1)|(std::uint64_t{scan&255U}<<16)|((!packet && (scan&0xff00U)==0xe000U)?0x1000000U:0)|(alt?0x20000000U:0)|((previous || up)?0x40000000U:0)|(up?0x80000000U:0);
                    GuiMessage message{target,generic_key(key),parameter,sys?(up?0x105U:0x104U):(up?0x101U:0x100U)};
                    message.queue_priority=1;message.injected=true;message.queued_time=time;message.input_extra=item.extra;message.source_device=1;message.source_origin=2;
                    observe_gui_key(message,true);gui_.last_input_ticks=std::uint64_t{time}*10000;if(!packet && scan)inject_dinput(true,di_scan(scan),up?0:1,time);
                    raw_input_keyboard(message,time,item.extra);
                    const bool hotkey=match_gui_hotkey(message);
                    if(raw_legacy_enabled(true) && !hotkey && target && enabled(target))enqueue_gui_message(std::move(message));
                }else {
                    const auto before=gui_.pointer_position;if(item.flags&1U){
                        if(item.flags&0x8000U){
                            const auto area=(item.flags&0x4000U)?gui_virtual_screen_bounds(gui_monitors()):gui_screen_area();
                            for(unsigned axis=0;axis<2;++axis){
                                const auto value=std::clamp(axis?item.y:item.x,0,65535);
                                const auto span=std::max<std::int64_t>(0,std::int64_t{area[axis+2]}-area[axis]-1);
                                gui_.pointer_position[axis]=static_cast<std::int32_t>(area[axis]+(value*span+32767)/65535);
                            }
                        }
                        else for(unsigned axis=0;axis<2;++axis)gui_.pointer_position[axis]=static_cast<std::int32_t>(std::clamp<std::int64_t>(std::int64_t{before[axis]}+(axis?item.y:item.x),INT32_MIN,INT32_MAX));
                        if(gui_.cursor_clip)gui_.pointer_position=gui_clip_cursor_point(gui_.pointer_position,*gui_.cursor_clip);
                        if(native_gui_){if(!native_gui_->set_cursor_position(gui_.pointer_position[0],gui_.pointer_position[1])){gui_.pointer_position=before;last_error_=5;break;}dinput_cursor_warped();}
                        for(unsigned axis=0;axis<2;++axis)inject_dinput(false,axis,std::int64_t{gui_.pointer_position[axis]}-before[axis],time);
                    }
                    const auto point=gui_.pointer_position;auto [target,hit]=gui_mouse_hit(gui_,point[0],point[1],gui_.input_desktop);if(gui_.capture){target=gui_.capture;hit=1;}
                    const auto send=[&](unsigned msg,unsigned high=0){if(!raw_legacy_enabled(false) || !target || !enabled(target))return;
                        const bool screen=msg==0x20a || msg==0x20e;const auto origin=screen?std::array<std::int64_t,2>{}:gui_client_origin(gui_,target);
                        const auto xy=(static_cast<std::uint64_t>(std::int64_t{point[0]}-origin[0])&65535U)|((static_cast<std::uint64_t>(std::int64_t{point[1]}-origin[1])&65535U)<<16);
                        GuiMessage message{target,mouse_keys(gui_.input_keyboard)|(std::uint64_t{high}<<16),xy,msg};
                        message.has_point=true;message.screen_point=true;message.point=point;
                        message.modifiers=modifiers(gui_.input_keyboard);message.keyboard_snapshot=gui_.input_keyboard;message.injected=true;message.queued_time=time;message.input_extra=item.extra;
                        message.mouse_no_coalesce=(item.flags&0x2000U)!=0;message.source_device=2;message.source_origin=2;
                        message.queue_priority=1;queue_gui_mouse(std::move(message));
                    };
                    if(item.flags&1U){record_mouse_move_point(point[0],point[1],time,item.extra);send(0x200);}
                    const auto button=[&](unsigned vk,unsigned object,bool down,unsigned message,unsigned high=0){
                        auto& value=gui_.async_keyboard[vk];if(down && !(value&0x80U))value|=1;value=static_cast<std::uint8_t>((value&1U)|(down?0x80U:0));
                        // SwapMouseButton：同步狀態與訊息改用對調後的邏輯按鍵；非同步狀態、DirectInput 與 Raw Input 仍是實體按鍵。
                        const auto logical=system_metric(memory,23) && (vk==1 || vk==2)?3U-vk:vk;if(logical!=vk)message=vk==1?message+3:message-3;
                        gui_.input_keyboard[logical]=down?0x80:0;gui_.injected_keys[vk]=down?1:2;inject_dinput(false,object,down?1:0,time);send(message,high);
                    };
                    for(const auto [flag,vk,object,down,msg]:std::array<std::tuple<unsigned,unsigned,unsigned,bool,unsigned>,6>{{{2,1,3,true,0x201},{4,1,3,false,0x202},{8,2,4,true,0x204},{16,2,4,false,0x205},{32,4,5,true,0x207},{64,4,5,false,0x208}}})if(item.flags&flag)button(vk,object,down,msg);
                    for(unsigned i=0;i<2;++i)if(item.data&(1U<<i)){if(item.flags&0x80U)button(5+i,6+i,true,0x20b,i+1);if(item.flags&0x100U)button(5+i,6+i,false,0x20c,i+1);}
                    if(item.flags&0x800U){inject_dinput(false,2,std::bit_cast<std::int32_t>(item.data),time);send(0x20a,item.data&65535U);}
                    if(item.flags&0x1000U)send(0x20e,item.data&65535U);
                    std::uint16_t raw_buttons=0;
                    for(const auto [flag,raw]:std::array<std::pair<unsigned,unsigned>,8>{{{2,1},{4,2},{8,4},{16,8},{32,16},{64,32},{0x800,0x400},{0x1000,0x800}}})if(item.flags&flag)raw_buttons|=static_cast<std::uint16_t>(raw);
                    for(unsigned i=0;i<2;++i)if(item.data&(1U<<i)){if(item.flags&0x80U)raw_buttons|=static_cast<std::uint16_t>(0x40U<<(i*2));if(item.flags&0x100U)raw_buttons|=static_cast<std::uint16_t>(0x80U<<(i*2));}
                    raw_input_mouse((item.flags&0x8000U?1U:0U)|(item.flags&0x4000U?2U:0U),item.flags&1U?item.x:0,item.flags&1U?item.y:0,raw_buttons,static_cast<std::uint16_t>(item.data),time,item.extra);
                    gui_.last_input_ticks=std::uint64_t{time}*10000;
                }
                ++result;
            }
            poll_dinput();if(name!="SendInput")result=0;
        }
    }catch(const MemoryFault&){last_error_=998;if(name!="SendInput")result=name=="GetMouseMovePointsEx"?UINT32_MAX:0;}catch(const std::bad_alloc&){last_error_=8;if(name!="SendInput")result=name=="GetMouseMovePointsEx"?UINT32_MAX:0;}
    arg.return_to(state,result);return std::nullopt;
}
}
