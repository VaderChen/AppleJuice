#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_window_state.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/windows_icon.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstdio>

namespace x64arm {
namespace {
constexpr std::array<std::u16string_view,23> stock_classes{u"button",u"static",u"edit",u"listbox",u"combobox",u"systreeview32",u"syslistview32",u"toolbarwindow32",u"msctls_statusbar32",u"rebarwindow32",u"scrollbar",u"msctls_progress32",u"comboboxex32",u"msctls_updown32",u"mdiclient",u"syslink",u"msctls_trackbar32",u"ime",u"systabcontrol32",u"tooltips_class32",u"sysheader32",u"sysdatetimepick32",u"sysmonthcal32"};
constexpr std::uint64_t stock_base=windows_address_layout::x64.callbacks+0x100;
constexpr std::uint64_t stock_base_x86=windows_address_layout::x86.callbacks+0x100;
void trace_gui_input(std::string_view filter,const char* stage,const GuiMessage& message,std::uint64_t thread,std::uint64_t ticks) {
    // X64ARM_TRACE_GUI_INPUT 只開按鍵紀錄，不經 X64ARM_TRACE_SERVICES（後者會關閉常駐快速路徑），
    // 供錄製人工操作後轉成自動化腳本；unix_ms 用來與量測工具的時間軸對齊。
    static const bool standalone=std::getenv("X64ARM_TRACE_GUI_INPUT")!=nullptr;
    if((!standalone && filter.find("GuiInput")==filter.npos) || message.message<0x100 || message.message>0x107)return;
    if(standalone && std::string_view(stage)!="enqueue")return;
    const auto unix_ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(stderr,"[gui-input] stage=%s unix_ms=%lld ticks=%llu thread=%llu hwnd=%llx message=%x key=%llx flags=%llx\n",stage,static_cast<long long>(unix_ms),
        static_cast<unsigned long long>(ticks),static_cast<unsigned long long>(thread),static_cast<unsigned long long>(message.window),
        message.message,static_cast<unsigned long long>(message.wparam),static_cast<unsigned long long>(message.lparam));
}
std::int32_t signed32(std::uint64_t value) { return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value)); }
constexpr std::uint64_t dc_base = 0x10000000;
}
void gui_write_message(GuestMemory& memory, std::uint64_t address, const GuiMessage& msg) {
    const bool x86=memory.execution_mode()==ExecutionMode::X86;const std::uint8_t bits=x86?32:64;
    memory.write(GuestAddress{address}, std::vector<std::uint8_t>(x86?28:48));
    memory.store(GuestAddress{address},bits,msg.window);memory.store(GuestAddress{address+(x86?4:8)},32,msg.message);
    memory.store(GuestAddress{address+(x86?8:16)},bits,msg.wparam);memory.store(GuestAddress{address+(x86?12:24)},bits,msg.lparam);
    memory.store(GuestAddress{address+(x86?16:32)},32,msg.queued_time);
    for(unsigned i=0;i<2;++i)memory.store(GuestAddress{address+(x86?20:36)+i*4},32,static_cast<std::uint32_t>(msg.queued_cursor[i]));
}
GuiMessage gui_read_message(const GuestMemory& memory, std::uint64_t address) {
    const bool x86=memory.execution_mode()==ExecutionMode::X86;const std::uint8_t bits=x86?32:64;
    memory.check(GuestAddress{address},x86?28:48,Access::Read);
    GuiMessage result{memory.load(GuestAddress{address},bits),memory.load(GuestAddress{address+(x86?8:16)},bits),memory.load(GuestAddress{address+(x86?12:24)},bits),static_cast<std::uint32_t>(memory.load(GuestAddress{address+(x86?4:8)},32))};
    result.queued_time=static_cast<std::uint32_t>(memory.load(GuestAddress{address+(x86?16:32)},32));
    for(unsigned i=0;i<2;++i)result.queued_cursor[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{address+(x86?20:36)+i*4},32)));
    return result;
}
void WindowsServices::enqueue_gui_message(GuiMessage message) {
    if(message.queue_priority==1 && !message.input_order)message.input_order=gui_.next_input_order++;
    if(!message.injected)message.queued_time=static_cast<std::uint32_t>(runtime_.clock_ticks/10000);
    if(message.has_point && message.screen_point)gui_.pointer_position=message.point;
    else if(message.has_point && message.message>=0x200 && message.message<=0x20e && message.message!=0x20a && message.message!=0x20e){
        const auto origin=gui_client_origin(gui_,message.window);
        for(unsigned i=0;i<2;++i)gui_.pointer_position[i]=static_cast<std::int32_t>(std::clamp<std::int64_t>(origin[i]+message.point[i],INT32_MIN,INT32_MAX));
    }else if(!message.injected && native_gui_)if(const auto position=native_gui_->cursor_position())gui_.pointer_position=*position;
    message.queued_cursor=gui_.pointer_position;
    trace_gui_input(trace_services_,"enqueue",message,threads_.current,runtime_.clock_ticks);
    // 滑鼠先經過可重入的 NCHITTEST／MOUSEACTIVATE；較晚的鍵盤已入列時，
    // 仍須將較早的滑鼠放回原順序，否則放開會被按鍵超越，拖曳尚未結束。
    const auto position=message.queue_priority==1?std::find_if(gui_.queue.begin(),gui_.queue.end(),[&](const auto& queued){return queued.queue_priority==1 && queued.input_order>message.input_order;}):gui_.queue.end();
    gui_.queue.insert(position,std::move(message));
}
void WindowsServices::remember_gui_message(const GuiMessage& message) {
    trace_gui_input(trace_services_,"peek",message,threads_.current,runtime_.clock_ticks);
    remember_pointer_message(message);
    if(message.ime && gui_.input_contexts.contains(message.ime_context))gui_.input_contexts.at(message.ime_context).data=*message.ime;
    auto& thread=threads_.items.at(threads_.current);thread.message_time=message.queued_time;
    if(message.input_extra)thread.message_extra=*message.input_extra;
    thread.message_position=static_cast<std::uint16_t>(message.queued_cursor[0])|(std::uint32_t{static_cast<std::uint16_t>(message.queued_cursor[1])}<<16);
}
std::uint64_t WindowsServices::stock_gui_proc(std::u16string_view name,bool wide) const {
    for(std::size_t i=0;i<stock_classes.size();++i)if(name.size()==stock_classes[i].size() && std::equal(name.begin(),name.end(),stock_classes[i].begin(),[](char16_t a,char16_t b){return (a>=u'A' && a<=u'Z'?a+32:a)==b;}))return callback_return_.value+0x100+(i*2+(wide?1:0))*16;return 0;
}
std::u16string_view WindowsServices::stock_gui_class(std::uint64_t proc){
    if(proc>=stock_base_x86 && proc<stock_base_x86+stock_classes.size()*32)proc=proc-stock_base_x86+stock_base;
    return proc>=stock_base && proc-stock_base<stock_classes.size()*32 && (proc-stock_base)%16==0?stock_classes[(proc-stock_base)/32]:std::u16string_view{};
}
GuiClass* WindowsServices::gui_class_record(std::u16string_view name,bool system){
    const auto equal=[](char16_t a,char16_t b){return (a>=u'A' && a<=u'Z'?a+32:a)==(b>=u'A' && b<=u'Z'?b+32:b);};
    for(auto& [key,record]:gui_.classes)if(key.size()==name.size() && std::equal(key.begin(),key.end(),name.begin(),equal))return &record;
    if(!system)return nullptr;
    const auto procedure=stock_gui_proc(name,true);if(!procedure)return nullptr;
    const std::u16string key(stock_gui_class(procedure));
    auto [found,inserted]=gui_.system_classes.try_emplace(key);
    if(inserted){found->second.procedure=procedure;found->second.atom=static_cast<std::uint16_t>(gui_.next_atom++);found->second.style=0x4000U;}
    return &found->second;
}
std::optional<std::uint64_t> WindowsServices::default_gui_control(GuestMemory& memory,const GuiMessage& message,bool){
    const auto found=gui_.windows.find(message.window);if(found==gui_.windows.end())return std::nullopt;
    const auto& window=found->second;const auto proc=window.procedure;
    if(window.mdi)return std::nullopt;
    if(proc && stock_gui_class(proc).empty() && proc!=resolve("user32.dll","DefWindowProcA").value && proc!=resolve("user32.dll","DefWindowProcW").value)return std::nullopt;
    if(message.message==0x10f){imm_default_result(message);return 0;}
    if(message.message==0x10d || message.message==0x10e)return 0;
    if(message.message==0xff){release_raw_input(message.lparam);return 0;}
    if(window.combo_owner && gui_control_class(window)==u"edit" && message.message==0x100 && (message.wparam==13 || message.wparam==27))return std::nullopt;
    if((window.calendar_owner || window.header_owner) && gui_control_class(window)==u"edit" && (message.message==8 || (message.message==0x100 && (message.wparam==13 || message.wparam==27))))return std::nullopt;
    if((gui_.clipboard_ole_object || gui_.clipboard_owner || !gui_.clipboard.empty()) && gui_control_class(window)==u"edit" &&
       ((message.message>=0x300 && message.message<=0x302) || (message.message==0x102 && (message.wparam==3 || message.wparam==22 || message.wparam==24))))return std::nullopt;
    if((gui_control_class(window)==u"rebarwindow32" || gui_control_class(window)==u"toolbarwindow32" || gui_control_class(window)==u"systabcontrol32") && message.message==0x4e)return std::nullopt;
    if(gui_control_class(window)==u"syslistview32" && (message.message==0x1030 || message.message==0x1051 || message.message==0x1005 || message.message==0x104b || message.message==0x102d || message.message==0x1073))return std::nullopt;
    if(gui_control_class(window)==u"button" && (message.message==0xf5 || message.message==0x202 || (message.message==0x101 && message.wparam==32)))return std::nullopt;
    if(gui_control_class(window)==u"systabcontrol32" && (message.message==0x1330 || message.message==0x201 || message.message==0x100))return std::nullopt;
    if(gui_control_class(window)==u"systreeview32" && (message.message==0x110b || message.message==0x10010 || message.message==0x1101 || message.message==0x1102 || message.message==0x110c || message.message==0x113e || message.message==0x1001a || message.message==0x1001b))return std::nullopt;
    if(gui_control_class(window)==u"sysdatetimepick32" || gui_control_class(window)==u"sysmonthcal32")return std::nullopt;
    if(gui_control_class(window)==u"tooltips_class32" || gui_control_class(window)==u"sysheader32" || (gui_control_class(window)==u"syslistview32" && message.message==0x4e))return std::nullopt;
    if(message.message==0x55 && message.lparam==4)return std::nullopt;
    if(gui_control_class(window)==u"msctls_statusbar32" && message.message==0x409)return std::nullopt;
    if(message.message==0x83 || message.message==0x81 || message.message==0x84 || message.message==0x86 || message.message==0x21 || message.message==6 || message.message==0xa1 || message.message==0xa3 || message.message==0x112 || message.message==0x10 || message.message==15 || message.message==0x47 || message.message==0x1f)return std::nullopt;
    return gui_control_message(memory,message,window.unicode);
}
std::optional<std::uint32_t> WindowsServices::invoke_stock_gui(GuestState& state,GuestMemory& memory){
    const auto cls=stock_gui_class(state.rip.value);const bool wide=((state.rip.value-stock_base)/16)%2!=0;const WindowsCall call(state,memory,4);
    const auto window=gui_.windows.find(call(0));
    if(window!=gui_.windows.end()){window->second.builtin=true;window->second.control_class=std::u16string(cls);}
    return invoke_gui(state,memory,{"user32.dll",wide?"DefWindowProcW":"DefWindowProcA",4});
}
void WindowsServices::enable_native_gui(const std::filesystem::path& snapshot) {
    if (!gui_.windows.empty()) throw HostCallFault("原生 GUI 必須在客體執行前設定");
    set_native_gui_backend(make_native_gui(snapshot));
}
void WindowsServices::set_native_gui_backend(std::shared_ptr<NativeGui> backend) {
    if (!gui_.windows.empty()) throw HostCallFault("原生 GUI 必須在客體執行前設定");
    native_gui_ = std::move(backend);
    if(native_gui_)native_gui_->raw_input_policy(!raw_legacy_enabled(true),!raw_legacy_enabled(false),false);
}
void WindowsServices::invalidate_window(std::uint64_t handle) {
    auto& window = gui_.windows.at(handle); window.invalid = true;
    if (window.visible && std::none_of(gui_.queue.begin(), gui_.queue.end(), [&](const auto& msg) { return msg.window == handle && msg.message == 15; }))
        { GuiMessage msg{handle,0,0,15};msg.queue_priority=2;enqueue_gui_message(std::move(msg)); }
}
void WindowsServices::erase_window(std::uint64_t handle,bool destroy_menus) {
    gui_.status_icons.erase_owner(handle);gui_.attention.erase(handle);
    if(native_gui_){(void)native_gui_->status_icons(gui_.status_icons.values());(void)native_gui_->attention(handle,false,false);}
    gui_.native_attention_windows.erase(handle);
    cancel_power_notifications(handle);
    erase_mdi_window(handle);
    gui_.window_themes.erase(handle);for(auto& [theme,data]:gui_.themes){(void)theme;if(data.window==handle)data.window=0;}
    release_raw_input(0,handle);
    std::erase_if(gui_.hotkeys,[&](const auto& entry){return entry.window==handle;});
    std::vector<std::uint64_t> canceled_sends;
    for(const auto& [token,request]:gui_.sent_messages)if(request.message.window==handle && !request.active && !request.done)canceled_sends.push_back(token);
    for(const auto token:canceled_sends)cancel_gui_send(token,1400);
    std::vector<std::uint64_t> children;for(const auto& [h,w]:gui_.windows)if(h!=handle && (w.parent==handle || w.owner==handle))children.push_back(h);
    for(const auto child:children)erase_window(child);
    if(const auto w=gui_.windows.find(handle);w!=gui_.windows.end() && w->second.resource_icon)gui_.icons.erase(w->second.resource_icon);
    unpublish_gui_window(handle);
    rasters_.erase(dc_base+handle);
    opengl_.swap_intervals.erase(handle);opengl_.pixel_formats.erase(dc_base+handle);
    if (native_gui_) native_gui_->destroy(handle);
    if(destroy_menus){const auto window=gui_.windows.find(handle);if(window!=gui_.windows.end())destroy_gui_menu(window->second.menu);}
    if(const auto menu=gui_.system_menus.find(handle);menu!=gui_.system_menus.end())destroy_gui_menu(menu->second);
    if(const auto window=gui_.windows.find(handle);window!=gui_.windows.end())
        for(const auto& [key,property]:window->second.properties)if(property.owns_atom)release_gui_atom(key.second,property.atom_job);
    if(gui_.windows.contains(handle) && gui_.windows.at(handle).owned_font)(gui_.font_natives.erase(gui_.windows.at(handle).font),gui_.fonts.erase(gui_.windows.at(handle).font));
    gui_.input_associations.erase(handle);std::erase_if(gui_.input_borrows,[&](const auto& v){return v.first.first==handle;});
    std::erase_if(gui_.input_windows,[&](const auto& entry){return entry.second==handle;});
    std::erase_if(gui_.input_window_targets,[&](const auto& entry){return entry.second==handle;});
    std::erase_if(gui_.mouse_tracking,[&](const auto& entry){return entry.second.window==handle;});
    gui_.windows.erase(handle);std::erase(gui_.z_order,handle);
    std::vector<std::uint64_t> calendar_parents;
    for(auto& [other,window]:gui_.windows){
        if(window.last_active_popup==handle)window.last_active_popup=0;
        if(window.tab && window.tab->tooltips==handle)window.tab->tooltips=0;
        if(window.calendar){auto& date=*window.calendar;bool changed=false;
            if(date.edit_window==handle){date.edit_window=0;date.editing=date.edit_request=date.commit_request=date.cancel_request=false;++date.edit_generation;changed=true;}
            if(date.updown_window==handle){date.updown_window=0;changed=true;}
            if(date.month_window==handle){date.month_window=0;date.open=date.open_request=date.close_request=false;date.pressed=-1;changed=true;}
            if(changed && !window.destroying)calendar_parents.push_back(other);
        }
        if(window.tooltip){auto& tip=*window.tooltip;
            const auto removed=[&](const auto& tool){return tool.owner==handle || ((tool.flags&1U) && tool.id==handle);};
            if(std::any_of(tip.tools.begin(),tip.tools.end(),[&](const auto& tool){return removed(tool) && (tool.identity==tip.shown || tool.identity==tip.current);})){hide_gui_tooltip(other);tip.current=0;tip.tracking=false;}
            if(std::erase_if(tip.tools,removed))++tip.revision;
        }
    }
    gui_.common_dialogs.erase(handle);
    gui_.shell_browsers.erase(handle);
    gui_.shell_open_with.erase(handle);
    if(gui_.appbars.erase(handle))notify_appbars(1);
    gui_.drag_lists.erase(handle);gui_.drag_insertions.erase(handle);
    if(gui_.clipboard_owner==handle){gui_.clipboard_owner=0;
        std::erase_if(gui_.clipboard,[](const auto& entry){return !entry.second;});
        std::erase_if(gui_.clipboard_order,[&](auto format){return !gui_.clipboard.contains(format);});
        std::erase_if(gui_.clipboard_types,[&](const auto& entry){return !gui_.clipboard.contains(entry.first);});
    }
    gui_.clipboard_listeners.erase(handle);for(auto& [id,station]:gui_.station_namespaces){(void)id;station.clipboard_listeners.erase(handle);}
    gui_.saved_contexts.erase(dc_base+handle);gui_.contexts.erase(dc_base+handle);gui_.dc_objects.erase(dc_base+handle);std::erase_if(gui_.scroll_info,[&](const auto& entry){return entry.first.first==handle;});gui_.system_menus.erase(handle);
    if(gui_.focus==handle)gui_.focus=0;if(gui_.active==handle)gui_.active=0;if(gui_.capture==handle)gui_.capture=0;
    std::erase_if(gui_.timers,[&](const auto& entry){return entry.first.first==handle;});
    std::erase_if(gui_.queue, [&](const auto& msg) { return msg.window == handle; }); ++gui_.destroyed;
    for(const auto parent:calendar_parents)present_gui(parent);
}
void WindowsServices::notify_clipboard(){
    for(const auto hwnd:gui_.clipboard_listeners)if(gui_.windows.contains(hwnd))enqueue_gui_message({hwnd,0,0,0x31d});
    gui_.clipboard_dirty=false;
}
void WindowsServices::refresh_clipboard(bool local){
    bool changed=false;
    if(native_gui_ && interactive_station())if(const auto sequence=native_gui_->clipboard_sequence()){
        changed=gui_.clipboard_native_sequence && *gui_.clipboard_native_sequence!=*sequence;
        if(!local && (changed || !gui_.clipboard_native_sequence))gui_.clipboard_native_text=native_gui_->clipboard_text();
        gui_.clipboard_native_sequence=sequence;
    }
    if(local){++gui_.clipboard_sequence;gui_.clipboard_dirty=true;}
    else if(changed){++gui_.clipboard_sequence;gui_.clipboard_stale=true;gui_.clipboard_owner=0;notify_clipboard();}
}
void WindowsServices::pump_mouse_tracking(){
    if(gui_.mouse_tracking.empty())return;
    if(native_gui_)if(const auto point=native_gui_->cursor_position())gui_.pointer_position=*point;
    const auto point=gui_.pointer_position;const auto hit=gui_mouse_hit(gui_,point[0],point[1],gui_.input_desktop);
    for(auto it=gui_.mouse_tracking.begin();it!=gui_.mouse_tracking.end();){auto& track=it->second;const bool nonclient=(track.flags&16)!=0;
        const auto found=gui_.windows.find(track.window);if(found==gui_.windows.end()){it=gui_.mouse_tracking.erase(it);continue;}
        const auto notify=[&](unsigned message,std::uint64_t wp,std::uint64_t lp){GuiMessage event{track.window,wp,lp,message};event.thread=it->first;event.queue_priority=1;event.modifiers=((gui_.async_keyboard[16]&128)?1U:0U)|((gui_.async_keyboard[17]&128)?2U:0U)|((gui_.async_keyboard[18]&128)?4U:0U);enqueue_gui_message(std::move(event));};
        if(hit.first!=track.window || ((hit.second!=1)!=nonclient)){
            if(track.flags&2)notify(nonclient?0x2a2:0x2a3,0,0);it=gui_.mouse_tracking.erase(it);continue;
        }
        if(track.flags&1){const auto dx=static_cast<std::int64_t>(point[0])-track.anchor[0],dy=static_cast<std::int64_t>(point[1])-track.anchor[1];
            const auto half_x=std::max<std::int64_t>(1,gui_.mouse_hover_width/2),half_y=std::max<std::int64_t>(1,gui_.mouse_hover_height/2);
            if(dx< -half_x || dx>=half_x || dy< -half_y || dy>=half_y){track.anchor=point;track.deadline=runtime_.clock_ticks+std::uint64_t{track.hover_time}*10000;}
            if(runtime_.clock_ticks>=track.deadline){const auto origin=nonclient?std::array<std::int64_t,2>{}:gui_client_origin(gui_,track.window);const auto position=static_cast<std::uint16_t>(point[0]-origin[0])|(std::uint64_t{static_cast<std::uint16_t>(point[1]-origin[1])}<<16);
                const auto keys=((gui_.async_keyboard[1]&128)?1U:0U)|((gui_.async_keyboard[2]&128)?2U:0U)|((gui_.async_keyboard[16]&128)?4U:0U)|((gui_.async_keyboard[17]&128)?8U:0U)|((gui_.async_keyboard[4]&128)?16U:0U)|((gui_.async_keyboard[5]&128)?32U:0U)|((gui_.async_keyboard[6]&128)?64U:0U);
                notify(nonclient?0x2a0:0x2a1,nonclient?hit.second:keys,position);track.flags&=~1U;}
        }
        if(!(track.flags&3))it=gui_.mouse_tracking.erase(it);else ++it;
    }
}
void WindowsServices::pump_gui(GuestMemory& memory, bool wait,std::uint64_t wait_ticks) {
    if(const auto found=gui_.icons.find(gui_.cursor);found!=gui_.icons.end() && !found->second.animation_sequence.empty()){
        const auto& icon=found->second;const auto step=icon_animation_step(icon,runtime_.clock_ticks>=gui_.cursor_animation_start?runtime_.clock_ticks-gui_.cursor_animation_start:0);
        if(step!=gui_.cursor_animation_step && (!native_gui_ || !interactive_station() || native_gui_->set_cursor(&icon.animation_frames.at(icon.animation_sequence[step]),gui_.cursor_count>=0)))gui_.cursor_animation_step=step;
    }
    poll_appbars();
    poll_power_notifications(memory);poll_dinput();poll_shell_notifications(memory);poll_desktop_notifications();
    if(gui_.cursor_clip){auto point=gui_.pointer_position;if(native_gui_)if(const auto current=native_gui_->cursor_position())point=*current;const auto& rect=*gui_.cursor_clip;
        const std::array<std::int32_t,2> constrained{std::clamp(point[0],rect[0],rect[2]-1),std::clamp(point[1],rect[1],rect[3]-1)};
        if(native_gui_ && point!=constrained)native_gui_->set_cursor_position(constrained[0],constrained[1]);gui_.pointer_position=constrained;}
    // 明確的 Clipboard API 仍立即同步；事件輪詢僅為已登錄的監聽視窗查詢宿主。
    if(!gui_.clipboard_listeners.empty() && runtime_.clock_ticks>=gui_.clipboard_poll_deadline){
        gui_.clipboard_poll_deadline=runtime_.clock_ticks+160000;refresh_clipboard();
    }
    poll_remote_frames();
    for(auto& [key,surface]:rasters_){(void)key;surface->flush();}
    poll_socket_lookups(memory);pump_gui_network();
    // 自繪控制項先收到 WM_PAINT；stock procedure 同步回呼父視窗並回收 scratch。
    for(const auto& [handle,w]:gui_.windows)if(w.builtin && ((gui_control_class(w)==u"button" && (w.style&15)==11) || (gui_control_class(w)==u"static" && (w.style&31)==13) || (gui_control_class(w)==u"systabcontrol32" && (w.style&0x2000U))) && w.invalid && w.redraw && w.visible && gui_.windows.contains(w.parent) && gui_.windows.at(w.parent).visible){
        if(std::none_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& msg){return msg.window==handle && msg.message==15;})){GuiMessage msg{handle,0,0,15};msg.queue_priority=2;enqueue_gui_message(std::move(msg));}
    }
    for(auto& [key,timer]:gui_.timers)if(timer.deadline<=runtime_.clock_ticks){if(std::none_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& m){return m.window==key.first && m.message==0x113 && m.wparam==key.second;})){GuiMessage msg{key.first,key.second,timer.procedure,0x113};msg.thread=timer.thread;msg.queue_priority=3;enqueue_gui_message(std::move(msg));}timer.deadline=gui_.timer_deadline(runtime_.clock_ticks+timer.interval,timer.coalescing_tolerance,key);}
    for(auto& [handle,w]:gui_.windows)if(w.header && w.header->filter_editing && w.header->filter_deadline && w.header->filter_deadline<=runtime_.clock_ticks){auto& control=*w.header;
        if(std::none_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& msg){return msg.window==handle && msg.message==0x10019 && msg.wparam==control.filter_item && msg.lparam==control.filter_generation;})){GuiMessage msg{handle,control.filter_item,control.filter_generation,0x10019};msg.thread=w.thread;msg.queue_priority=3;enqueue_gui_message(std::move(msg));}}
    pump_mouse_tracking();
    if (!native_gui_ || (!wait && wait_ticks==gui_native_poll_deferred)) return;
    // 宿主事件等待不得越過客體訊息計時器與排程 deadline；100 ns 為 Windows clock tick。
    auto bounded=wait?std::min(wait_ticks,UINT64_C(500000)):0;
    for(const auto& [key,timer]:gui_.timers){(void)key;bounded=std::min(bounded,timer.deadline>runtime_.clock_ticks?timer.deadline-runtime_.clock_ticks:0);}
    for(const auto& [handle,w]:gui_.windows){(void)handle;if(w.header && w.header->filter_editing && w.header->filter_deadline)bounded=std::min(bounded,w.header->filter_deadline>runtime_.clock_ticks?w.header->filter_deadline-runtime_.clock_ticks:0);}
    bounded=std::min(bounded,gui_.attention.deadline()>runtime_.clock_ticks?gui_.attention.deadline()-runtime_.clock_ticks:0);
    const auto before=std::chrono::steady_clock::now();auto events=native_gui_->poll_for(bounded*100);
    if(wait){const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-before).count();if(elapsed>0){if(!runtime_.host_clock)runtime_.clock_ticks+=static_cast<std::uint64_t>(elapsed)/100;retired(memory,0);}}
    for (auto msg : events) {
        if(msg.status_icon){if(const auto translated=gui_.status_icons.event(*msg.status_icon);translated && gui_.windows.contains(translated->owner))enqueue_gui_message({translated->owner,translated->wparam,translated->lparam,translated->message});continue;}
        msg.queue_priority=msg.message==15?2:1;
        if(msg.message==0x20a || msg.message==0x20e)dinput_mouse_wheel(static_cast<std::int16_t>(msg.wparam>>16),msg.message==0x20e);
        const bool physical_key=msg.message>=0x100 && msg.message<=0x105;
        const bool physical_mouse=msg.message>=0x200 && msg.message<=0x20e;
        if(physical_key || physical_mouse || msg.message==0x10004){
            gui_.last_input_ticks=runtime_.clock_ticks;
            const auto key=[&](unsigned code,bool down){auto& value=gui_.async_keyboard[code];if(down && !(value&0x80))value|=1;value=static_cast<std::uint8_t>((value&1U)|(down?0x80U:0));};
            if(!physical_key){key(16,(msg.modifiers&1) || gui_.injected_keys[16]==1);key(17,(msg.modifiers&2) || gui_.injected_keys[17]==1);key(18,(msg.modifiers&4) || gui_.injected_keys[18]==1);}
            if(msg.message==0x100 || msg.message==0x101 || msg.message==0x104 || msg.message==0x105)observe_gui_key(msg,false);
            if(physical_mouse || msg.message==0x10004){key(1,msg.wparam&1);key(2,msg.wparam&2);key(4,msg.wparam&16);key(5,msg.wparam&32);key(6,msg.wparam&64);}
        }
        // 實體按鍵的 Raw Input 來自宿主鍵盤狀態輪詢；注入（自動化）按鍵沒有實體狀態，比照 SendInput 在此產生。
        if(physical_key && msg.injected)raw_input_keyboard(msg,static_cast<std::uint32_t>(runtime_.clock_ticks/10000),0);
        // 注入的滾輪同樣產生 Raw Input（RI_MOUSE_WHEEL／RI_MOUSE_HWHEEL，usButtonData 為滾動量）。
        if((msg.message==0x20a || msg.message==0x20e) && msg.injected)
            raw_input_mouse(0,0,0,msg.message==0x20a?0x400U:0x800U,static_cast<std::uint16_t>(msg.wparam>>16),static_cast<std::uint32_t>(runtime_.clock_ticks/10000),0);
        // 注入的滑鼠移動：以 Raw Input 相對位移送出（依 Raw Input 自繪游標的程式只看相對位移）。
        // 起點取視窗中央（游標置中的程式常見初始位置），之後以上一次注入位置為基準。
        if(msg.message==0x200 && msg.injected){
            const auto x=static_cast<std::int32_t>(static_cast<std::int16_t>(msg.lparam&0xffffU)),y=static_cast<std::int32_t>(static_cast<std::int16_t>((msg.lparam>>16)&0xffffU));
            if(!automation_mouse_){const auto found=gui_.windows.find(msg.window);
                automation_mouse_=found!=gui_.windows.end()?std::pair{static_cast<std::int32_t>(found->second.width/2),static_cast<std::int32_t>(found->second.height/2)}:std::pair{x,y};}
            raw_input_mouse(0,x-automation_mouse_->first,y-automation_mouse_->second,0,0,static_cast<std::uint32_t>(runtime_.clock_ticks/10000),0);
            automation_mouse_=std::pair{x,y};
        }
        // 注入的滑鼠按鍵：RI_MOUSE_{LEFT,RIGHT,MIDDLE}_BUTTON_{DOWN,UP}。
        if(msg.message>=0x201 && msg.message<=0x209 && msg.injected){
            static constexpr std::uint16_t flags[]={0x1,0x2,0x1,0x4,0x8,0x4,0x10,0x20,0x10}; // DOWN、UP、DBLCLK（視同 DOWN）
            raw_input_mouse(0,0,0,flags[msg.message-0x201],0,static_cast<std::uint32_t>(runtime_.clock_ticks/10000),0);
        }
        if((msg.message==0x100 || msg.message==0x101 || msg.message==0x104 || msg.message==0x105) && match_gui_hotkey(msg))continue;
        if((physical_key && !raw_legacy_enabled(true)) || (physical_mouse && !raw_legacy_enabled(false)))continue;
        if(msg.message==0x10004){ // 原生 tracking loop 消耗的放開事件只同步狀態，不重送客體輸入。
            gui_.keyboard[1]=(msg.wparam&1)?0x80:0;gui_.keyboard[2]=(msg.wparam&2)?0x80:0;gui_.keyboard[4]=(msg.wparam&16)?0x80:0;
            gui_.keyboard[16]=(msg.modifiers&1)?0x80:0;gui_.keyboard[17]=(msg.modifiers&2)?0x80:0;gui_.keyboard[18]=(msg.modifiers&4)?0x80:0;
            continue;
        }
        const auto found = gui_.windows.find(msg.window); if (found == gui_.windows.end()) continue;
        auto& w=found->second;if(w.desktop_object!=gui_.input_desktop)continue;
        if(msg.message==0x1000d){
            if(msg.wparam && gui_.active!=msg.window && !(w.ex_style&0x08000000U))queue_gui_mouse(msg,true);
            continue;
        }
        const bool input=msg.message==0x10 || msg.message==0x10010 || msg.message==0x10011 || msg.message==0x10013 || msg.message==0x10014 || (msg.message>=0x100 && msg.message<=0x10f) || (msg.message>=0x200 && msg.message<=0x20e) || (msg.message>=0x300 && msg.message<=0x304) || (msg.message>=0x10001 && msg.message<=0x1000b);
        if((input || msg.message==0x10013) && !gui_window_input_enabled(gui_,msg.window))continue;
        if(msg.message==0x10013){
            if(gui_.focus!=msg.window){const auto previous=gui_.focus;gui_.focus=msg.window;
                const auto send_focus=[&](std::uint64_t handle,std::uint32_t code,std::uint64_t other){const auto target=gui_.windows.find(handle);if(target==gui_.windows.end())return;
                    if(gui_.sent_messages.size()>=8192)throw HostCallFault("焦點訊息佇列超過上限");GuiSentMessage request;request.message={handle,other,0,code};request.receiver=target->second.thread;request.wide=target->second.unicode;gui_.sent_messages.emplace(gui_.next_object++,std::move(request));};
                if(previous)send_focus(previous,8,msg.window);send_focus(msg.window,7,previous);
            }continue;
        }
        if(msg.message==0x10014){if(gui_control_class(w)==u"edit"){
            const auto length=static_cast<std::uint32_t>(w.title.size());w.edit_start=std::min(static_cast<std::uint32_t>(msg.wparam),length);w.edit_end=std::min(static_cast<std::uint32_t>(msg.lparam),length);
        }continue;}
        if(imm_native_message(msg))continue;
        if(gui_native_control_message(memory,msg))continue;
        if(physical_mouse){queue_gui_mouse(std::move(msg));continue;}
        if(physical_key){
            if(gui_.focus && gui_.windows.contains(gui_.focus))msg.window=gui_.focus;
            else if(gui_.active && gui_.windows.contains(gui_.active)){msg.window=gui_.active;if(msg.message==0x100)msg.message=0x104;else if(msg.message==0x101)msg.message=0x105;}
            if(!gui_window_input_enabled(gui_,msg.window))continue;
        }

        if(msg.message==0x10011 && gui_control_class(w)==u"toolbarwindow32"){
            if(msg.wparam>=w.toolbar_buttons.size())continue;
            const auto index=static_cast<std::size_t>(msg.wparam);auto& button=w.toolbar_buttons[index];
            if(static_cast<std::uint64_t>(button.command)!=msg.lparam || !(button.state&4U) || (button.state&8U) || (button.style&1U))continue;
            if(button.style&2U){
                if(button.style&4U){
                    auto first=index,last=index+1;while(first && (w.toolbar_buttons[first-1].style&4U) && !(w.toolbar_buttons[first-1].style&1U))--first;
                    while(last<w.toolbar_buttons.size() && (w.toolbar_buttons[last].style&4U) && !(w.toolbar_buttons[last].style&1U))++last;
                    for(auto i=first;i<last;++i)w.toolbar_buttons[i].state=static_cast<std::uint8_t>((w.toolbar_buttons[i].state&~1U)|(i==index?1U:0U));
                }else button.state^=1U;
            }
            const auto parent=w.parent,toolbar=msg.window;const auto command=static_cast<std::uint32_t>(button.command)&65535U;
            if(native_gui_)present_native_window(toolbar,w);enqueue_gui_message({parent,command,toolbar,0x111});continue;
        }
        if(msg.message==0x10003 && gui_control_class(w)==u"button"){
            // 內部對話框控制項沒有客體 WNDPROC；原生 action 已完成按下／放開，
            // 直接送共用的 BN_CLICKED。客體控制項仍走 BM_CLICK 以保留 subclass。
            if(w.builtin && !w.procedure){if(const auto notify=complete_gui_button_click(msg.window))enqueue_gui_message(*notify);}
            else {msg.message=0xf5;enqueue_gui_message(msg);}
            continue;
        }
        if(msg.message>=0x10001 && msg.message<=0x10003){
            const auto handle=msg.window;unsigned notification=0;
            if(msg.message==0x10001){const auto previous=w.title;w.title=msg.text.substr(0,w.text_limit);notification=gui_control_class(w)==u"combobox"?5:0x300;
                if(gui_control_class(w)==u"edit"){const auto length=static_cast<std::uint32_t>(w.title.size());
                    w.edit_start=msg.indices.size()==2?std::min(msg.indices[0],length):length;w.edit_end=msg.indices.size()==2?std::min(msg.indices[1],length):length;
                    if(w.title!=previous){w.edit_undo=previous;w.edit_modified=true;enqueue_gui_message({w.parent,UINT64_C(0x4000000)|(w.id&65535U),handle,0x111});}}
            }
            else if(msg.message==0x10002){
                if(gui_control_class(w)==u"systreeview32"){
                    if(msg.wparam>=w.items.size())continue;
                    const auto action=msg.lparam==2?2U:1U;
                    msg.lparam=w.items[static_cast<std::size_t>(msg.wparam)].handle;msg.wparam=action;msg.message=0x10010;
                    // 共用同步通知完成後才投影結果；先回畫舊項目會讓每次選取跳動。
                    enqueue_gui_message(std::move(msg));continue;
                }
                const auto previous=w.selection;w.selection=static_cast<std::int32_t>(msg.wparam);
                const bool multiple=gui_control_class(w)==u"listbox" && (w.style&(0x8U|0x800U));
                if(multiple){
                    for(auto& item:w.items)item.state&=~1U;
                    for(const auto index:msg.indices)if(index<w.items.size())w.items[index].state|=1U;
                    if(w.selection<0 || static_cast<std::size_t>(w.selection)>=w.items.size())w.selection=previous;
                } else if(w.selection<0 || static_cast<std::size_t>(w.selection)>=w.items.size())continue;
                if(gui_control_class(w)==u"combobox")w.title=w.items[static_cast<std::size_t>(w.selection)].text;notification=1;
            }
            present_gui(handle);enqueue_gui_message({w.parent,(static_cast<std::uint64_t>(notification)<<16)|(w.id&65535),handle,0x111});continue;
        }
        if(msg.message==0x1000c){
            const auto next=msg.wparam==1?2U:msg.wparam==2?3U:1U;
            const auto old_state=w.show_command;const auto old_width=w.width,old_height=w.height;const auto old_x=w.x,old_y=w.y;
            if(old_state==1 && next!=1)w.normal_rect=std::array<std::int32_t,4>{w.x,w.y,static_cast<std::int32_t>(w.width),static_cast<std::int32_t>(w.height)};
            if(next==2)w.restore_maximized=old_state==3;
            w.show_command=next;w.style=(w.style&~0x21000000U)|(next==2?0x20000000U:next==3?0x01000000U:0U);
            if(next!=2){w.width=static_cast<std::uint32_t>(msg.lparam&65535);w.height=static_cast<std::uint32_t>((msg.lparam>>16)&65535);if(msg.has_point){w.x=msg.point[0];w.y=msg.point[1];}if(next==1)w.normal_rect=std::array<std::int32_t,4>{w.x,w.y,static_cast<std::int32_t>(w.width),static_cast<std::int32_t>(w.height)};}
            if(old_x!=w.x || old_y!=w.y)enqueue_gui_message({msg.window,0,gui_move_lparam(w),3});
            if(old_state!=next || old_width!=w.width || old_height!=w.height)enqueue_gui_message({msg.window,msg.wparam,next==2?0:msg.lparam,5});
            if(next!=2)invalidate_window(msg.window);continue;
        }
        if (msg.message == 5 && msg.wparam!=1) {
            found->second.width = static_cast<std::uint32_t>(msg.lparam & 65535); found->second.height = static_cast<std::uint32_t>((msg.lparam >> 16) & 65535);
            invalidate_window(msg.window);
        }
        if (gui_.queue.size() >= 10000) throw HostCallFault("GUI 訊息佇列超過上限");
        enqueue_gui_message(msg);
    }
    pump_mouse_tracking();
}
void WindowsServices::destroy_window(GuestState& state, GuestMemory& memory, std::uint64_t handle, bool default_proc) {
    auto& window = gui_.windows.at(handle);
    if (window.destroying) { complete_call(state,memory,0); return; }
    window.destroying = true;
    std::vector<GuestCallback> callbacks;
    if(window.mdi_child)if(const auto parent=gui_.windows.find(window.parent);parent!=gui_.windows.end() && parent->second.mdi && !parent->second.destroying && parent->second.mdi->active==handle)
        callbacks=activate_mdi(window.parent,next_mdi_child(window.parent,handle,false),memory);
    if(gui_.clipboard_owner==handle && !gui_.clipboard_ole_object && std::any_of(gui_.clipboard.begin(),gui_.clipboard.end(),[](const auto& entry){return !entry.second;}))
        callbacks.push_back({resolve("user32.dll","SendMessageW").value,{handle,0x306,0,0}});
    const bool mdi_frame=std::any_of(gui_.windows.begin(),gui_.windows.end(),[&](const auto& item){return item.second.parent==handle && item.second.mdi.has_value();});
    if(window.mdi || mdi_frame)for(const auto& [child,model]:gui_.windows)if(model.parent==handle)callbacks.push_back({resolve("user32.dll","DestroyWindow").value,{child}});
    if(window.builtin && !window.mdi && !mdi_frame && (!window.procedure || !stock_gui_class(window.procedure).empty())){auto release=revoke_gui_drop_targets(memory,handle);callbacks.insert(callbacks.end(),release.begin(),release.end());erase_window(handle);start_callbacks(state,memory,std::move(callbacks),Continuation::Return,default_proc?0:1);return;}
    const auto procedure=window.procedure;
    callbacks.push_back({procedure,{handle,2,0,0}});callbacks.push_back({procedure,{handle,0x82,0,0}});
    start_callbacks(state, memory, std::move(callbacks), default_proc ? Continuation::GuiDestroyDefault : Continuation::GuiDestroy, handle);
}
std::optional<std::uint32_t> WindowsServices::resume_gui(GuestState& state, GuestMemory& memory, const CallbackFrame& frame, std::uint64_t value) {
    const auto handle = frame.value;
    if (!gui_.windows.contains(handle)) { complete_call(state,memory,0); return std::nullopt; }
    if(frame.continuation==Continuation::GuiModalEnd){const auto result=gui_.windows.at(handle).dialog_result;auto callbacks=revoke_gui_drop_targets(memory,handle);erase_window(handle);start_callbacks(state,memory,std::move(callbacks),Continuation::Return,result);return std::nullopt;}
    if(frame.continuation==Continuation::GuiModalInit || frame.continuation==Continuation::GuiModalDispatch){
        if(frame.continuation==Continuation::GuiModalInit){auto& w=gui_.windows.at(handle);w.visible=true;if(native_gui_)show_native_window(handle,true);if(gui_.windows.contains(w.owner)){
            w.modal_owner_was_enabled=gui_.shell_open_with.contains(handle)?gui_.shell_open_with.at(handle).owner_enabled:gui_.print_dialogs.contains(handle)?gui_.print_dialogs.at(handle).owner_enabled:gui_.common_dialogs.contains(handle)?gui_.common_dialogs.at(handle).owner_enabled:gui_.windows.at(w.owner).enabled;
            gui_set_window_enabled(gui_.windows.at(w.owner),false);present_gui(w.owner);}}
        return advance_gui_modal(state,memory,handle);
    }
    if(frame.continuation==Continuation::GuiModalSystemFilter || frame.continuation==Continuation::GuiModalFilter) {
        const auto buffer=gui_.windows.at(handle).modal_message_buffer;
        if(frame.continuation==Continuation::GuiModalSystemFilter && !value &&
           start_gui_hooks(state,memory,-1,0,0,buffer,Continuation::GuiModalFilter,handle))return std::nullopt;
        if(!value) {
            auto message=gui_.windows.at(handle).modal_message;
            const auto edited=gui_read_message(memory,buffer);message.window=edited.window;message.message=edited.message;
            message.wparam=edited.wparam;message.lparam=edited.lparam;
            if(dispatch_gui_modal_message(state,memory,handle,message))return std::nullopt;
        }
        return advance_gui_modal(state,memory,handle);
    }
    if (frame.continuation == Continuation::GuiNcCreate && value) {
        const auto& window = gui_.windows.at(handle);
        start_callbacks(state, memory, {{window.procedure, {handle, 1, 0, window.create_data}}}, Continuation::GuiCreate, handle); return std::nullopt;
    }
    std::uint64_t result = handle;
    if (frame.continuation == Continuation::GuiDestroy || frame.continuation == Continuation::GuiDestroyDefault) {
        result = frame.continuation == Continuation::GuiDestroy ? 1 : 0;auto callbacks=revoke_gui_drop_targets(memory,handle);erase_window(handle);start_callbacks(state,memory,std::move(callbacks),Continuation::Return,result);return std::nullopt;
    } else if ((frame.continuation == Continuation::GuiNcCreate && !value) || (frame.continuation == Continuation::GuiCreate && signed32(value) == -1)) {
        auto callbacks=revoke_gui_drop_targets(memory,handle);erase_window(handle);start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;
    } else if (gui_.windows.at(handle).style & 0x10000000U) {
        gui_.windows.at(handle).visible = true;
        if (native_gui_) show_native_window(handle, true);
        invalidate_window(handle);
    }
    if(frame.continuation==Continuation::GuiCreate){auto& window=gui_.windows.at(handle);const auto proc=window.procedure;
        if(window.initial_size_pending && !window.visible){complete_call(state,memory,result);return std::nullopt;}
        window.initial_size_pending=false;
        start_callbacks(state,memory,{{proc,{handle,5,0,(window.width&65535U)|((window.height&65535U)<<16)}},{proc,{handle,3,0,gui_move_lparam(window)}}},Continuation::Return,result);return std::nullopt;}
    complete_call(state,memory,result); return std::nullopt;
}
bool WindowsServices::dispatch_gui_modal_message(GuestState& state,GuestMemory& memory,std::uint64_t handle,const GuiMessage& msg) {
    const auto found=gui_.windows.find(handle);if(found==gui_.windows.end())return false;
    auto& dialog=found->second;
    const auto target=gui_.windows.find(msg.window);if(target==gui_.windows.end())return false;
    if(shell_dialog_message(state,memory,msg))return true;
    if(common_dialog_message(memory,msg))return false;
    if(msg.message>=0x100 && msg.message<=0x109){
        auto child=msg.window;for(std::size_t depth=0;child && depth<=gui_.windows.size();++depth){
            if(child==handle){
                if(!dialog.modal_message_buffer)dialog.modal_message_buffer=runtime_allocate(memory,{},48);
                gui_write_message(memory,dialog.modal_message_buffer,msg);
                start_callbacks(state,memory,{{resolve("user32.dll",target->second.unicode?"IsDialogMessageW":"IsDialogMessageA").value,{handle,dialog.modal_message_buffer}}},Continuation::GuiModalDispatch,handle);return true;
            }
            const auto ancestor=gui_.windows.find(child);if(ancestor==gui_.windows.end())break;child=ancestor->second.parent;
        }
    }
    if(msg.window==handle && !dialog.procedure && !dialog.dialog_proc && (msg.message==0x111 || msg.message==0x10)){dialog.ended=true;dialog.dialog_result=msg.message==0x111?(msg.wparam&65535):gui_child(handle,2)?2:1;return false;}
    if((msg.message>=0x200 && msg.message<=0x208) || msg.message==0xa0 || msg.message==0x10012){
        // 模態對話框也經共用 SendMessage 路徑轉送 Tooltip；回到原本 modal continuation。
        start_callbacks(state,memory,{{resolve("user32.dll",target->second.unicode?"SendMessageW":"SendMessageA").value,{msg.window,msg.message,msg.wparam,msg.lparam}}},Continuation::GuiModalDispatch,handle);return true;
    }
    if(begin_gui_text_thunk(state,memory,msg,dialog.unicode,Continuation::GuiModalDispatch,handle))return true;
    if(default_gui_control(memory,msg,dialog.unicode))return false;
    const auto proc=target->second.procedure;
    if(!proc)return false;
    start_callbacks(state,memory,{{proc,{msg.window,msg.message,msg.wparam,msg.lparam}}},Continuation::GuiModalDispatch,handle);return true;
}
std::optional<std::uint32_t> WindowsServices::advance_gui_modal(GuestState& state,GuestMemory& memory,std::uint64_t handle){
    for(;;){
        const auto found=gui_.windows.find(handle);if(found==gui_.windows.end()){complete_call(state,memory,UINT64_MAX);return std::nullopt;}
        auto& dialog=found->second;
        if(dialog.ended && dialog.shell_delete){
            auto& operation=*dialog.shell_delete;
            if(!operation.result){
                const bool confirmed=dialog.dialog_result==1;
                operation.result=finish_shell_delete(memory,operation,confirmed);
                if(confirmed && *operation.result && !(operation.flags&0x400U) && native_gui_){
                    dialog.ended=false;dialog.title=u"無法刪除";dialog.default_id=1;present_gui(handle);
                    for(auto& [child,w]:gui_.windows)if(w.parent==handle){
                        if(gui_control_class(w)==u"static")w.title=u"刪除未完成（錯誤碼 "+utf8_to_utf16_lossy(std::to_string(*operation.result))+u"）。請檢查寫入權限、檔案是否使用中或路徑是否仍存在。";
                        else if(w.id==1){w.title=u"確定";w.style|=1U;}
                        else if(w.id==2){w.visible=false;w.style&=~0x10000000U;}
                        present_gui(child);
                    }
                }
            }
            dialog.dialog_result=*operation.result;
        }
        if(dialog.ended && dialog.certificate_manager && dialog.dialog_result==3){dialog.ended=false;show_certificate_selection(handle);continue;}
        if(dialog.ended && dialog.certificate_manager)dialog.dialog_result=1; // 關閉憑證管理員即成功。
        if(dialog.ended && dialog.credential_prompt){
            if(!dialog.credential_prompt->result){
                const auto error=finish_credential_prompt(memory,handle,dialog.dialog_result==1);
                if(dialog.credential_prompt->legacy && dialog.dialog_result==1 && (error==2202 || error==122)){
                    dialog.ended=false;dialog.dialog_result=0;
                    for(auto& [child,w]:gui_.windows)if(w.parent==handle && w.id==0xffff && w.y==16){
                        w.title=error==2202?u"使用者名稱格式無效，請重新輸入。":u"輸入內容超過呼叫程式提供的緩衝區，請縮短後再試。";present_gui(child);
                    }
                    continue;
                }
                dialog.credential_prompt->result=error;
            }
            dialog.dialog_result=*dialog.credential_prompt->result;
        }
        if(dialog.ended){
            if(gui_.windows.contains(dialog.owner)){auto& owner=gui_.windows.at(dialog.owner);gui_set_window_enabled(owner,dialog.modal_owner_was_enabled);present_gui(dialog.owner);if(native_gui_ && owner.visible)show_native_window(dialog.owner,true);}
            std::vector<GuestCallback> callbacks;if(dialog.procedure)callbacks={{dialog.procedure,{handle,2,0,0}},{dialog.procedure,{handle,0x82,0,0}}};
            start_callbacks(state,memory,std::move(callbacks),Continuation::GuiModalEnd,handle);return std::nullopt;
        }
        if(dispatch_gui_send(state,memory,handle))return std::nullopt;
        // 先派送已排隊的訊息，避免讀入後續原生編輯並提早覆蓋尚未處理的操作。
        const bool input_pending=std::any_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& msg){return gui_message_matches(msg,threads_.current);});
        pump_gui(memory,false,input_pending?gui_native_poll_deferred:0);
        const auto queued=std::find_if(gui_.queue.begin(),gui_.queue.end(),[&](const auto& msg){return gui_message_matches(msg,threads_.current);});
        if(queued==gui_.queue.end()){wait_gui_input(state,memory,{},handle);return std::nullopt;}
        const auto msg=*queued;remember_gui_message(msg);consume_shell_message(msg);gui_.queue.erase(queued);
        if(msg.message==18){dialog.ended=true;dialog.dialog_result=UINT64_MAX;continue;}
        if(std::any_of(gui_.hooks.begin(),gui_.hooks.end(),[&](const auto& entry){const auto& hook=entry.second;return (hook.type==-1 || hook.type==6) && (!hook.thread || hook.thread==threads_.current);})) {
            if(!dialog.modal_message_buffer)dialog.modal_message_buffer=runtime_allocate(memory,{},48);
            dialog.modal_message=msg;gui_write_message(memory,dialog.modal_message_buffer,msg);
            if(start_gui_hooks(state,memory,6,0,0,dialog.modal_message_buffer,Continuation::GuiModalSystemFilter,handle) ||
               start_gui_hooks(state,memory,-1,0,0,dialog.modal_message_buffer,Continuation::GuiModalFilter,handle))return std::nullopt;
        }
        if(dispatch_gui_modal_message(state,memory,handle,msg))return std::nullopt;
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_gui(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service) {
    if(invoke_remote_gui(state,memory,service))return std::nullopt;
    if(service.symbol.starts_with("SendMessageCallback") || service.symbol.starts_with("SendNotifyMessage") || service.symbol=="WaitMessage")return invoke_gui_async(state,memory,service);
    if(intercept_gui_cbt(state,memory,service))return std::nullopt;
    const WindowsCall arg(state, memory, service); const auto symbol = service.symbol;
    const auto dword = [&](unsigned index) { return static_cast<std::uint32_t>(arg(index)); };
    const bool wide = symbol.ends_with('W');
    const auto string = [&](std::uint64_t pointer) { return !pointer ? std::u16string{} : wide ? read_utf16(memory, GuestAddress{pointer}) : utf8_to_utf16_lossy(read_ansi(memory, pointer)); };
    const auto fail = [&](std::uint32_t code) { last_error_ = code; };
    const auto dispatch = [&](const GuiMessage& msg, Continuation continuation = Continuation::ReturnCallback, std::uint64_t value = 0) {
        ++gui_.dispatched;
        if(begin_gui_text_thunk(state,memory,msg,wide,continuation,value))return;
        if((continuation==Continuation::Return || continuation==Continuation::ReturnCallback) && begin_gui_tooltip_relay(state,memory,msg,wide,continuation==Continuation::Return,value))return;
        if(msg.message==15){
            auto& window=gui_.windows.at(msg.window);window.internal_paint=false;
            if(window.procedure && (window.paint_frame || window.paint_erase)){
                std::vector<GuestCallback> callbacks;
                if(window.paint_frame)callbacks.push_back({window.procedure,{msg.window,0x85,1,0}});
                if(window.paint_erase)callbacks.push_back({window.procedure,{msg.window,0x14,dc_base+msg.window,0}});
                window.paint_frame=false;window.paint_erase=false;
                callbacks.push_back({window.procedure,{msg.window,15,msg.wparam,msg.lparam}});
                start_callbacks(state,memory,std::move(callbacks),continuation,value);return;
            }
        }
        if(continuation==Continuation::Return || continuation==Continuation::ReturnCallback)if(const auto result=default_gui_control(memory,msg,wide)){complete_call(state,memory,continuation==Continuation::Return?value:*result);return;}
        const auto& window=gui_.windows.at(msg.window);const auto procedure=window.procedure;
        start_callbacks(state, memory, {{procedure, {msg.window, msg.message, msg.wparam, msg.lparam}}}, continuation, value);
    };
    if (symbol == "PostQuitMessage" && gui_.queue.size() >= 10000) throw HostCallFault("GUI 訊息佇列超過上限");
    std::uint64_t result = 0;
    if (symbol.starts_with("RegisterClass")) {
        const auto address = arg(0); const bool extended = symbol.starts_with("RegisterClassEx");const GuiClassAbi abi{x86_,extended};
        memory.check(GuestAddress{address}, abi.size, Access::Read);
        if (extended && memory.load(GuestAddress{address}, 32) != abi.size) fail(87);
        else if (memory.load(GuestAddress{address+abi.class_extra}, 32)>65536 || memory.load(GuestAddress{address+abi.window_extra}, 32)>65536)
            fail(87);
        else {
            const auto name = string(memory.load(GuestAddress{address+abi.name}, abi.pointer_bits));
            const auto procedure = memory.load(GuestAddress{address+abi.procedure}, abi.pointer_bits);
            if (name.empty() || !procedure) fail(87);
            else if (gui_class_record(name,false)) fail(1410);
            else if (gui_.classes.size() >= 256) fail(8);
            else {
                // 註冊只保存類別屬性；視窗程序在真正進入回呼時才檢查執行權限。
                const auto menu=memory.load(GuestAddress{address+abi.menu},abi.pointer_bits);
                const auto menu_name=menu>65535?string(menu):std::u16string{};
                result = gui_.next_atom++;
                gui_.classes.emplace(name, GuiClass{procedure, memory.load(GuestAddress{address+abi.instance}, abi.pointer_bits), memory.load(GuestAddress{address+abi.background}, abi.pointer_bits), static_cast<std::uint16_t>(result),static_cast<std::uint32_t>(memory.load(GuestAddress{address+abi.window_extra},32)),memory.load(GuestAddress{address+abi.cursor},abi.pointer_bits),memory.load(GuestAddress{address+abi.icon},abi.pointer_bits),extended?memory.load(GuestAddress{address+abi.small_icon},abi.pointer_bits):0,static_cast<std::uint32_t>(memory.load(GuestAddress{address+abi.style},32))});
                auto& registered=gui_.classes.at(name);registered.unicode=wide;
                registered.class_extra_size=static_cast<std::uint32_t>(memory.load(GuestAddress{address+abi.class_extra},32));registered.extra.resize(registered.class_extra_size);
                if(menu<=65535)registered.menu_ordinal=static_cast<std::uint16_t>(menu);
                else registered.menu_name=menu_name;

            }
        }
    } else if (symbol.starts_with("CreateWindowEx")) {
        auto class_name = arg(1) > 65535 ? string(arg(1)) : std::u16string{};
        auto found = arg(1) > 65535 ? std::find_if(gui_.classes.begin(),gui_.classes.end(),[&](const auto& item){return gui_class_record(class_name,false)==&item.second;}) : std::find_if(gui_.classes.begin(), gui_.classes.end(), [&](const auto& item) { return item.second.atom == arg(1); });
        auto builtin_name=class_name;for(auto& c:builtin_name)if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
        const bool builtin=found==gui_.classes.end() && stock_gui_proc(builtin_name,wide)!=0;
        if (found == gui_.classes.end() && !builtin) fail(1407);
        else if (((arg(3)&0x40000000U) || (arg(0)&0x40U)) && arg(8)!=(x86_?UINT64_C(0xfffffffd):gui_message_parent) && arg(8)!=gui_.desktop && !gui_.windows.contains(arg(8))) fail(1400);
        else if (gui_.windows.size() >= 4096) fail(8);
        else {
            const auto width = static_cast<std::uint32_t>(arg(6)) == 0x80000000U ? 640U : static_cast<std::uint32_t>(arg(6));
            const auto height = static_cast<std::uint32_t>(arg(7)) == 0x80000000U ? 360U : static_cast<std::uint32_t>(arg(7));
            if (width > 8192 || height > 8192) { fail(87); }
            else {
                const auto handle = allocate_gui_window(); GuiWindow window;window.dpi_context=effective_dpi_context();window.nonclient_dpi_scaling=window.dpi_context==UINT64_MAX-3;window.thread=threads_.current;
                window.builtin=builtin;window.class_name=builtin?builtin_name:found->first;window.procedure = builtin?stock_gui_proc(builtin_name,wide):found->second.procedure;window.control_class=std::u16string(stock_gui_class(window.procedure));if(!window.control_class.empty())window.builtin=true;
                if(!builtin){window.class_atom=found->second.atom;window.extra.resize(found->second.extra_size);}
                else {const auto* cls=gui_class_record(builtin_name);window.class_atom=cls->atom;window.procedure=cls->procedure==stock_gui_proc(builtin_name,true)?stock_gui_proc(builtin_name,wide):cls->procedure;window.extra.resize(cls->extra_size);}window.title = arg(2)>65535?string(arg(2)):std::u16string{};
                window.parent=arg(8)==(x86_?UINT64_C(0xfffffffd):gui_message_parent)?gui_message_parent:((arg(3)&0x40000000U) || (arg(0)&0x40U)) && arg(8)!=gui_.desktop?arg(8):0;window.owner=window.parent || arg(8)==gui_.desktop?0:arg(8);window.id=arg(9);window.x=signed32(arg(4));window.y=signed32(arg(5));
                if(!window.parent){if(static_cast<std::uint32_t>(arg(4))==0x80000000U){window.x=80;window.y=80;}else if(static_cast<std::uint32_t>(arg(5))==0x80000000U)window.y=80;}
                if(!window.parent)window.menu=arg(9);
                window.enabled=(arg(3)&0x8000000U)==0;
                window.instance = arg(10); window.style = static_cast<std::uint32_t>(arg(3)); window.ex_style = static_cast<std::uint32_t>(arg(0));
                window.initial_size_pending=(window.style&0xc0000000U)==0;
                const bool combo=window.control_class==u"combobox" || window.control_class==u"comboboxex32";
                if(combo)window.combo_drop_height=height>24?height-24:100;
                bool owned_menu=false;
                if(!window.parent && !window.menu && !builtin && (found->second.menu_ordinal || !found->second.menu_name.empty())){
                    const auto& cls=found->second;
                    // 類別名稱已在註冊時複製；暫存名稱只用於資源查找。
                    std::vector<std::uint64_t> scratch;
                    try{
                        const auto name=cls.menu_ordinal?std::uint64_t{cls.menu_ordinal}:allocate_gui_scratch(memory,scratch,utf16_bytes(cls.menu_name));
                        const auto entry=gui_resource(memory,cls.instance,name,4,true);
                        if(!entry){release_gui_scratch(memory,state,scratch);fail(1814);arg.return_to(state,0);return std::nullopt;}
                        const auto* image=resource_image(entry,true);
                        window.menu=load_gui_menu(memory,image->base+memory.load(GuestAddress{entry},32),memory.load(GuestAddress{entry+4},32));
                        owned_menu=true;
                    }catch(...){release_gui_scratch(memory,state,scratch);throw;}
                    release_gui_scratch(memory,state,scratch);
                }
                gui_refresh_frame(window);const auto client=gui_client_size(width,combo && (window.style&3U)!=1U?24U:height,gui_frame_insets(window));window.width=client[0];window.height=client[1];
                if(const auto error=prepare_mdi_window(memory,window,arg(11),{dword(4),dword(5),dword(6),dword(7)})) {
                    if(owned_menu)destroy_gui_menu(window.menu);fail(error);arg.return_to(state,0);return std::nullopt;
                }
                GuiWindowCreation plan{handle,std::move(window),wide};plan.owned_menu=owned_menu;plan.parameter=arg(11);plan.class_pointer=arg(1);plan.name_pointer=arg(2);
                if(plan.window.mdi_child) {
                    plan.parameter_bytes.resize(x86_?36:56);
                    const unsigned unit=x86_?4:8,integers=x86_?12:24;
                    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<unit;++j)plan.parameter_bytes[i*unit+j]=static_cast<std::uint8_t>(arg(i==0?1:i==1?2:10)>>(8*j));
                    for(unsigned i=0;i<5;++i)for(unsigned j=0;j<4;++j)plan.parameter_bytes[integers+4*i+j]=static_cast<std::uint8_t>(dword(i<4?i+4:3)>>(8*j));
                    for(unsigned j=0;j<unit;++j)plan.parameter_bytes[(x86_?32:48)+j]=static_cast<std::uint8_t>(arg(11)>>(8*j));
                }
                return start_gui_creation(state,memory,{std::move(plan)});
            }
        }
    } else if (symbol == "PostQuitMessage") { GuiMessage message{0,static_cast<std::uint32_t>(arg(0)),0,18};message.thread=threads_.current;enqueue_gui_message(message); }
    else if (symbol.starts_with("GetMessage") || symbol.starts_with("PeekMessage")) {
        const bool peek = symbol.starts_with("PeekMessage");
        if(!peek || dword(4)==1)retire_shell_messages(memory);
        if ((arg(1) && arg(1) != (x86_?UINT32_MAX:UINT64_MAX) && (!gui_.windows.contains(arg(1)) || gui_.windows.at(arg(1)).thread!=threads_.current)) || dword(2) > 65535 || dword(3) > 65535 || (peek && dword(4) > 1)) { fail(87); result = peek ? 0 : UINT32_MAX; }
        else {
            memory.check(GuestAddress{arg(0)}, x86_?28:48, Access::Write);
            for (;;) {
                if(dispatch_gui_send(state,memory))return std::nullopt;
                const bool input_pending=std::any_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& msg){return gui_message_matches(msg,threads_.current,arg(1),dword(2),dword(3));});
                pump_gui(memory,false,input_pending?gui_native_poll_deferred:0);
                // 宿主 pump 可能產生新的 sent 通知，須先交給 WNDPROC 再回傳 Peek／Get。
                if(dispatch_gui_send(state,memory))return std::nullopt;
                for(auto& message:gui_.queue)if(gui_message_matches(message,threads_.current))message.queue_seen=true;
                // 保留同來源的 FIFO；明確 PostMessage 的 WM_PAINT/WM_TIMER 仍是 posted。
                auto found=gui_.queue.end();
                for(auto it=gui_.queue.begin();it!=gui_.queue.end();++it)
                    if(gui_message_matches(*it,threads_.current,arg(1),dword(2),dword(3)) &&
                       (found==gui_.queue.end() || it->queue_priority<found->queue_priority))found=it;
                if (found != gui_.queue.end()) {
                    if(!peek || dword(4)){
                        consume_gui_input(*found);consume_shell_message(*found);
                    }
                    remember_gui_message(*found);gui_write_message(memory, arg(0), *found); result = peek || found->message != 18 ? 1 : 0;
                    const bool queuesync=found->message==0x23 && (!peek || dword(4));
                    if (!peek || dword(4)) gui_.queue.erase(found);
                    if(queuesync && start_gui_hooks(state,memory,5,2,0,0,Continuation::Return,result))return std::nullopt;
                    break;
                }
                if (!gui_.windows.empty()) report_child_input_idle();
                if (peek) break;
                ThreadWait wait;wait.gui_window=arg(1);wait.gui_min=dword(2);wait.gui_max=dword(3);
                wait_gui_input(state,memory,std::move(wait));return std::nullopt;
            }
        }
    } else if (symbol == "TranslateMessage") {
        const auto msg=gui_read_message(memory,arg(0));
        if(gui_.queue.size()>=9998)fail(1816);else result=translate_gui_key(msg)?1:0;
    } else if (symbol.starts_with("DispatchMessage")) {
        auto msg = gui_read_message(memory, arg(0));
        trace_gui_input(trace_services_,"dispatch",msg,threads_.current,runtime_.clock_ticks);
        // VK_PACKET 的 MSG 高字承載 UTF-16；傳給 WNDPROC 時恢復正常按鍵旗標。
        if(msg.wparam==0xe7 && (msg.message==0x100 || msg.message==0x101 || msg.message==0x104 || msg.message==0x105))msg.lparam=(msg.lparam&65535U)|((msg.message==0x104 || msg.message==0x105)?0x20000000U:0)|((msg.message==0x101 || msg.message==0x105)?0xc0000000U:0);
        if(msg.message==0x1000a)return begin_gui_main_menu(state,memory,msg);
        if(msg.message==0x113 && msg.lparam){
            if(!handles(GuestAddress{msg.lparam}))memory.check(GuestAddress{msg.lparam},1,Access::Execute);
            GuestCallback callback{msg.lparam,{msg.window,0x113,msg.wparam,static_cast<std::uint32_t>(runtime_.clock_ticks/10000)}};
            callback.user_mode_callback=true;start_callbacks(state,memory,{std::move(callback)},Continuation::Return,0);return std::nullopt;
        }
        if (gui_.windows.contains(msg.window)) { dispatch(msg); return std::nullopt; }
    } else if(symbol.starts_with("PostThreadMessage")) {
        const auto id=thread_from_identifier(dword(0));const auto thread=threads_.items.find(id);
        if(thread==threads_.items.end() || thread->second.status==ThreadStatus::Exited || !gui_.message_threads.contains(id))fail(1444);
        else if(gui_.queue.size()>=10000)fail(1816);
        else {GuiMessage message{0,arg(2),arg(3),dword(1)};message.thread=id;enqueue_gui_message(message);result=1;}
    } else if(symbol.starts_with("SendMessageTimeout")){
        const auto flags=dword(4);const GuiMessage message{arg(0),arg(2),arg(3),dword(1)};
        if(flags&~0x2bU)fail(87);
        else if(arg(0)==0xffff)fail(50); // 廣播與跨程序訊息尚無共用視窗 broker。
        else {
            if(arg(6))memory.check(GuestAddress{arg(6)},x86_?4:8,Access::Write);
            if(route_gui_send(state,memory,message,wide,true,flags,dword(5),arg(6)))return std::nullopt;
            if(!gui_.windows.contains(arg(0)))fail(1400);
            else {start_callbacks(state,memory,{{resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value,{arg(0),arg(1),arg(2),arg(3)}}},Continuation::GuiSendTimeout);return std::nullopt;}
        }
    } else if (symbol.starts_with("SendMessage") || symbol.starts_with("PostMessage")) {
        GuiMessage msg{arg(0),arg(2),arg(3),static_cast<std::uint32_t>(arg(1))};if(!msg.window)msg.thread=threads_.current;
        if(symbol.starts_with("SendMessage") && route_gui_send(state,memory,msg,wide))return std::nullopt;
        if (msg.window && !gui_.windows.contains(msg.window)) fail(1400);
        else if (symbol.starts_with("PostMessage")) { if (gui_.queue.size() >= 10000) fail(1816); else { enqueue_gui_message(msg); result = 1; } }
        else if (msg.window) { dispatch(msg); return std::nullopt; }
    } else if (symbol == "LoadCursorW" || symbol == "LoadCursorA") {
        return invoke_icon_api(state,memory,service);
    } else if (symbol == "GetSystemMetrics") {
        result=system_metric(dword(0));
    } else if (symbol == "GetStockObject") {
        if (dword(0) > 19) throw HostCallFault("尚未支援此 GDI stock object"); result = 0xb000 + dword(0);
    } else if (symbol == "CreateSolidBrush") {
        if (gui_.brushes.size() < 4096) { result = gui_.next_object++; gui_.brushes.emplace(result, static_cast<std::uint32_t>(arg(0))); }
    } else if (symbol == "DeleteObject") { result = gui_.brushes.erase(arg(0)) || (gui_.font_natives.erase(arg(0)),gui_.fonts.erase(arg(0))) ? 1 : 0; }
    else if(symbol=="GetClientRect" && arg(0)==gui_.desktop){
        memory.write(GuestAddress{arg(1)},std::array<std::uint8_t,16>{});
        const auto area=gui_screen_area();memory.store(GuestAddress{arg(1)+8},32,static_cast<std::uint32_t>(area[2]-area[0]));memory.store(GuestAddress{arg(1)+12},32,static_cast<std::uint32_t>(area[3]-area[1]));result=1;
    }
    else {
        const bool dc_call = symbol == "FillRect" || symbol.starts_with("TextOut") || symbol == "SetTextColor" || symbol == "SetBkMode";
        const auto handle = dc_call && arg(0) >= dc_base ? arg(0) - dc_base : arg(0);
        const auto found = gui_.windows.find(handle);
        if (found == gui_.windows.end()) fail(dc_call ? 6 : 1400);
        else {
            auto& window = found->second;
            if (symbol == "ShowWindow") {
                auto command=dword(1);
                if(runtime_.startup_show!=UINT32_MAX && (!runtime_.startup_show_used || command==10)){command=runtime_.startup_show==10?1:runtime_.startup_show;runtime_.startup_show_used=true;}
                else if(command==10)command=1;
                if(command>11){fail(87);}
                else {
                    result=window.visible?1:0;const auto previous=window.show_command;
                    const bool minimize=command==2 || command==6 || command==7 || command==11;
                    auto next=command==3?3U:minimize?2U:(command==1 || command==4 || command==9)?1U:previous;
                    if(command==9 && previous==2 && window.restore_maximized)next=3;
                    if(next!=previous){
                        if(previous==1)window.normal_rect=std::array<std::int32_t,4>{window.x,window.y,static_cast<std::int32_t>(window.width),static_cast<std::int32_t>(window.height)};
                        if(next==2){window.restore_maximized=previous==3;if(window.mdi_child && gui_.windows.contains(window.parent)){const auto& parent=gui_.windows.at(window.parent);const auto inset=gui_frame_insets(window);window.x=0;window.y=static_cast<std::int32_t>(parent.height)-32;window.width=160-inset.left-inset.right;window.height=32>inset.top+inset.bottom?32-inset.top-inset.bottom:0;}}
                        else if(next==3){
                            if(window.mdi_child && gui_.windows.contains(window.parent)){const auto& parent=gui_.windows.at(window.parent);const auto inset=gui_frame_insets(window);window.x=-static_cast<std::int32_t>(inset.left);window.y=-static_cast<std::int32_t>(inset.top);window.width=parent.width;window.height=parent.height;}
                            else {const auto area=gui_work_area();const auto size=gui_client_size(static_cast<std::uint32_t>(area[2]-area[0]),static_cast<std::uint32_t>(area[3]-area[1]),gui_frame_insets(window));window.x=area[0];window.y=area[1];window.width=size[0];window.height=size[1];}
                        }
                        else if(window.normal_rect){const auto& r=*window.normal_rect;window.x=r[0];window.y=r[1];window.width=static_cast<std::uint32_t>(r[2]);window.height=static_cast<std::uint32_t>(r[3]);}
                    }
                    window.show_command=next;window.visible=command!=0;
                    window.style=(window.style&~0x31000000U)|(window.visible?0x10000000U:0U)|(next==2?0x20000000U:next==3?0x01000000U:0U);
                    present_gui(handle);if(native_gui_)show_native_window(handle,window.visible);
                    if(window.visible && next!=2)invalidate_window(handle);
                    const bool size_changed=window.visible && (next!=previous || window.initial_size_pending);
                    window.initial_size_pending=false;
                    if(size_changed && window.procedure){const auto packed=next==2?0:(window.width&65535U)|((window.height&65535U)<<16);
                        start_callbacks(state,memory,{{window.procedure,{handle,5,next==2?1U:next==3?2U:0U,packed}},{window.procedure,{handle,3,0,gui_move_lparam(window)}}},Continuation::Return,result);return std::nullopt;}
                }
            } else if (symbol == "DestroyWindow") { if(window.thread!=threads_.current)fail(5);else {destroy_window(state,memory,handle,false);return std::nullopt;} }
            else if (symbol == "UpdateWindow") {
                result = 1;
                if (window.invalid) { std::erase_if(gui_.queue, [&](const auto& msg) { return msg.window == handle && msg.message == 15; }); dispatch({handle, 0, 0, 15}, Continuation::Return, 1); return std::nullopt; }
            } else if (symbol.starts_with("DefWindowProc")) {
                if(window.mdi && dispatch_mdi_client(state,memory,{handle,arg(2),arg(3),dword(1)},wide))return std::nullopt;
                if(dword(1)==0x1f){
                    gui_cancel_window_menu(gui_,handle);
                    if(gui_.capture==handle){start_callbacks(state,memory,{{resolve("user32.dll","ReleaseCapture").value,{}}},Continuation::Return,0);return std::nullopt;}
                }
                if(dword(1)==0xff)release_raw_input(arg(3));
                if(begin_gui_control(state,memory,{handle,arg(2),arg(3),dword(1)},wide))return std::nullopt;
                if(dword(1)==0x18 && arg(3) && window.owner && (arg(2)?!window.visible && window.owned_popup_hidden:window.visible)){
                    // ShowOwnedPopups：SW_PARENTCLOSING 隱藏並記錄，SW_PARENTOPENING 只恢復先前因擁有者而隱藏的視窗。
                    window.owned_popup_hidden=!arg(2);
                    start_callbacks(state,memory,{{resolve("user32.dll","ShowWindow").value,{handle,arg(2)?4U:0U}}},Continuation::Return,0);return std::nullopt;
                }
                if (dword(1) == 0x81 || dword(1)==0x86) result = 1;
                else if(dword(1)>=0x245 && dword(1)<=0x247 && promote_pointer_message(handle,arg(2),dword(1)))result=0;
                else if(dword(1)==0x84)result=gui_window_hit(gui_,handle,static_cast<std::int16_t>(arg(3)),static_cast<std::int16_t>(arg(3)>>16));
                else if(dword(1)==0x21){
                    if(window.parent && gui_.windows.contains(window.parent)){
                        start_callbacks(state,memory,{{resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value,{window.parent,0x21,arg(2),arg(3)}}},Continuation::ReturnCallback,0);return std::nullopt;
                    }
                    result=(window.ex_style&0x08000000U)?3U:1U;
                }
                else if(dword(1)==6 && (dword(2)&65535U) && !(dword(2)>>16) && gui_root_window(gui_.focus)!=handle){
                    start_callbacks(state,memory,{{resolve("user32.dll","SetFocus").value,{handle}}},Continuation::Return,0);return std::nullopt;
                }
                else if(dword(1)==0xa1 || dword(1)==0xa3){
                    const auto hit=dword(2);std::uint32_t command=0;
                    if(hit==2)command=dword(1)==0xa3 && (window.style&0x10000U)?(window.show_command==3?0xf120U:0xf030U):0xf012U;
                    else if(hit>=10 && hit<=17){static constexpr unsigned edges[]{1,2,3,4,5,6,7,8};command=0xf000U|edges[hit-10];}
                    else if(hit==20)command=0xf060U;else if(hit==8)command=0xf020U;else if(hit==9)command=window.show_command==3?0xf120U:0xf030U;
                    if(command){start_callbacks(state,memory,{{resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value,{handle,0x112,command,arg(3)}}},Continuation::Return,0);return std::nullopt;}
                }
                else if(dword(1)==0x83){
                    memory.check(GuestAddress{arg(3)},arg(2)?(x86_?52U:56U):16U,Access::Write);
                    const auto f=gui_frame_insets(window.style,window.ex_style,window.menu!=0);
                    const auto left=signed32(memory.load(GuestAddress{arg(3)},32)),top=signed32(memory.load(GuestAddress{arg(3)+4},32));
                    const auto right=signed32(memory.load(GuestAddress{arg(3)+8},32)),bottom=signed32(memory.load(GuestAddress{arg(3)+12},32));
                    const auto x=std::min<std::int64_t>(right,std::int64_t{left}+f.left),y=std::min<std::int64_t>(bottom,std::int64_t{top}+f.top);
                    const std::array<std::int64_t,4> rect{x,y,std::max<std::int64_t>(x,std::int64_t{right}-f.right),std::max<std::int64_t>(y,std::int64_t{bottom}-f.bottom)};
                    for(unsigned j=0;j<4;++j)memory.store(GuestAddress{arg(3)+4*j},32,static_cast<std::uint32_t>(rect[j]));
                    result=arg(2)?0x300U:0U;
                }
                else if(dword(1)==0x112){
                    const auto command=dword(2)&0xfff0U;
                    if(command==0xf010 || command==0xf000)return begin_gui_move_size(state,memory,handle,dword(2),arg(3));
                    if(command==0xf060){start_callbacks(state,memory,{{window.procedure,{handle,0x10,0,0}}},Continuation::Return,0);return std::nullopt;}
                    if(command==0xf120){state.reg(Gpr::Rdx)=9;return invoke_gui(state,memory,{"user32.dll","ShowWindow",2});}
                    if(command==0xf020 || command==0xf030){state.reg(Gpr::Rdx)=command==0xf020?2:3;return invoke_gui(state,memory,{"user32.dll","ShowWindow",2});}
                }
                else if (dword(1) == 0x10) { destroy_window(state, memory, handle, true); return std::nullopt; }
                else if (dword(1) == 0x47) {
                    memory.check(GuestAddress{arg(3)},x86_?28:40,Access::Read);const auto flags=memory.load(GuestAddress{arg(3)+(x86_?24:32)},32);
                    const auto proc=window.procedure;std::vector<GuestCallback> callbacks;
                    if(proc && !(flags&2))callbacks.push_back({proc,{handle,3,0,gui_move_lparam(window)}});
                    if(!(flags&1))window.initial_size_pending=false;
                    if(proc && !(flags&1))callbacks.push_back({proc,{handle,5,window.show_command==2?1U:window.show_command==3?2U:0U,(window.width&65535U)|((window.height&65535U)<<16)}});
                    start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;
                }
                else if (dword(1) == 15) { window.invalid = false; std::erase_if(gui_.queue, [&](const auto& msg) { return msg.window == handle && msg.message == 15; }); }
                else if(const auto control=gui_control_message(memory,{handle,arg(2),arg(3),dword(1)},wide))result=*control;
                else if (dword(1) == 12) { window.title = string(arg(3)); present_gui(handle); result = 1; }
                else if(dword(1)==14)result=wide?window.title.size():utf16_to_utf8(window.title).size();
                else if(dword(1)==13 && dword(2)){
                    if(wide){const auto text=window.title.substr(0,dword(2)-1);memory.write(GuestAddress{arg(3)},utf16_bytes(text));result=text.size();}
                    else {auto text=utf16_to_utf8(window.title);text.resize(std::min<std::size_t>(text.size(),dword(2)-1));result=text.size();text.push_back(0);memory.write(GuestAddress{arg(3)},std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));}
                }
            } else if (symbol == "GetClientRect") {
                memory.write(GuestAddress{arg(1)}, std::array<std::uint8_t, 16>{}); memory.store(GuestAddress{arg(1) + 8}, 32, window.width); memory.store(GuestAddress{arg(1) + 12}, 32, window.height); result = 1;
            } else if (symbol == "InvalidateRect" || symbol == "InvalidateRgn") {
                // 尚無 HRGN 後端；區域失效以整個視窗失效處理，只增加重繪，不遺漏更新。
                invalidate_window(handle); if(arg(2))window.paint_erase=true; result = 1;
            }
            else if (symbol == "BeginPaint") {
                const unsigned unit=x86_?4U:8U;
                memory.write(GuestAddress{arg(1)}, std::vector<std::uint8_t>(x86_?64:72)); memory.store(GuestAddress{arg(1)}, x86_?32:64, dc_base + handle);
                memory.store(GuestAddress{arg(1)+unit},32,1);memory.store(GuestAddress{arg(1)+unit+12},32,window.width);memory.store(GuestAddress{arg(1)+unit+16},32,window.height);
                window.invalid = false; window.drawing.clear();
                if(gui_.contexts[dc_base+handle].window_frame){gui_.contexts[dc_base+handle]=GuiDc{};gui_.dc_objects[dc_base+handle]=0xb011;}
                gui_.contexts[dc_base+handle].clipped=false;gui_.contexts[dc_base+handle].excluded.clear();gui_.contexts[dc_base+handle].application_clip.reset();gui_.contexts[dc_base+handle].meta_clip.reset();
                std::erase_if(gui_.queue, [&](const auto& msg) { return msg.window == handle && msg.message == 15; }); result = dc_base + handle;
            } else if (symbol == "EndPaint") {
                memory.check(GuestAddress{arg(1)}, x86_?64:72, Access::Read);
                if (memory.load(GuestAddress{arg(1)}, x86_?32:64) != dc_base + handle) throw HostCallFault("EndPaint 的 PAINTSTRUCT 不符合視窗");
                if (native_gui_) present_native_window(handle, window); ++gui_.paints; result = 1;
                gui_.contexts[dc_base+handle].clipped=false;gui_.contexts[dc_base+handle].excluded.clear();gui_.contexts[dc_base+handle].application_clip.reset();gui_.contexts[dc_base+handle].meta_clip.reset();
            } else if (symbol == "SetTextColor") { result = window.text_color; window.text_color = static_cast<std::uint32_t>(arg(1)); }
            else if (symbol == "SetBkMode") { if (dword(1) == 1 || dword(1) == 2) { result = window.background_mode; window.background_mode = static_cast<std::uint32_t>(arg(1)); } }
            else if (symbol == "FillRect") {
                const auto brush = arg(2); std::uint32_t color = 0;
                if (gui_.brushes.contains(brush)) color = gui_.brushes.at(brush);
                else if (brush == 0xb000 || brush == 6) color = 0xffffff;
                else if (brush == 0xb001) color = 0xc0c0c0; else if (brush == 0xb002) color = 0x808080; else if (brush == 0xb003) color = 0x404040;
                else if (brush == 0xb005) { complete_call(state,memory,1); return std::nullopt; }
                else if (brush != 0xb004) throw HostCallFault("FillRect brush 尚未支援");
                GuiDraw draw; draw.kind = GuiDraw::Kind::Fill; draw.color = color;
                draw.x = signed32(memory.load(GuestAddress{arg(1)}, 32)); draw.y = signed32(memory.load(GuestAddress{arg(1) + 4}, 32));
                draw.right = signed32(memory.load(GuestAddress{arg(1) + 8}, 32)); draw.bottom = signed32(memory.load(GuestAddress{arg(1) + 12}, 32));
                if (window.drawing.size() >= 4096) throw HostCallFault("GDI 繪圖命令超過上限");
                if (draw.right > draw.x && draw.bottom > draw.y) window.drawing.push_back(std::move(draw)); present_gui(handle); result = 1;
            } else if (symbol.starts_with("TextOut")) {
                if (dword(4) > 65536 || window.drawing.size() >= 4096) throw HostCallFault("GDI 繪圖資料超過上限");
                GuiDraw draw; draw.kind = GuiDraw::Kind::Text; draw.x = signed32(arg(1)); draw.y = signed32(arg(2)); draw.color = window.text_color;
                if (wide) for (std::uint64_t i = 0; i < dword(4); ++i) draw.text.push_back(static_cast<char16_t>(memory.load(GuestAddress{arg(3) + i * 2}, 16)));
                else { std::string text; for (std::uint64_t i = 0; i < dword(4); ++i) text.push_back(static_cast<char>(memory.load(GuestAddress{arg(3) + i}, 8))); draw.text = utf8_to_utf16_lossy(text); }
                window.drawing.push_back(std::move(draw)); present_gui(handle); result = 1;
            } else if (symbol.starts_with("GetWindowLongPtr") || symbol.starts_with("SetWindowLongPtr")) {
                const auto index = signed32(arg(1)); const bool set = symbol.starts_with("Set");
                const unsigned pointer_bytes=x86_?4:8;
                if(window.dialog && index==static_cast<std::int32_t>(pointer_bytes)){result=window.dialog_proc;if(set){if(arg(2) && !handles(GuestAddress{arg(2)}))memory.check(GuestAddress{arg(2)},1,Access::Execute);window.dialog_proc=arg(2);}}
                else if(index>=0 && static_cast<std::uint64_t>(index)+pointer_bytes<=window.extra.size()){
                    for(unsigned i=0;i<pointer_bytes;++i)result|=static_cast<std::uint64_t>(window.extra[static_cast<std::size_t>(index)+i])<<(i*8);
                    if(set)for(unsigned i=0;i<pointer_bytes;++i)window.extra[static_cast<std::size_t>(index)+i]=static_cast<std::uint8_t>(arg(2)>>(i*8));
                }
                else if (index == -21) { result = window.user_data; if (set) window.user_data = arg(2); }
                else if (index == -4) { result=gui_window_procedure(handle,wide);if(set){
                    auto procedure=arg(2);bool encoding=wide;const auto reference=gui_procedure_references_.find(procedure);
                    if(reference!=gui_procedure_references_.end()){procedure=reference->second.procedure;encoding=reference->second.wide;}
                    if(!handles(GuestAddress{procedure}))memory.check(GuestAddress{procedure},1,Access::Execute);window.procedure=procedure;window.unicode=encoding;
                } }
                else if (index == -6) {result=window.instance;if(set)window.instance=arg(2);}
                else if (index == -12) {result=window.id;if(set){window.id=arg(2);publish_gui_window(handle);}}
                else if (index == -16 || index == -20) {
                    result=index==-16?window.style:window.ex_style;
                    if(set){
                        if(gui_.control_operations.size()>=128)throw HostCallFault("視窗樣式回呼超過上限");
                        // 保留舊 nonclient 快取，FRAMECHANGED 再套用新的框架幾何。
                        if(!window.frame_insets)gui_refresh_frame(window);
                        GuiControlOperation operation;operation.thread=threads_.current;operation.wide=wide;
                        operation.message={handle,static_cast<std::uint64_t>(static_cast<std::int64_t>(index)),0,0x7c};
                        operation.style=GuiControlOperation::Style{index,static_cast<std::uint32_t>(result),false};
                        if(gui_.control_notification_buffers.empty())operation.buffer=runtime_allocate(memory,{},gui_control_notification_bytes);
                        else {operation.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}
                        const auto buffer=operation.buffer,token=gui_.next_object++;operation.message.lparam=buffer;
                        memory.store(GuestAddress{buffer},32,result);memory.store(GuestAddress{buffer+4},32,static_cast<std::uint32_t>(arg(2)));
                        gui_.control_operations.emplace(token,std::move(operation));
                        try{start_callbacks(state,memory,{{resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value,{handle,0x7c,static_cast<std::uint64_t>(static_cast<std::int64_t>(index)),buffer}}},Continuation::GuiControlOperation,token);}
                        catch(...){discard_gui_control(token);throw;}
                        return std::nullopt;
                    }
                }
                else fail(1413);
            } else throw HostCallFault("尚未實作此 GUI 服務");
        }
    }
    arg.return_to(state, result); return std::nullopt;
}
}
