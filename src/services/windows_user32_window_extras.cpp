// user32 視窗補充家族，全部以既有 user32／gdi32 服務組成：
// - InternalGetWindowIcon(hwnd, iconType)：依視窗圖示（WM_SETICON）、類別圖示、另一尺寸、預設應用程式圖示的順序取得，回傳複本（呼叫者 DestroyIcon）。
// - GetInternalWindowPos／SetInternalWindowPos：WINDOWPLACEMENT 的顯示狀態、一般位置與最小化位置。
// - DrawCaption／DrawCaptionTempA／W：DC_ACTIVE、DC_GRADIENT（水平漸層）、DC_ICON（小圖示）、DC_TEXT（標題列字型，DC_SMALLCAP 為小標題字型）、
//   DC_INBUTTON（按鈕底色）；可指定字型、圖示與文字，否則取視窗本身的。A 版文字交給 DrawTextA 沿用其字碼頁轉換。
// - GetAutoRotationState：沙箱顯示沒有方向感測器（AR_NOSENSOR）；Get／SetDisplayAutoRotationPreferences 保存程序的方向偏好。
// - GetDpiForMonitorInternal：shcore GetDpiForMonitor 的 BOOL 版本。
// - IsHungAppWindow：視窗所屬執行緒有訊息佇列、不在等待輸入（GetMessage／WaitMessage／MsgWait*），且超過 5 秒未取訊息即視為沒有回應；
//   尚未取過訊息的執行緒仍在啟動中，不算沒有回應。與 SendMessageTimeout 的 SMTO_ABORTIFHUNG 使用同一個取訊息時間。
// - RegisterShellHookWindow／DeregisterShellHookWindow：登錄視窗收到張貼的 RegisterWindowMessage("SHELLHOOK") 訊息，wParam 為 HSHELL_*。
//   工作列視窗（可見、頂層、無 owner 且非 WS_EX_TOOLWINDOW，或帶 WS_EX_APPWINDOW）出現／隱藏或銷毀時為 WINDOWCREATED／WINDOWDESTROYED，
//   標題變更為 REDRAW，移到另一個螢幕為 MONITORCHANGED，啟用的工作列視窗改變為 WINDOWACTIVATED（全螢幕視窗為 RUDEAPPACTIVATED）；
//   FlashWindow(Ex) 開始閃爍為 FLASH、停止為 REDRAW；頂層視窗未處理的 WM_APPCOMMAND 由 DefWindowProc 轉為 APPCOMMAND。
//   只通知與事件視窗同桌面的登錄視窗；收件範圍為本程序的視窗。首位登錄者以當下狀態為基準，既有視窗不補發建立通知。
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_userdraw.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
std::span<const WindowsServiceDescriptor> user32_window_extra_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","InternalGetWindowIcon",2,0,0,0x2},{"user32.dll","GetInternalWindowPos",3},{"user32.dll","SetInternalWindowPos",4,0,0,0x2},
        {"user32.dll","DrawCaption",4,0,0,0x8},{"user32.dll","DrawCaptionTempA",7,0,0,0x40},{"user32.dll","DrawCaptionTempW",7,0,0,0x40},
        {"user32.dll","GetAutoRotationState",1},{"user32.dll","GetDisplayAutoRotationPreferences",1},{"user32.dll","SetDisplayAutoRotationPreferences",1,0,0,0x1},
        {"user32.dll","GetDpiForMonitorInternal",4,0,0,0x2},{"user32.dll","IsHungAppWindow",1},
        {"user32.dll","RegisterShellHookWindow",1},{"user32.dll","DeregisterShellHookWindow",1}};
    return table;
}
bool WindowsServices::shell_hook_candidate(std::uint64_t handle) const {
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())return false;
    const auto& w=found->second;
    if(!w.visible || w.destroying || w.parent || (w.style&0x40000000U))return false;
    return (w.ex_style&0x40000U) || (!w.owner && !(w.ex_style&0x80U));
}
void WindowsServices::notify_shell_hook(std::uint32_t code,std::uint64_t lparam,std::uint64_t desktop){
    if(gui_.shell_hook_windows.empty())return;
    constexpr auto message=gui_registered_message(u"SHELLHOOK");
    for(const auto target:gui_.shell_hook_windows){
        const auto found=gui_.windows.find(target);
        if(found==gui_.windows.end() || found->second.destroying || found->second.desktop_object!=desktop)continue;
        if(gui_.queue.size()>=10000)break;
        enqueue_gui_message({target,code,lparam,message});
    }
}
void WindowsServices::poll_shell_hooks(bool announce){
    if(gui_.shell_hook_windows.empty())return;
    std::erase_if(gui_.shell_hook_windows,[&](std::uint64_t handle){return !gui_.windows.contains(handle);});
    auto& tracked=gui_.shell_hook_tracked;
    if(gui_.shell_hook_windows.empty()){tracked.clear();gui_.shell_hook_active=0;return;}
    const auto post=[&](std::uint32_t code,std::uint64_t lparam,std::uint64_t desktop){if(announce)notify_shell_hook(code,lparam,desktop);};
    for(auto it=tracked.begin();it!=tracked.end();){
        if(shell_hook_candidate(it->first)){++it;continue;}
        const auto handle=it->first,desktop=it->second.desktop;it=tracked.erase(it);post(2,handle,desktop);
    }
    for(const auto& [handle,window]:gui_.windows){
        if(!shell_hook_candidate(handle))continue;
        const auto monitor=gui_monitor_from_rect(gui_window_screen_rect(window),2);
        const auto [entry,inserted]=tracked.try_emplace(handle,GuiShellHookWindow{window.title,monitor,window.desktop_object});
        if(inserted){post(1,handle,window.desktop_object);continue;}
        if(entry->second.title!=window.title){entry->second.title=window.title;post(6,handle,window.desktop_object);}
        if(entry->second.monitor!=monitor){entry->second.monitor=monitor;post(16,handle,window.desktop_object);}
    }
    // 啟用的 owned popup 歸到其 owner 鏈上的工作列視窗；轉到非工作列視窗時不另行通知。
    auto root=gui_.active;
    for(unsigned depth=0;root && depth<64 && !tracked.contains(root);++depth){const auto found=gui_.windows.find(root);root=found==gui_.windows.end()?0:found->second.owner;}
    if(!root || !tracked.contains(root) || root==gui_.shell_hook_active)return;
    gui_.shell_hook_active=root;
    const auto& window=gui_.windows.at(root);const auto rect=gui_window_screen_rect(window);
    const bool full=std::any_of(gui_monitors().begin(),gui_monitors().end(),[&](const GuiMonitor& m){return rect[0]<=m.rect[0] && rect[1]<=m.rect[1] && rect[2]>=m.rect[2] && rect[3]>=m.rect[3];});
    post(full?0x8004U:4U,root,window.desktop_object);
}
std::optional<std::uint32_t> WindowsServices::invoke_user32_window_extras(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol.ends_with("ShellHookWindow")){
        const WindowsCall call(state,memory,service);const auto window=call(0);const auto found=gui_.windows.find(window);
        if(found==gui_.windows.end() || found->second.destroying){last_error_=1400;call.return_to(state,0);return std::nullopt;}
        if(service.symbol.starts_with("Register")){
            if(gui_.shell_hook_windows.empty()){gui_.shell_hook_tracked.clear();gui_.shell_hook_active=0;gui_.shell_hook_windows.insert(window);poll_shell_hooks(false);}
            else gui_.shell_hook_windows.insert(window);
        }else if(gui_.shell_hook_windows.erase(window) && gui_.shell_hook_windows.empty()){gui_.shell_hook_tracked.clear();gui_.shell_hook_active=0;}
        call.return_to(state,1);return std::nullopt;
    }
    return nested_continuation(state,memory,"user32 視窗補充",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const auto name=service.symbol;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto api=[&](std::string_view dll,std::string_view symbol,std::initializer_list<std::uint64_t> arguments){return nested_service(s,m,dll,symbol,arguments);};
        const auto guest=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
            co_return co_await service_callback::call_value(resolve(dll,symbol).value,std::move(arguments));
        };
        // 視窗圖示：ICON_BIG（1）先取大圖示，ICON_SMALL（0）／ICON_SMALL2（2）先取小圖示；再退回類別圖示與預設應用程式圖示。
        const auto window_icon=[&](std::uint64_t hwnd,std::uint32_t type)->std::uint64_t{
            const auto found=gui_.windows.find(hwnd);if(found==gui_.windows.end())return 0;
            const bool big=type==1;const auto& w=found->second;
            const auto class_icon=[&](bool large){return api("user32.dll","GetClassLongPtrW",{hwnd,large?static_cast<std::uint64_t>(-14):static_cast<std::uint64_t>(-34)});};
            for(const auto candidate:{big?w.window_icon:w.window_small_icon,class_icon(big),big?w.window_small_icon:w.window_icon,class_icon(!big)})
                if(candidate && gui_.icons.contains(candidate))return candidate;
            return api("user32.dll","LoadIconW",{0,32512});
        };
        if(name=="IsHungAppWindow"){
            const auto window=gui_.windows.find(call(0));if(window==gui_.windows.end()){last_error_=1400;co_return finish(0);}
            const auto thread=threads_.items.find(window->second.thread);
            if(thread==threads_.items.end() || thread->second.status==ThreadStatus::Exited || !gui_.message_threads.contains(thread->first))co_return finish(0);
            const auto& t=thread->second;
            const bool waiting=t.status==ThreadStatus::Waiting && t.wait.kind==ThreadWaitKind::GuiInput;
            co_return finish(!waiting && t.gui_pump_time && runtime_.clock_ticks>t.gui_pump_time && runtime_.clock_ticks-t.gui_pump_time>50000000U?1:0);
        }
        if(name=="GetAutoRotationState" || name=="GetDisplayAutoRotationPreferences"){
            if(!call(0)){last_error_=87;co_return finish(0);}
            m.store(GuestAddress{call(0)},32,name=="GetAutoRotationState"?0x10U:gui_.orientation_preference);co_return finish(1);
        }
        if(name=="SetDisplayAutoRotationPreferences"){
            // ORIENTATION_PREFERENCE：NONE 0 或 LANDSCAPE 1、PORTRAIT 2、LANDSCAPE_FLIPPED 4、PORTRAIT_FLIPPED 8 的組合。
            const auto value=static_cast<std::uint32_t>(call(0));if(value&~0xfU){last_error_=87;co_return finish(0);}
            gui_.orientation_preference=value;co_return finish(1);
        }
        if(name=="GetDpiForMonitorInternal"){
            const auto status=static_cast<std::uint32_t>(api("shcore.dll","GetDpiForMonitor",{call(0),call(1),call(2),call(3)}));
            if(status){last_error_=status==0x80070057U?87U:6U;co_return finish(0);}
            co_return finish(1);
        }
        if(name=="InternalGetWindowIcon"){
            if(!gui_.windows.contains(call(0))){last_error_=1400;co_return finish(0);}
            const auto type=static_cast<std::uint32_t>(call(1));if(type>2){last_error_=87;co_return finish(0);}
            const auto icon=window_icon(call(0),type);co_return finish(icon?api("user32.dll","CopyIcon",{icon}):0);
        }
        if(name=="GetInternalWindowPos" || name=="SetInternalWindowPos"){
            // WINDOWPLACEMENT {length=44, flags, showCmd@8, ptMinPosition@12, ptMaxPosition@20, rcNormalPosition@28}。
            const auto placement=scratch(44);m.store(GuestAddress{placement},32,44);
            if(!api("user32.dll","GetWindowPlacement",{call(0),placement}))co_return finish(0);
            if(name=="GetInternalWindowPos"){
                if(call(1)){std::array<std::uint8_t,16> rect{};m.read(GuestAddress{placement+28},rect);m.write(GuestAddress{call(1)},rect);}
                if(call(2)){std::array<std::uint8_t,8> point{};m.read(GuestAddress{placement+12},point);m.write(GuestAddress{call(2)},point);}
                co_return finish(m.load(GuestAddress{placement+8},32));
            }
            m.store(GuestAddress{placement+8},32,static_cast<std::uint32_t>(call(1)));
            if(call(2)){std::array<std::uint8_t,16> rect{};m.read(GuestAddress{call(2)},rect);m.write(GuestAddress{placement+28},rect);}
            if(call(3)){std::array<std::uint8_t,8> point{};m.read(GuestAddress{call(3)},point);m.write(GuestAddress{placement+12},point);
                m.store(GuestAddress{placement+4},32,m.load(GuestAddress{placement+4},32)|1U);} // WPF_SETMINPOSITION
            co_return finish(static_cast<std::uint32_t>(co_await guest("user32.dll","SetWindowPlacement",{call(0),placement})));
        }
        // DrawCaption(hwnd, hdc, lprect, flags)／DrawCaptionTemp(hwnd, hdc, lprect, hFont, hIcon, str, flags)。
        const bool temp=name.starts_with("DrawCaptionTemp"),ansi=name.ends_with('A');
        const auto hwnd=call(0),dc=call(1),source=call(2);
        const auto font_handle=temp?call(3):0,icon_handle=temp?call(4):0,text=temp?call(5):0;
        const auto flags=static_cast<std::uint32_t>(call(temp?6:3));
        if(!source || !gui_.contexts.contains(dc))co_return finish(0);
        if(hwnd && !gui_.windows.contains(hwnd)){last_error_=1400;co_return finish(0);}
        std::array<std::int32_t,4> r{};for(unsigned i=0;i<4;++i)r[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(m.load(GuestAddress{source+i*4},32)));
        const auto rect=scratch(16);const auto store_rect=[&](const std::array<std::int32_t,4>& value){for(unsigned i=0;i<4;++i)m.store(GuestAddress{rect+i*4},32,static_cast<std::uint32_t>(value[i]));};
        store_rect(r);
        const bool active=flags&1U,button=flags&0x10U;
        // 底色：按鈕樣式用 COLOR_3DFACE；漸層由標題列色到漸層色（COLOR_GRADIENT(IN)ACTIVECAPTION）；否則標題列色。
        if(!button && (flags&0x20U)){
            const auto from=gui_system_color(active?2U:3U),to=gui_system_color(active?27U:28U);
            // TRIVERTEX {x, y, Red, Green, Blue, Alpha}（16 bytes）×2 與 GRADIENT_RECT {0, 1}。
            const auto vertices=scratch(32),mesh=scratch(8);
            const auto vertex=[&](std::uint64_t at,std::int32_t x,std::int32_t y,std::uint32_t color){
                m.store(GuestAddress{at},32,static_cast<std::uint32_t>(x));m.store(GuestAddress{at+4},32,static_cast<std::uint32_t>(y));
                m.store(GuestAddress{at+8},16,(color&0xffU)<<8);m.store(GuestAddress{at+10},16,((color>>8)&0xffU)<<8);m.store(GuestAddress{at+12},16,((color>>16)&0xffU)<<8);m.store(GuestAddress{at+14},16,0);};
            vertex(vertices,r[0],r[1],from);vertex(vertices+16,r[2],r[3],to);m.store(GuestAddress{mesh},32,0);m.store(GuestAddress{mesh+4},32,1);
            (void)api("gdi32.dll","GdiGradientFill",{dc,vertices,2,mesh,1,0});
        }else (void)api("user32.dll","FillRect",{dc,rect,api("user32.dll","GetSysColorBrush",{button?15U:active?2U:3U})});
        auto x=r[0]+2;
        if(flags&4U){
            const auto size=static_cast<std::int32_t>(api("user32.dll","GetSystemMetrics",{49}));
            std::uint64_t icon=icon_handle,owned=0;
            if(!icon && hwnd){const auto base=window_icon(hwnd,0);if(base)owned=icon=api("user32.dll","CopyIcon",{base});}
            if(icon){(void)api("user32.dll","DrawIconEx",{dc,static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(r[1]+(r[3]-r[1]-size)/2),icon,static_cast<std::uint32_t>(size),static_cast<std::uint32_t>(size),0,0,3});x+=size+2;}
            if(owned)(void)api("user32.dll","DestroyIcon",{owned});
        }
        if(flags&8U){
            std::uint64_t font=font_handle,created=0;
            if(!font){
                // NONCLIENTMETRICSW：lfCaptionFont@24、lfSmCaptionFont@124（504 bytes）。
                const auto metrics=scratch(504);m.store(GuestAddress{metrics},32,504);
                if(api("user32.dll","SystemParametersInfoW",{0x29,504,metrics,0}))created=font=api("gdi32.dll","CreateFontIndirectW",{metrics+((flags&2U)?124U:24U)});
            }
            const auto previous=font?api("gdi32.dll","SelectObject",{dc,font}):0;
            const auto color=api("gdi32.dll","SetTextColor",{dc,gui_system_color(button?18U:active?9U:19U)});const auto mode=api("gdi32.dll","SetBkMode",{dc,1});
            store_rect({x,r[1],r[2]-2,r[3]});
            constexpr std::uint32_t format=0x20U|0x4U|0x8000U|0x800U; // DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX
            if(text)(void)api("user32.dll",ansi?"DrawTextA":"DrawTextW",{dc,text,UINT32_MAX,rect,format});
            else if(hwnd){
                const auto buffer=scratch(512*2);
                if(co_await guest("user32.dll","GetWindowTextW",{hwnd,buffer,512}))(void)api("user32.dll","DrawTextW",{dc,buffer,UINT32_MAX,rect,format});
            }
            (void)api("gdi32.dll","SetBkMode",{dc,mode});(void)api("gdi32.dll","SetTextColor",{dc,color});
            if(previous)(void)api("gdi32.dll","SelectObject",{dc,previous});
            if(created)(void)api("gdi32.dll","DeleteObject",{created});
        }
        co_return finish(1);
    });
}
}
