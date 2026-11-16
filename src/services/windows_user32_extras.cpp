#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include "x64arm/unicode.hpp"
#include <cmath>

// user32 補充家族：舊式掛鉤、MessageBoxEx、非同步顯示、輸入狀態、視窗模組路徑、DPI 程序查詢與主控執行緒行為。
// 全部轉接既有 user32／kernel32 實作；需要傳送視窗訊息（可能回呼客體視窗程序）者經 nested_continuation 續行。
namespace x64arm {
std::span<const WindowsServiceDescriptor> user32_extra_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","SetWindowsHookA",2,0,0,0x1},{"user32.dll","SetWindowsHookW",2,0,0,0x1},{"user32.dll","UnhookWindowsHook",2,0,0,0x1},
        {"user32.dll","MessageBoxExA",5,0,0,0x18},{"user32.dll","MessageBoxExW",5,0,0,0x18},
        {"user32.dll","ShowWindowAsync",2,0,0,0x2},{"user32.dll","CloseWindow",1},{"user32.dll","GetInputState",0},
        {"user32.dll","GetWindowModuleFileName",3,0,0,0x4},{"user32.dll","GetWindowModuleFileNameA",3,0,0,0x4},{"user32.dll","GetWindowModuleFileNameW",3,0,0,0x4},
        {"user32.dll","SetMessageQueue",1,0,0,0x1},{"user32.dll","IsImmersiveProcess",1},{"user32.dll","GetSystemDpiForProcess",1},
        {"user32.dll","GetThreadDpiHostingBehavior",0},{"user32.dll","SetThreadDpiHostingBehavior",1,0,0,0x1},
        // 視窗屬性列舉、執行緒 GUI 狀態、標題列、視窗字組、說明 ID、內部文字、切換與排列、圖示資訊、手勢設定、Shell 視窗與舊式 IME。
        {"user32.dll","EnumPropsA",2},{"user32.dll","EnumPropsW",2},{"user32.dll","EnumPropsExA",3},{"user32.dll","EnumPropsExW",3},
        {"user32.dll","GetGUIThreadInfo",2},{"user32.dll","GetTitleBarInfo",2},{"user32.dll","GetWindowWord",2},{"user32.dll","SetWindowWord",3},
        {"user32.dll","GetWindowContextHelpId",1},{"user32.dll","SetWindowContextHelpId",2},{"user32.dll","InternalGetWindowText",3},{"user32.dll","IsGUIThread",1},
        {"user32.dll","SwitchToThisWindow",2},{"user32.dll","IsWindowArranged",1},{"user32.dll","AnyPopup",0},{"user32.dll","GetAltTabInfoA",5},{"user32.dll","GetAltTabInfoW",5},
        {"user32.dll","GetAltTabInfo",5},{"user32.dll","GetListBoxInfo",1},{"user32.dll","CascadeWindows",5},{"user32.dll","TileWindows",5},
        {"user32.dll","CascadeChildWindows",2},{"user32.dll","TileChildWindows",2},{"user32.dll","ArrangeIconicWindows",1},
        {"user32.dll","GetIconInfoExA",2},{"user32.dll","GetIconInfoExW",2},{"user32.dll","OemKeyScan",1},{"user32.dll","GetDpiAwarenessContextForProcess",1},
        {"user32.dll","SystemParametersInfoForDpi",5},{"user32.dll","GetGestureConfig",6},{"user32.dll","SetGestureConfig",5},{"user32.dll","GetGestureInfo",2},
        {"user32.dll","GetGestureExtraArgs",3},{"user32.dll","CloseGestureInfoHandle",1},{"user32.dll","GetProgmanWindow",0},{"user32.dll","SetProgmanWindow",1},
        {"user32.dll","GetTaskmanWindow",0},{"user32.dll","SetTaskmanWindow",1},{"user32.dll","WINNLSEnableIME",2},{"user32.dll","WINNLSGetEnableStatus",1},
        {"user32.dll","PaintDesktop",1},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_user32_extra(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    // 會傳送視窗訊息或呼叫客體回呼的項目經協程續行。
    if(name.starts_with("EnumProps")){
        return nested_continuation(state,memory,"User32Props",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const bool wide=service.symbol.ends_with('W'),extended=service.symbol.starts_with("EnumPropsEx");
            const auto done=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            const auto window=gui_.windows.find(call(0));if(window==gui_.windows.end()){last_error_=1400;co_return done(UINT32_MAX);}
            if(!call(1)){last_error_=87;co_return done(UINT32_MAX);}
            // 先取快照：回呼中可安全增刪屬性。字串 atom 傳名稱，整數 atom 直接傳數值；沒有屬性時回 -1。
            std::vector<std::pair<std::uint16_t,std::uint64_t>> entries;for(const auto& [key,property]:window->second.properties)entries.push_back({key.second,property.value});
            GuestScratch scratch(*this,m,s);const auto buffer=scratch.allocate(512);std::uint64_t result=UINT32_MAX;
            const auto atom_name=resolve("kernel32.dll",wide?"GlobalGetAtomNameW":"GlobalGetAtomNameA").value;
            for(const auto& [atom,value]:entries){
                std::uint64_t text=atom;
                if(atom>=0xc000){const auto length=co_await service_callback::call(atom_name,{atom,buffer,255});if(!length)continue;text=buffer;}
                std::vector<std::uint64_t> args{call(0),text,value};if(extended)args.push_back(call(2));
                result=static_cast<std::uint32_t>(co_await service_callback::call(call(1),std::move(args)));if(!result)break;
            }
            co_return done(result);
        });
    }
    if(name=="SwitchToThisWindow" || name=="GetListBoxInfo" || name=="CascadeWindows" || name=="TileWindows" || name=="CascadeChildWindows" || name=="TileChildWindows" || name=="ArrangeIconicWindows"){
        return nested_continuation(state,memory,"User32Arrange",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto symbol=service.symbol;
            const auto user=[&](std::string_view target){const auto stub=host_service_stub("user32.dll",target);return stub.value?stub.value:resolve("user32.dll",target).value;};
            const auto done=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            if(symbol=="GetListBoxInfo")co_return done(co_await service_callback::call(user("SendMessageW"),{call(0),0x1b2,0,0})); // LB_GETLISTBOXINFO
            if(symbol=="SwitchToThisWindow"){
                // 最小化時先還原，再帶到前景。
                const auto window=gui_.windows.find(call(0));if(window==gui_.windows.end()){last_error_=1400;co_return done(0);}
                if(window->second.style&0x20000000U)(void)co_await service_callback::call(user("ShowWindow"),{call(0),9});
                (void)co_await service_callback::call(user("SetForegroundWindow"),{call(0)});co_return done(0);
            }
            // (parent, how, rect, count, kids)；Cascade／TileChildWindows 為 (parent, how)。NULL 父視窗為桌面（排列本程序的頂層視窗）。
            const bool children_only=symbol.ends_with("ChildWindows"),iconic=symbol=="ArrangeIconicWindows";const auto parent=call(0);const auto how=iconic?0U:static_cast<std::uint32_t>(call(1));
            if(parent && !gui_.windows.contains(parent)){last_error_=1400;co_return done(0);}
            std::vector<std::uint64_t> targets;
            if(!children_only && !iconic && call(3) && call(4)){for(std::uint32_t i=0;i<static_cast<std::uint32_t>(call(3));++i)targets.push_back(m.load(GuestAddress{call(4)+i*(x86_?4U:8U)},x86_?32:64));}
            else for(const auto& [handle,window]:gui_.windows)if(window.parent==parent && window.visible && !window.destroying && (parent || !window.owner || window.style&0x00C00000U))targets.push_back(handle);
            // 依目前 Z 序之外的建立順序排列；略過最小化（排列圖示時只取最小化）、以及 MDITILE_SKIPDISABLED（2）時停用的視窗。
            std::erase_if(targets,[&](std::uint64_t h){const auto w=gui_.windows.find(h);if(w==gui_.windows.end())return true;const bool minimized=w->second.style&0x20000000U;
                return iconic?!minimized:(minimized || ((how&2U) && !w->second.enabled));});
            std::array<std::int32_t,4> area{};GuestScratch scratch(*this,m,s);const auto rect=scratch.allocate(16);
            if(!children_only && !iconic && call(2))for(unsigned i=0;i<4;++i)area[i]=static_cast<std::int32_t>(m.load(GuestAddress{call(2)+i*4},32));
            else if(parent){(void)co_await service_callback::call(user("GetClientRect"),{parent,rect});for(unsigned i=0;i<4;++i)area[i]=static_cast<std::int32_t>(m.load(GuestAddress{rect+i*4},32));}
            else {(void)co_await service_callback::call(user("SystemParametersInfoW"),{0x30,0,rect,0});for(unsigned i=0;i<4;++i)area[i]=static_cast<std::int32_t>(m.load(GuestAddress{rect+i*4},32));}
            const auto metric=[&](std::uint32_t index)->service_callback::Task<std::int32_t>{co_return static_cast<std::int32_t>(co_await service_callback::call(user("GetSystemMetrics"),{index}));};
            const auto width=area[2]-area[0],height=area[3]-area[1];std::uint32_t arranged=0;
            const auto place=[&](std::uint64_t h,std::int32_t x,std::int32_t y,std::int32_t w,std::int32_t hgt,std::uint32_t flags)->service_callback::Task<bool>{
                co_return co_await service_callback::call(user("SetWindowPos"),{h,0,static_cast<std::uint32_t>(x),static_cast<std::uint32_t>(y),static_cast<std::uint32_t>(w),static_cast<std::uint32_t>(hgt),flags})!=0;};
            if(iconic){
                // 最小化的子視窗沿工作區底邊由左至右排列；回傳一列的高度。
                const auto spacing_x=co_await metric(47),spacing_y=co_await metric(48);std::int32_t x=area[0];std::int32_t y=area[3]-spacing_y;
                for(const auto h:targets){if(co_await place(h,x,y,0,0,0x15))++arranged;x+=spacing_x;if(x+spacing_x>area[2]){x=area[0];y-=spacing_y;}}
                co_return done(targets.empty()?0:static_cast<std::uint32_t>(spacing_y));
            }
            if(targets.empty())co_return done(0);
            if(symbol.starts_with("Cascade")){
                // 每個視窗相對前一個右下移一個標題列加框寬；大小為排列區域的四分之三。
                const auto step=co_await metric(4)+co_await metric(33);const auto w=width*3/4,hgt=height*3/4;std::int32_t offset=0;
                for(auto it=targets.rbegin();it!=targets.rend();++it){if(offset+hgt>height || offset+w>width)offset=0;if(co_await place(*it,area[0]+offset,area[1]+offset,w,hgt,0x14))++arranged;offset+=step;}
            }else {
                // MDITILE_HORIZONTAL（1）上下排列，否則左右排列；超過三個時排成近似正方形的格子。
                const auto n=static_cast<std::int32_t>(targets.size());std::int32_t columns=n<=3?n:static_cast<std::int32_t>(std::ceil(std::sqrt(static_cast<double>(n))));std::int32_t rows=(n+columns-1)/columns;
                if(how&1U)std::swap(columns,rows);
                for(std::int32_t i=0;i<n;++i){const auto c=(how&1U)?i/rows:i%columns,r=(how&1U)?i%rows:i/columns;
                    const auto x0=area[0]+width*c/columns,x1=area[0]+width*(c+1)/columns,y0=area[1]+height*r/rows,y1=area[1]+height*(r+1)/rows;
                    if(co_await place(targets[static_cast<std::size_t>(i)],x0,y0,x1-x0,y1-y0,0x14))++arranged;}
            }
            co_return done(arranged);
        });
    }
    if(name.starts_with("MessageBoxEx") || name=="ShowWindowAsync" || name=="CloseWindow"){
        return nested_continuation(state,memory,"User32Extra",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const auto symbol=service.symbol;
            const auto user=[&](std::string_view target){const auto stub=host_service_stub("user32.dll",target);return stub.value?stub.value:resolve("user32.dll",target).value;};
            const auto done=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            if(symbol.starts_with("MessageBoxEx")){
                // (Owner, Text, Caption, Type, LanguageId)：語言識別碼只影響按鈕文字的語系選擇，與 MessageBox 同一流程。
                co_return done(co_await service_callback::call(user(symbol.ends_with('A')?"MessageBoxA":"MessageBoxW"),{call(0),call(1),call(2),static_cast<std::uint32_t>(call(3))}));
            }
            // ShowWindowAsync 對本程序視窗直接套用顯示狀態並回 TRUE；CloseWindow 為最小化（SW_MINIMIZE）。
            if(!co_await service_callback::call(user("IsWindow"),{call(0)})){last_error_=1400;co_return done(0);}
            (void)co_await service_callback::call(user("ShowWindow"),{call(0),symbol=="CloseWindow"?6U:static_cast<std::uint32_t>(call(1))});
            co_return done(1);
        });
    }
    const WindowsCall arg(state,memory,service);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto host=[&](std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,family,symbol,args);};
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    if(name=="SetWindowsHookA" || name=="SetWindowsHookW"){
        // 舊式掛鉤：(idHook, HookProc) 等同只掛在目前執行緒的 SetWindowsHookEx；記住 HHOOK 供 UnhookWindowsHook 以程序位址找回。
        const auto thread=thread_identifier(threads_.current);
        auto hook=host("user32.dll",name.ends_with('A')?"SetWindowsHookExA":"SetWindowsHookExW",{u32(0),arg(1),0,thread});if(x86_)hook&=UINT32_MAX;
        if(hook)legacy_hooks_[{threads_.current,u32(0),arg(1)}]=hook;
        return done(hook);
    }
    if(name=="UnhookWindowsHook"){
        const auto found=legacy_hooks_.find({threads_.current,u32(0),arg(1)});
        if(found==legacy_hooks_.end()){last_error_=1404;return done(0);} // ERROR_INVALID_HOOK_HANDLE
        const auto result=host("user32.dll","UnhookWindowsHookEx",{found->second});legacy_hooks_.erase(found);return done(result);
    }
    if(name=="GetInputState"){
        // 佇列中有滑鼠按鍵或鍵盤訊息時回非零（GetQueueStatus 的 QS_KEY｜QS_MOUSEBUTTON 高位字）。
        return done((host("user32.dll","GetQueueStatus",{0x5U})>>16)!=0?1U:0U);
    }
    if(name.starts_with("GetWindowModuleFileName")){
        // 視窗建立時的 HINSTANCE 所屬模組路徑；無尾碼版本為 ANSI。
        if(!host("user32.dll","IsWindow",{arg(0)})){last_error_=1400;return done(0);}
        const auto instance=host("user32.dll","GetWindowLongPtrW",{arg(0),x86_?UINT32_MAX-5U:UINT64_MAX-5U});
        return done(host("kernel32.dll",name.ends_with('W')?"GetModuleFileNameW":"GetModuleFileNameA",{instance,arg(1),u32(2)}));
    }
    if(name=="SetMessageQueue")return done(u32(0)?1U:0U); // 32 位元 Windows 的訊息佇列自動擴充；此函式只為相容保留
    if(name=="IsImmersiveProcess"){
        // 引擎只執行傳統桌面程式；有效程序 handle 回 FALSE，無效 handle 設 ERROR_INVALID_HANDLE。
        if(arg(0)!=(x86_?UINT32_MAX:UINT64_MAX) && !process_handle_pid(arg(0),0x1000U)){last_error_=6;return done(0);}
        last_error_=0;return done(0);
    }
    if(name=="GetSystemDpiForProcess"){
        if(arg(0)!=(x86_?UINT32_MAX:UINT64_MAX) && !process_handle_pid(arg(0),0x1000U))return done(0);
        return done(host("user32.dll","GetDpiForSystem",{}));
    }
    if(name=="GetThreadDpiHostingBehavior")return done(dpi_hosting_behavior_[threads_.current]);
    if(name=="SetThreadDpiHostingBehavior"){
        // DPI_HOSTING_BEHAVIOR：DEFAULT＝0、MIXED＝1；回傳先前值，無效值回 INVALID（-1）。
        if(u32(0)>1)return done(UINT32_MAX);
        auto& value=dpi_hosting_behavior_[threads_.current];const auto previous=value;value=u32(0);return done(previous);
    }
    const auto window_of=[&](std::uint64_t handle)->GuiWindow*{const auto found=gui_.windows.find(handle);if(found==gui_.windows.end()){last_error_=1400;return nullptr;}return &found->second;};
    if(name=="GetGUIThreadInfo"){
        // GUITHREADINFO：cbSize、flags、active／focus／capture／menu owner／move-size／caret 視窗、caret 矩形（caret 視窗的工作區座標）；執行緒 0 為前景執行緒。
        const unsigned unit=x86_?4U:8U,size=8+6*unit+16;if(!arg(1) || memory.load(GuestAddress{arg(1)},32)!=size){last_error_=87;return done(0);}
        std::uint64_t thread=0;
        if(!u32(0)){const auto active=gui_.windows.find(gui_.active);if(active==gui_.windows.end()){last_error_=87;return done(0);}thread=active->second.thread;}
        else {thread=thread_from_identifier(u32(0));if(!thread){last_error_=87;return done(0);}}
        const auto owned=[&](std::uint64_t h){const auto w=gui_.windows.find(h);return w!=gui_.windows.end() && w->second.thread==thread?h:0;};
        std::uint32_t flags=0;std::uint64_t menu_owner=0,move_size=0,caret_window=0;std::array<std::int32_t,4> caret{};
        for(const auto& [id,track]:gui_.menu_tracks){(void)id;if(track.thread==thread && !track.cancelled){menu_owner=track.owner;flags|=track.main_bar?4U:0x14U;}}
        for(const auto& [id,operation]:gui_.move_sizes){(void)id;if(operation.thread==thread){move_size=operation.window;flags|=2U;}}
        if(const auto found=gui_.carets.find(thread);found!=gui_.carets.end()){caret_window=found->second.window;caret={found->second.x,found->second.y,found->second.x+static_cast<std::int32_t>(found->second.width),found->second.y+static_cast<std::int32_t>(found->second.height)};if(!found->second.hidden)flags|=1U;}
        const std::array<std::uint64_t,6> windows{owned(gui_.active),owned(gui_.focus),owned(gui_.capture),menu_owner,move_size,caret_window};
        memory.store(GuestAddress{arg(1)+4},32,flags);for(unsigned i=0;i<6;++i)memory.store(GuestAddress{arg(1)+8+i*unit},x86_?32:64,windows[i]);
        for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(1)+8+6*unit+i*4},32,static_cast<std::uint32_t>(caret[i]));return done(1);
    }
    if(name=="GetTitleBarInfo"){
        // TITLEBARINFO（44 位元組）：標題列矩形（螢幕座標、不含外框）與六個狀態：標題列、保留、最小化、最大化、說明、關閉。
        auto* window=window_of(arg(0));if(!window)return done(0);if(!arg(1) || memory.load(GuestAddress{arg(1)},32)!=44){last_error_=87;return done(0);}
        GuestScratch scratch(*this,memory,state);const auto rect=scratch.allocate(16),frame=scratch.allocate(16);
        if(!host("user32.dll","GetWindowRect",{arg(0),rect}))return done(0);memory.fill(GuestAddress{frame},0,16);
        (void)host("user32.dll","AdjustWindowRectEx",{frame,window->style,0,window->ex_style});
        const auto at=[&](std::uint64_t base,unsigned i){return static_cast<std::int32_t>(memory.load(GuestAddress{base+i*4},32));};
        const bool caption=(window->style&0x00C00000U)==0x00C00000U,tool=window->ex_style&0x80U;
        const auto caption_height=caption?static_cast<std::int32_t>(host("user32.dll","GetSystemMetrics",{tool?51U:4U})):0;
        const auto border_x=-at(frame,0),border_top=-at(frame,1)-caption_height;
        const std::array<std::int32_t,4> bar{at(rect,0)+border_x,at(rect,1)+border_top,at(rect,2)-border_x,at(rect,1)+border_top+caption_height};
        for(unsigned i=0;i<4;++i)memory.store(GuestAddress{arg(1)+4+i*4},32,static_cast<std::uint32_t>(bar[i]));
        constexpr std::uint32_t unavailable=1,invisible=0x8000,focusable=0x100000;
        const bool sysmenu=caption && (window->style&0x80000U),minimize=sysmenu && (window->style&0x20000U),maximize=sysmenu && (window->style&0x10000U);
        const bool help=sysmenu && (window->ex_style&0x400U) && !minimize && !maximize;
        const std::array<std::uint32_t,6> states{caption?focusable|(gui_.active==arg(0)?0U:0U):invisible,0,
            minimize?0U:maximize?unavailable:invisible,maximize?0U:minimize?unavailable:invisible,help?0U:invisible,sysmenu?0U:invisible};
        for(unsigned i=0;i<6;++i)memory.store(GuestAddress{arg(1)+20+i*4},32,states[i]);return done(1);
    }
    if(name=="GetWindowWord" || name=="SetWindowWord"){
        // 正位移讀寫額外視窗記憶體的 16 位元值；GetWindowWord 另接受 GWL_HINSTANCE（-6）、GWL_HWNDPARENT（-8）與 GWL_ID（-12）取低位字。
        auto* window=window_of(arg(0));if(!window)return done(0);const auto index=std::bit_cast<std::int32_t>(u32(1));
        if(index<0){if(name=="SetWindowWord" || (index!=-6 && index!=-8 && index!=-12)){last_error_=1413;return done(0);}
            return done(host("user32.dll","GetWindowLongPtrW",{arg(0),static_cast<std::uint64_t>(static_cast<std::int64_t>(index))})&0xffffU);}
        if(static_cast<std::size_t>(index)+2>window->extra.size()){last_error_=1413;return done(0);}
        const auto previous=static_cast<std::uint32_t>(window->extra[static_cast<std::size_t>(index)]|(window->extra[static_cast<std::size_t>(index)+1]<<8));
        if(name=="SetWindowWord"){window->extra[static_cast<std::size_t>(index)]=static_cast<std::uint8_t>(u32(2));window->extra[static_cast<std::size_t>(index)+1]=static_cast<std::uint8_t>(u32(2)>>8);}
        return done(previous);
    }
    if(name=="GetWindowContextHelpId"){auto* window=window_of(arg(0));return done(window?window->context_help_id:0U);}
    if(name=="SetWindowContextHelpId"){auto* window=window_of(arg(0));if(!window)return done(0);window->context_help_id=u32(1);return done(1);}
    if(name=="InternalGetWindowText"){
        // 直接取視窗標題，不傳送 WM_GETTEXT；回傳複製的字元數。
        auto* window=window_of(arg(0));if(!window)return done(0);const auto capacity=std::bit_cast<std::int32_t>(u32(2));if(!arg(1) || capacity<=0){last_error_=87;return done(0);}
        auto text=window->title.substr(0,static_cast<std::size_t>(capacity-1));for(std::size_t i=0;i<text.size();++i)memory.store(GuestAddress{arg(1)+i*2},16,text[i]);
        memory.store(GuestAddress{arg(1)+text.size()*2},16,0);return done(text.size());
    }
    if(name=="IsGUIThread"){
        // 已有訊息佇列的執行緒為 GUI 執行緒；bConvert 時建立佇列。
        const bool gui=gui_.message_threads.contains(threads_.current) || std::any_of(gui_.windows.begin(),gui_.windows.end(),[&](const auto& item){return item.second.thread==threads_.current;});
        if(!gui && u32(0)){gui_.message_threads.insert(threads_.current);return done(1);}return done(gui?1U:0U);
    }
    if(name=="IsWindowArranged"){window_of(arg(0));return done(0);} // 視窗管理員不提供貼齊排列
    if(name=="AnyPopup"){
        // 螢幕上有可見的擁有者視窗或 WS_POPUP 頂層視窗。
        return done(std::any_of(gui_.windows.begin(),gui_.windows.end(),[](const auto& item){const auto& w=item.second;return !w.parent && w.visible && !w.destroying && (w.owner || (w.style&0x80000000U));})?1U:0U);
    }
    if(name.starts_with("GetAltTabInfo")){
        // 只有 Alt+Tab 切換視窗本身可查詢；本桌面沒有這個視窗。
        last_error_=1400;return done(0);
    }
    if(name=="GetIconInfoExA" || name=="GetIconInfoExW"){
        // ICONINFOEX：ICONINFO 欄位後接 wResID、szModName[MAX_PATH]、szResName[MAX_PATH]（A 為字元、W 為 WCHAR）。
        const bool wide=name.ends_with('W');const unsigned unit=x86_?4U:8U,fixed=16+2*unit,character=wide?2U:1U;
        const auto size=(fixed+2+520*character+unit-1)/unit*unit;if(!arg(1) || memory.load(GuestAddress{arg(1)},32)!=size){last_error_=87;return done(0);}
        const auto icon=gui_.icons.find(arg(0));if(icon==gui_.icons.end()){last_error_=1402;return done(0);}
        const auto resource_instance=icon->second.resource_instance;const auto resource_id=icon->second.resource_name_id;const auto resource_name=icon->second.resource_name;
        GuestScratch scratch(*this,memory,state);const auto info=scratch.allocate(x86_?20:32);if(!host("user32.dll","GetIconInfo",{arg(0),info}))return done(0);
        memory.store(GuestAddress{arg(1)+4},32,memory.load(GuestAddress{info},32));memory.store(GuestAddress{arg(1)+8},32,memory.load(GuestAddress{info+4},32));memory.store(GuestAddress{arg(1)+12},32,memory.load(GuestAddress{info+8},32));
        memory.store(GuestAddress{arg(1)+16},x86_?32:64,memory.load(GuestAddress{info+(x86_?12U:16U)},x86_?32:64));memory.store(GuestAddress{arg(1)+16+unit},x86_?32:64,memory.load(GuestAddress{info+(x86_?16U:24U)},x86_?32:64));
        memory.store(GuestAddress{arg(1)+fixed},16,resource_id);memory.fill(GuestAddress{arg(1)+fixed+2},0,520*character);
        if(resource_instance)(void)host("kernel32.dll",wide?"GetModuleFileNameW":"GetModuleFileNameA",{resource_instance,arg(1)+fixed+2,260});
        const auto names=arg(1)+fixed+2+260*character;
        if(wide)for(std::size_t i=0;i<std::min<std::size_t>(resource_name.size(),259);++i)memory.store(GuestAddress{names+i*2},16,resource_name[i]);
        else {const auto narrow=utf16_to_utf8_lossy(resource_name);for(std::size_t i=0;i<std::min<std::size_t>(narrow.size(),259);++i)memory.store(GuestAddress{names+i},8,static_cast<std::uint8_t>(narrow[i]));}
        return done(1);
    }
    if(name=="OemKeyScan"){
        // OEM 字元先依 OEM 碼頁轉成 Unicode，再取虛擬鍵與掃描碼；高位字為 Shift 狀態，無法對應時回 0xFFFFFFFF。
        GuestScratch scratch(*this,memory,state);const auto in=scratch.allocate(4),out=scratch.allocate(4);memory.store(GuestAddress{in},8,u32(0)&255U);
        if(!host("kernel32.dll","MultiByteToWideChar",{1,0,in,1,out,2}))return done(UINT32_MAX);
        const auto vk=static_cast<std::uint32_t>(host("user32.dll","VkKeyScanW",{memory.load(GuestAddress{out},16)}));if((vk&0xffffU)==0xffffU)return done(UINT32_MAX);
        const auto scan=static_cast<std::uint32_t>(host("user32.dll","MapVirtualKeyW",{vk&0xffU,0}));if(!scan)return done(UINT32_MAX);return done(((vk>>8)&0xffU)<<16|scan);
    }
    if(name=="GetDpiAwarenessContextForProcess"){
        // 本程序（NULL 或本程序 handle）回程序 DPI 感知內容；其他程序未登記時視為未感知。
        if(!arg(0) || current_process_handle(arg(0)))return done(gui_.dpi_context);
        if(!process_handle_pid(arg(0),0x1000U)){last_error_=6;return done(0);}return done(x86_?0xffffffffU:UINT64_MAX);
    }
    if(name=="SystemParametersInfoForDpi"){
        // 只支援 SPI_GETICONTITLELOGFONT（0x1F）、SPI_GETNONCLIENTMETRICS（0x29）、SPI_GETICONMETRICS（0x2D）：取 96 DPI 值後依指定 DPI 縮放尺寸與字型高度。
        const auto action=u32(0),dpi=u32(4);if((action!=0x1f && action!=0x29 && action!=0x2d) || !dpi || !arg(2)){last_error_=87;return done(0);}
        if(!host("user32.dll","SystemParametersInfoW",{action,u32(1),arg(2),0}))return done(0);
        const auto scale=[&](std::uint64_t at){const auto v=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{at},32)));memory.store(GuestAddress{at},32,static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(v*static_cast<double>(dpi)/96))));};
        const auto font=[&](std::uint64_t at){scale(at);scale(at+4);};
        if(action==0x1f)font(arg(2));
        else if(action==0x2d){scale(arg(2)+4);scale(arg(2)+8);font(arg(2)+16);}
        else {for(unsigned i=1;i<=5;++i)scale(arg(2)+i*4);font(arg(2)+24);scale(arg(2)+116);scale(arg(2)+120);font(arg(2)+124);scale(arg(2)+216);scale(arg(2)+220);font(arg(2)+224);font(arg(2)+316);font(arg(2)+408);
            if(memory.load(GuestAddress{arg(2)},32)>=504)scale(arg(2)+500);}
        return done(1);
    }
    if(name=="SetGestureConfig" || name=="GetGestureConfig"){
        // GESTURECONFIG（dwID、dwWant、dwBlock，12 位元組）。預設：縮放與雙指點按、按住點按開啟，平移允許單指垂直、邊界回饋與慣性，旋轉封鎖。
        auto* window=window_of(arg(0));if(!window)return done(0);
        const auto defaults=[](std::uint32_t id)->std::pair<std::uint32_t,std::uint32_t>{switch(id){case 3:return {1,0};case 4:return {0x1bU,0x4U};case 5:return {0,1};case 6:case 7:return {1,0};default:return {0,0};}};
        if(name=="SetGestureConfig"){
            const auto count=u32(2);if(u32(4)!=12 || !arg(3) || !count || count>64){last_error_=87;return done(0);}
            for(std::uint32_t i=0;i<count;++i){const auto at=arg(3)+i*12;const auto id=static_cast<std::uint32_t>(memory.load(GuestAddress{at},32));
                const auto want=static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32)),block=static_cast<std::uint32_t>(memory.load(GuestAddress{at+8},32));
                if(id==0){for(std::uint32_t g=3;g<=7;++g)window->gesture_config[g]={want,block};}else if(id>=3 && id<=7)window->gesture_config[id]={want,block};else {last_error_=87;return done(0);}}
            return done(1);
        }
        // (hwnd, reserved, flags, count*, configs, cbSize)：依呼叫端填入的 dwID 取回；GID_ALL（0）取全部。
        if(u32(5)!=12 || !arg(3) || !arg(4)){last_error_=87;return done(0);}const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));
        std::vector<std::uint32_t> ids;for(std::uint32_t i=0;i<capacity;++i)ids.push_back(static_cast<std::uint32_t>(memory.load(GuestAddress{arg(4)+i*12},32)));
        if(std::all_of(ids.begin(),ids.end(),[](auto id){return id==0;})){ids.clear();for(std::uint32_t g=3;g<=7;++g)ids.push_back(g);if(capacity<ids.size()){memory.store(GuestAddress{arg(3)},32,ids.size());last_error_=122;return done(0);}}
        for(std::size_t i=0;i<ids.size();++i){const auto found=window->gesture_config.find(ids[i]);const auto value=found!=window->gesture_config.end()?found->second:defaults(ids[i]);
            memory.store(GuestAddress{arg(4)+i*12},32,ids[i]);memory.store(GuestAddress{arg(4)+i*12+4},32,value.first);memory.store(GuestAddress{arg(4)+i*12+8},32,value.second);}
        memory.store(GuestAddress{arg(3)},32,ids.size());return done(1);
    }
    if(name=="GetGestureInfo" || name=="GetGestureExtraArgs" || name=="CloseGestureInfoHandle"){
        // HGESTUREINFO 只隨 WM_GESTURE 產生；本輸入路徑不合成手勢訊息，因此不存在有效 handle。
        last_error_=6;return done(0);
    }
    if(name=="GetProgmanWindow")return done(gui_.progman_window);
    if(name=="GetTaskmanWindow")return done(gui_.taskman_window);
    if(name=="SetProgmanWindow" || name=="SetTaskmanWindow"){
        if(arg(0) && !gui_.windows.contains(arg(0))){last_error_=1400;return done(0);}(name=="SetProgmanWindow"?gui_.progman_window:gui_.taskman_window)=arg(0);return done(arg(0));}
    if(name=="WINNLSEnableIME"){const auto previous=gui_.legacy_ime_enabled;gui_.legacy_ime_enabled=u32(1)!=0;return done(previous?1U:0U);}
    if(name=="WINNLSGetEnableStatus")return done(gui_.legacy_ime_enabled?1U:0U);
    if(name=="PaintDesktop"){
        // 以 COLOR_DESKTOP 系統色填滿 DC 的裁切範圍（沒有桌布）。
        GuestScratch scratch(*this,memory,state);const auto rect=scratch.allocate(16);if(host("gdi32.dll","GetClipBox",{arg(0),rect})==0)return done(0);
        return done(host("user32.dll","FillRect",{arg(0),rect,host("user32.dll","GetSysColorBrush",{1})})?1U:0U);
    }
    throw HostCallFault("未知的 user32 補充服務");
}
}
