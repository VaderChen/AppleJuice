#include "x64arm/windows_services.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/windows_calendar.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_window_state.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <numeric>
#include <cstdlib>

namespace x64arm {
namespace {
constexpr std::size_t display_bytes=gui_control_notification_bytes;
bool callback_text(const GuiItem& item,std::uint32_t subitem){
    return gui_list_text_callback(item,subitem);
}
}
bool WindowsServices::begin_gui_control(GuestState& state,GuestMemory& memory,const GuiMessage& message,bool wide){
    const auto window=gui_.windows.find(message.window);
    if(window==gui_.windows.end())return false;
    const auto& cls=gui_control_class(window->second);
    const GuiControlAbi abi(x86_);const auto header=abi.notify_size;
    if(cls==u"edit" && window->second.header_owner){
        const auto owner=gui_.windows.find(window->second.header_owner);
        if(owner!=gui_.windows.end() && owner->second.header && owner->second.header->filter_editing && owner->second.header->filter_editor==message.window){
            if(message.message==0x87 && (message.wparam==13 || message.wparam==27)){complete_call(state,memory,0x8d);return true;}
            if((message.message==0x100 && (message.wparam==13 || message.wparam==27)) || message.message==8){
                const auto forwarded=message.message==8?GuiMessage{owner->first,UINT64_C(0x2000000)|(window->second.id&65535U),message.window,0x111}:GuiMessage{owner->first,message.wparam,owner->second.header->filter_generation,0x10018};
                start_callbacks(state,memory,{{resolve("user32.dll","SendMessageW").value,{forwarded.window,forwarded.message,forwarded.wparam,forwarded.lparam}}},Continuation::ReturnCallback);return true;
            }
        }
    }
    if(cls==u"edit" && window->second.calendar_owner){
        const auto owner=gui_.windows.find(window->second.calendar_owner);
        if(owner!=gui_.windows.end() && owner->second.calendar && owner->second.calendar->editing){
            if(message.message==0x87 && (message.wparam==13 || message.wparam==27)){complete_call(state,memory,0x8d);return true;}
            if((message.message==0x100 && (message.wparam==13 || message.wparam==27)) || message.message==8){
                const GuiMessage forwarded=message.message==8?GuiMessage{owner->first,UINT64_C(0x2000000)|(window->second.id&65535U),message.window,0x111}:GuiMessage{owner->first,message.wparam,message.lparam,0x100};
                start_callbacks(state,memory,{{resolve("user32.dll","SendMessageW").value,{forwarded.window,forwarded.message,forwarded.wparam,forwarded.lparam}}},Continuation::ReturnCallback);return true;
            }
        }
    }
    if(gui_calendar_class(cls) && ((message.message==1 && !window->second.calendar) || (message.message==0x1a && window->second.calendar) || (window->second.calendar && !window->second.calendar->picker && message.message==0x100c && !message.lparam))){
        if(gui_.control_operations.size()>=128)throw HostCallFault("日期控制項初始化回呼超過上限");
        GuiControlOperation op;op.message=message;op.thread=threads_.current;op.wide=wide;op.calendar.emplace();op.calendar->stage=message.message==1?7U:message.message==0x1a?30U:9U;
        if(gui_.control_notification_buffers.empty())op.buffer=runtime_allocate(memory,{},gui_control_notification_bytes);else {op.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}
        const auto token=gui_.next_object++,buffer=op.buffer;gui_.control_operations.emplace(token,std::move(op));
        std::vector<GuestCallback> callbacks;if(message.message!=0x1a)callbacks.push_back({resolve("kernel32.dll","GetLocalTime").value,{buffer+64}});
        if(message.message==0x1a || (message.message==1 && !gui_.calendar_locale)){auto& request=*gui_.control_operations.at(token).calendar;request.locale_revision=++gui_.calendar_locale_revision;request.locale_buffer=allocate_heap(memory,gui_calendar_locale_types.size()*gui_calendar_locale_chars*2);if(!request.locale_buffer){discard_gui_control(token);throw HostCallFault("日期語系資料配置失敗");}memory.write(GuestAddress{request.locale_buffer},std::vector<std::uint8_t>(gui_calendar_locale_types.size()*gui_calendar_locale_chars*2));for(std::size_t n=0;n<gui_calendar_locale_types.size();++n)callbacks.push_back({resolve("kernel32.dll","GetLocaleInfoW").value,{0x400,gui_calendar_locale_types[n],request.locale_buffer+n*gui_calendar_locale_chars*2,gui_calendar_locale_chars}});}
        try{start_callbacks(state,memory,std::move(callbacks),Continuation::GuiControlOperation,token);}catch(...){if(const auto live=gui_.control_operations.find(token);live!=gui_.control_operations.end() && live->second.calendar && live->second.calendar->locale_buffer){const auto buffer_to_free=live->second.calendar->locale_buffer;live->second.calendar->locale_buffer=0;(void)free_heap(memory,buffer_to_free,state);}discard_gui_control(token);throw;}return true;
    }
    constexpr std::array common_classes{u"syslink",u"systabcontrol32",u"systreeview32",u"syslistview32",u"sysheader32",u"toolbarwindow32",u"msctls_statusbar32",u"rebarwindow32",u"msctls_progress32",u"comboboxex32",u"msctls_updown32",u"msctls_trackbar32",u"tooltips_class32",u"sysdatetimepick32",u"sysmonthcal32"};
    if((message.message==1 || (message.message==0x55 && message.lparam==4)) && std::any_of(common_classes.begin(),common_classes.end(),[&](const auto name){return cls==name;})){
        if(gui_.control_operations.size()>=128)throw HostCallFault("共用控制項通知格式回呼超過上限");
        GuiControlOperation op;op.message=message;op.thread=threads_.current;op.wide=wide;op.header.emplace();op.header->kind=GuiControlOperation::Header::Kind::Format;
        if(message.message==1)window->second.list_unicode=window->second.unicode;
        const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(op));try{(void)resume_gui_header(state,memory,token,0);}catch(...){discard_gui_control(token);throw;}return true;
    }
    if(gui_calendar_class(cls) && (gui_calendar_dispatch(message.message) || message.message==0x4e || message.message==0x111)){
        auto& control=window->second;if(!control.calendar)return false;
        GuiControlOperation op;op.message=message;op.thread=threads_.current;op.wide=wide;op.calendar.emplace();auto& request=*op.calendar;const auto previous_value_revision=control.calendar->picker_revision;
        const auto storage=[&]{if(gui_.control_notification_buffers.empty())op.buffer=runtime_allocate(memory,{},gui_control_notification_bytes);else {op.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}memory.write(GuestAddress{op.buffer},std::vector<std::uint8_t>(gui_control_notification_bytes));};
        if(message.message==0x4e){
            auto& date=*control.calendar;if(!date.picker || !message.lparam){complete_call(state,memory,0);return true;}
            const auto child=memory.load(GuestAddress{message.lparam},abi.bits);const auto code=static_cast<std::uint32_t>(memory.load(GuestAddress{message.lparam+abi.unit*2},32));
            if(child==date.updown_window && code==static_cast<std::uint32_t>(-722)){
                const auto delta=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{message.lparam+header+4},32)));if(control.enabled && date.field>=9 && delta){date.callback_key=delta>0?38U:40U;request.return_value=0;}else request.return_value=control.enabled && gui_date_field_step(date,delta)?0U:1U;present_gui(message.window);
            }else if(child==date.month_window && code==static_cast<std::uint32_t>(-746)){
                GuiSystemTime selected=date.value;for(unsigned n=0;n<4;++n)selected[n]=static_cast<std::uint16_t>(memory.load(GuestAddress{message.lparam+header+n*2},16));storage();gui_date_write(memory,op.buffer+64,selected);
                (void)gui_control_message(memory,{message.window,0,op.buffer+64,0x1002},wide);date.close_request=date.open;
            }else {complete_call(state,memory,0);return true;}
        }else if(message.message==0x111){
            auto& date=*control.calendar;if(!date.picker || message.lparam!=date.edit_window || !date.editing || (message.wparam>>16)!=0x200U){complete_call(state,memory,0);return true;}date.commit_request=true;
        }else {
            if(message.message==15){std::erase_if(gui_.queue,[&](const auto& queued){return queued.window==message.window && queued.message==15;});control.invalid=false;}
            request.return_value=gui_control_message(memory,message,wide).value_or(0);
            if(control.calendar->picker && control.calendar->open && (message.message==0x1006 || message.message==0x1009 || message.message==0x100b)){
                const auto child=gui_.windows.find(control.calendar->month_window);if(child!=gui_.windows.end() && child->second.calendar){
                    auto& month=child->second;month.calendar->colors=control.calendar->colors;
                    if(message.message==0x1009){month.font=control.calendar->month_font?control.calendar->month_font:control.font;month.control_font=gui_font(gui_,month.font);gui_calendar_project(month,native_gui_.get());}
                    if(message.message==0x100b)month.style=(month.style&0xffff0000U)|(control.calendar->month_style&0xffffU);
                    if(message.message==0x100b || (message.message==0x1009 && message.lparam)){gui_calendar_project(month,native_gui_.get());const auto insets=gui_frame_insets(month);const auto width=month.calendar->minimum_width,height=month.calendar->minimum_height;if(width!=month.width || height!=month.height){request.resize_window=child->first;request.resize_width=width+insets.left+insets.right;request.resize_height=height+insets.top+insets.bottom;request.stage=15;}}
                    if(message.message!=0x1009 || message.lparam)present_gui(child->first);
                }
            }
            if(message.message==0x201 && control.enabled && control.calendar->pressed>=0){gui_.capture=message.window;if(gui_.focus!=message.window)request.stage=12;}
            if(message.message==0x202 && gui_.capture==message.window)gui_.capture=0;
        }
        auto& date=*control.calendar;request.notices=std::move(date.notices);date.notices.clear();request.open=date.open_request;request.close=date.close_request;request.edit=date.edit_request;request.commit=date.commit_request;request.cancel=date.cancel_request;date.open_request=date.close_request=date.edit_request=date.commit_request=date.cancel_request=false;
        if(request.edit)op.text=control.title;
        if(request.commit || request.cancel){request.revision=date.picker_revision;request.edit_generation=date.edit_generation;if(const auto editor=gui_.windows.find(date.edit_window);editor!=gui_.windows.end())op.text=editor->second.title;}
        if(message.message!=0x100f || message.lparam)gui_date_capture_callbacks(control,request,message.message,previous_value_revision);
        if(request.notices.empty() && !request.open && !request.close && !request.edit && !request.commit && !request.cancel && !request.callbacks && !request.stage){if(op.buffer)gui_.control_notification_buffers.push_back(op.buffer);complete_call(state,memory,request.return_value);return true;}
        if(gui_.control_operations.size()>=128)throw HostCallFault("日期控制項回呼超過上限");if(!op.buffer)storage();const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(op));
        try{start_callbacks(state,memory,{},Continuation::GuiControlOperation,token);}catch(...){discard_gui_control(token);throw;}return true;
    }
    if(cls==u"systabcontrol32" && begin_gui_tab(state,memory,message,wide))return true;
    if(cls==u"sysheader32" && begin_gui_header(state,memory,message,wide))return true;
    if(cls==u"systreeview32" && begin_gui_tree(state,memory,message,wide))return true;
    if(cls==u"systreeview32" && begin_gui_tree_selection(state,memory,message,wide))return true;
    if(cls==u"tooltips_class32" && begin_gui_tooltip(state,memory,message,wide))return true;
    if(cls==u"msctls_statusbar32" && message.message==0x409){
        const bool changed=window->second.status_simple!=(message.wparam!=0);const auto parent=window->second.parent,id=window->second.id;
        (void)gui_control_message(memory,message,wide);
        if(!changed || !gui_.windows.contains(parent)){complete_call(state,memory,0);return true;}
        const auto buffer=allocate_heap(memory,header);if(!buffer){complete_call(state,memory,0);return true;}
        try{memory.write(GuestAddress{buffer},std::vector<std::uint8_t>(header));memory.store(GuestAddress{buffer},abi.bits,message.window);memory.store(GuestAddress{buffer+abi.unit},abi.bits,id);memory.store(GuestAddress{buffer+abi.unit*2},32,static_cast<std::uint32_t>(-880));
            start_callbacks(state,memory,{{resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value,{parent,0x4e,id,buffer}}},Continuation::ReturnCallbackFreeHeap,buffer);
        }catch(...){(void)free_heap(memory,buffer,state);throw;}return true;
    }
    if(cls==u"msctls_statusbar32" && message.message==15){
        auto& control=window->second;
        std::erase_if(gui_.queue,[&](const auto& queued){return queued.window==message.window && queued.message==15;});
        if(!control.redraw || std::any_of(gui_.control_operations.begin(),gui_.control_operations.end(),[&](const auto& op){return op.second.owner_draw && op.second.message.window==message.window;})){complete_call(state,memory,0);return true;}
        if(gui_.control_operations.size()>=128)throw HostCallFault("狀態列自繪回呼超過上限");
        control.invalid=false;control.drawing.clear();const auto dc=0x10000000+message.window;
        if(native_gui_){rasters_.erase(dc);native_gui_->graphics_surface(message.window,{});present_gui(message.window);
            if(!control.status_simple && std::any_of(control.status_parts.begin(),control.status_parts.end(),[](const auto& part){return (part.flags&0x1000U)!=0;})){
                const auto background=native_gui_->capture_client(message.window);gui_.contexts.try_emplace(dc);
                if(background)if(const auto surface=raster_surface(memory,dc))surface->upload(background->pixels);
            }
        }
        GuiControlOperation operation;operation.message=message;operation.thread=threads_.current;operation.wide=wide;operation.owner_draw=true;
        if(!control.status_simple)for(auto& part:control.status_parts){if(!part.identity)part.identity=gui_.next_item++;operation.identities.push_back(part.identity);}
        auto& saved=gui_.saved_contexts[dc];saved.push_back(gui_.contexts[dc]);operation.parameter=saved.size();
        if(gui_.control_notification_buffers.empty())operation.buffer=runtime_allocate(memory,{},display_bytes);else {operation.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}
        const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(operation));
        try{start_callbacks(state,memory,{},Continuation::GuiControlOperation,token);}catch(...){discard_gui_control(token);throw;}return true;
    }
    if(cls==u"edit" && (gui_.clipboard_ole_object || gui_.clipboard_owner || !gui_.clipboard.empty()) && (message.message==0x301 || message.message==0x300 || (message.message==0x102 && (message.wparam==3 || message.wparam==24)))){
        const auto provider=gui_.clipboard_ole_object,thread=gui_.clipboard_ole_thread,owner=gui_.clipboard_owner;
        const auto sequence=gui_.clipboard_sequence;
        (void)gui_edit_input(memory,message,wide);
        if(gui_.clipboard_sequence!=sequence && gui_.clipboard_stale && gui_.clipboard_owner==message.window){gui_.clipboard_ole_object=gui_.clipboard_ole_thread=0;release_gui_clipboard(state,memory);
            std::vector<GuestCallback> callbacks;if(provider)callbacks.push_back({resolve("internal-com-proxy","ReleaseAt").value,{thread,provider}});
            if(owner && gui_.windows.contains(owner))callbacks.push_back({resolve("user32.dll","SendMessageW").value,{owner,0x307,0,0}});
            start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return true;}
        complete_call(state,memory,0);return true;
    }
    if(cls==u"edit" && (message.message==0x302 || (message.message==0x102 && message.wparam==22)) && (gui_.clipboard_ole_object || gui_.clipboard_owner) &&
       ((!gui_.clipboard.contains(13) || !gui_.clipboard.at(13)) && !gui_.clipboard_native_text)){
        if((job_ui_restrictions()&2U) || !window->second.enabled || (window->second.style&0x800U) || (gui_.clipboard_open_thread && gui_.clipboard_open_thread!=threads_.current)){complete_call(state,memory,0);return true;}
        if(gui_.control_operations.size()>=128)throw HostCallFault("Edit 剪貼簿操作超過上限");
        GuiControlOperation operation;operation.message=message;operation.thread=threads_.current;operation.wide=wide;operation.edit_paste=true;
        operation.parameter=gui_.clipboard.contains(13)?13:1;operation.phase=gui_.clipboard_open_thread?1:0;
        const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(operation));
        if(gui_.clipboard_open_thread)start_callbacks(state,memory,{{resolve("user32.dll","GetClipboardData").value,{gui_.control_operations.at(token).parameter}}},Continuation::GuiControlOperation,token);
        else start_callbacks(state,memory,{{resolve("user32.dll","OpenClipboard").value,{message.window}}},Continuation::GuiControlOperation,token);
        return true;
    }
    if(cls==u"button" && message.message==0xf5){
        if(!window->second.enabled || (window->second.style&15U)==7){complete_call(state,memory,0);return true;}
        if(gui_.control_operations.size()>=128)throw HostCallFault("控制項 callback 操作超過上限");
        GuiControlOperation operation;operation.message=message;operation.thread=threads_.current;operation.wide=wide;operation.button_click=true;
        const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(operation));
        try{const auto send=resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value;
            // 各 phase 重新經 SendMessage 查 WNDPROC；subclass 可攔截或在 down 內銷毀控制項。
            start_callbacks(state,memory,{{send,{message.window,0x201,0,0}},{send,{message.window,0x202,0,0}}},Continuation::GuiControlOperation,token);
        }catch(...){discard_gui_control(token);throw;}return true;
    }
    if(cls==u"button" && (message.message==0x202 || (message.message==0x101 && message.wparam==32))){
        const auto& button=window->second;const auto x=std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(message.lparam)),y=std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(message.lparam>>16));
        const bool inside=message.message!=0x202 || (x>=0 && y>=0 && static_cast<std::uint32_t>(x)<button.width && static_cast<std::uint32_t>(y)<button.height);
        const bool click=button.enabled && button.button_armed && button.button_pressed && gui_.capture==message.window && inside;
        (void)gui_control_message(memory,message,wide);
        if(click)if(const auto notify=complete_gui_button_click(message.window)){
            start_callbacks(state,memory,{{resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value,{notify->window,notify->message,notify->wparam,notify->lparam}}},Continuation::Return,0);return true;
        }
        complete_call(state,memory,0);return true;
    }
    if(((cls==u"button" && (window->second.style&15U)==11) || (cls==u"static" && (window->second.style&31U)==13)) && message.message==15){
        auto& button=window->second;
        std::erase_if(gui_.queue,[&](const auto& queued){return queued.window==message.window && queued.message==15;});
        if(!button.redraw || !gui_.windows.contains(button.parent)){complete_call(state,memory,0);return true;}
        if(gui_.control_operations.size()>=128)throw HostCallFault("控制項 callback 操作超過上限");
        const auto parent=button.parent,id=button.id,dc=0x10000000+message.window;
        const auto token=gui_.next_object++;
        GuiControlOperation operation;operation.message=message;operation.thread=threads_.current;operation.wide=wide;operation.owner_draw=true;
        gui_.control_operations.emplace(token,std::move(operation));
        try{
            auto& active=gui_.control_operations.at(token);
            gui_.control_notification_buffers.reserve(gui_.control_notification_buffers.size()+gui_.control_operations.size());
            // 與通知共用 bounded scratch pool；nested callback 各借一塊，返回後才歸還。
            if(gui_.control_notification_buffers.empty())active.buffer=runtime_allocate(memory,{},display_bytes);
            else {active.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}
            const auto buffer=active.buffer;memory.write(GuestAddress{buffer},std::array<std::uint8_t,64>{});
            memory.store(GuestAddress{buffer},32,cls==u"button"?4U:5U);memory.store(GuestAddress{buffer+4},32,id);
            memory.store(GuestAddress{buffer+12},32,1); // ODA_DRAWENTIRE：狀態改變時重畫整個 client。
            memory.store(GuestAddress{buffer+16},32,(button.button_pressed?1U:0U)|(!button.enabled?4U:0U)|(gui_.focus==message.window?16U:0U));
            const auto item_window=x86_?20U:24U,rectangle=item_window+abi.unit*2;
            memory.store(GuestAddress{buffer+item_window},abi.bits,message.window);memory.store(GuestAddress{buffer+item_window+abi.unit},abi.bits,dc);
            memory.store(GuestAddress{buffer+rectangle+8},32,button.width);memory.store(GuestAddress{buffer+rectangle+12},32,button.height);
            auto& context=gui_.contexts[dc];auto& saved=gui_.saved_contexts[dc];saved.push_back(context);active.parameter=saved.size();context.font=button.font;context.clipped=true;context.clip={0,0,static_cast<std::int32_t>(button.width),static_cast<std::int32_t>(button.height)};context.excluded.clear();
            button.invalid=false;button.drawing.clear();
            start_callbacks(state,memory,{{resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value,{parent,0x2b,id,buffer}}},Continuation::GuiControlOperation,token);
        }catch(...){if(const auto live=gui_.windows.find(message.window);live!=gui_.windows.end())live->second.invalid=true;discard_gui_control(token);throw;}
        return true;
    }
    if(message.message==0x4e && cls==u"syslistview32" && gui_.windows.contains(window->second.parent)){
        const auto source=memory.load(GuestAddress{message.lparam},abi.bits);const auto code=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{message.lparam+abi.notify_code},32)));
        if(source==window->second.list_header && (code==-302 || code==-322)){
            const auto index=static_cast<std::uint32_t>(memory.load(GuestAddress{message.lparam+header},32));const auto buffer=allocate_heap(memory,x86_?44:64);if(!buffer){complete_call(state,memory,0);return true;}
            memory.write(GuestAddress{buffer},std::vector<std::uint8_t>(x86_?44:64));memory.store(GuestAddress{buffer},abi.bits,message.window);memory.store(GuestAddress{buffer+abi.unit},abi.bits,window->second.id);memory.store(GuestAddress{buffer+abi.notify_code},32,static_cast<std::uint32_t>(-108));
            memory.store(GuestAddress{buffer+header},32,UINT32_MAX);memory.store(GuestAddress{buffer+header+4},32,index<window->second.columns.size()?static_cast<std::uint32_t>(window->second.columns[index].subitem):index);
            try{start_callbacks(state,memory,{{resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value,{window->second.parent,0x4e,window->second.id,buffer}}},Continuation::ReturnCallbackFreeHeap,buffer);}catch(...){(void)free_heap(memory,buffer,state);throw;}return true;
        }
        start_callbacks(state,memory,{{resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value,{window->second.parent,message.message,message.wparam,message.lparam}}},Continuation::ReturnCallback);return true;
    }
    if(message.message==0x4e && (cls==u"rebarwindow32" || cls==u"toolbarwindow32" || cls==u"systabcontrol32") && gui_.windows.contains(window->second.parent)){
        start_callbacks(state,memory,{{resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value,{window->second.parent,message.message,message.wparam,message.lparam}}},Continuation::ReturnCallback);return true;
    }
    if(cls==u"edit" && window->second.combo_owner && message.message==0x100 && (message.wparam==13 || message.wparam==27)){
        const auto owner=gui_.windows.find(window->second.combo_owner);if(owner==gui_.windows.end() || !gui_.windows.contains(owner->second.parent))return false;
        auto& combo=owner->second;
        if(std::any_of(gui_.control_operations.begin(),gui_.control_operations.end(),[&](const auto& entry){return entry.second.combo_end && entry.second.item==owner->first;}))throw HostCallFault("ComboBoxEx 結束編輯通知不得重入同一控制項");
        if(gui_.control_operations.size()>=128)throw HostCallFault("控制項 callback 操作超過上限");
        GuiControlOperation operation;operation.message=message;operation.thread=threads_.current;operation.wide=combo.list_unicode;operation.combo_end=true;operation.item=owner->first;operation.text=combo.title;operation.parameter=message.wparam==13?2:3;
        if(gui_.control_notification_buffers.empty())operation.buffer=runtime_allocate(memory,{},display_bytes);
        else {operation.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}
        const auto buffer=operation.buffer;
        memory.write(GuestAddress{buffer},std::array<std::uint8_t,display_bytes>{});
        memory.store(GuestAddress{buffer},abi.bits,owner->first);memory.store(GuestAddress{buffer+abi.unit},abi.bits,combo.id);
        memory.store(GuestAddress{buffer+abi.notify_code},32,static_cast<std::uint32_t>(combo.list_unicode?-806:-805));
        memory.store(GuestAddress{buffer+header},32,operation.parameter==2 && combo.combo_edit_changed?1U:0U);
        auto selection=combo.selection;
        if(operation.parameter==2 && (selection<0 || static_cast<std::size_t>(selection)>=combo.items.size() || combo.items[static_cast<std::size_t>(selection)].text!=combo.title))selection=-1;
        operation.subitem=static_cast<std::uint32_t>(selection);memory.store(GuestAddress{buffer+header+4},32,operation.subitem);
        if(combo.list_unicode)memory.write(GuestAddress{buffer+header+8},utf16_bytes(combo.title.substr(0,259)));
        else {auto text=utf16_to_utf8(combo.title);text.resize(std::min<std::size_t>(text.size(),259));std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);memory.write(GuestAddress{buffer+header+8},bytes);}
        memory.store(GuestAddress{buffer+header+(combo.list_unicode?528U:268U)},32,operation.parameter);
        combo.combo_edit_changed=false;
        const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(operation));
        try{start_callbacks(state,memory,{{resolve("user32.dll",combo.list_unicode?"SendMessageW":"SendMessageA").value,{combo.parent,0x4e,combo.id,buffer}}},Continuation::GuiControlOperation,token);}
        catch(...){discard_gui_control(token);throw;}return true;
    }
    if(cls!=u"syslistview32")return false;
    auto& view=window->second;const auto code=message.message;const bool sorting=code==0x1030 || code==0x1051;
    const bool getting=code==0x1005 || code==0x104b || code==0x102d || code==0x1073;
    if(!sorting && !getting)return false;
    GuiControlOperation operation;operation.message=message;operation.thread=threads_.current;operation.wide=wide;
    if(sorting){
        if((view.style&0x1000) || !message.lparam){complete_call(state,memory,0);return true;}
        if(std::any_of(gui_.control_operations.begin(),gui_.control_operations.end(),[&](const auto& entry){return entry.second.sort && entry.second.message.window==message.window;}))throw HostCallFault("ListView 排序期間不得重入同一列表排序");
        if(view.items.size()<2){complete_call(state,memory,1);return true;}
        if(!handles(GuestAddress{message.lparam}))memory.check(GuestAddress{message.lparam},1,Access::Execute);
        operation.sort=true;operation.sort_indices=code==0x1051;operation.callback=message.lparam;operation.parameter=message.wparam;
        operation.order.resize(view.items.size());std::iota(operation.order.begin(),operation.order.end(),0U);operation.output.resize(view.items.size());
        for(const auto& item:view.items)operation.identities.push_back(item.handle);
        operation.left=0;operation.right=1;operation.left_end=1;operation.right_end=std::min<std::size_t>(2,view.items.size());
    }else{
        memory.check(GuestAddress{message.lparam},abi.list_size,Access::Read);const bool text_only=code==0x102d || code==0x1073;
        const auto mask=text_only?1U:static_cast<std::uint32_t>(memory.load(GuestAddress{message.lparam},32));
        const auto row=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(text_only?message.wparam:memory.load(GuestAddress{message.lparam+4},32)));
        const auto subitem=static_cast<std::uint32_t>(memory.load(GuestAddress{message.lparam+8},32));
        if(row<0 || static_cast<std::size_t>(row)>=view.items.size() || subitem>=256)return false;
        const auto& item=view.items[static_cast<std::size_t>(row)];
        const bool text=(mask&1) && !(mask&0x800) && callback_text(item,subitem),image=(mask&2) && item.image==-1;
        if(!text && !image)return false;
        if(!gui_.windows.contains(view.parent))throw HostCallFault("ListView 顯示資料回呼缺少父視窗");
        if(gui_.control_notification_buffers.empty())operation.buffer=runtime_allocate(memory,{},display_bytes);
        else {operation.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}
        const auto buffer=operation.buffer;operation.item=item.handle;operation.subitem=subitem;
        memory.write(GuestAddress{buffer},std::array<std::uint8_t,display_bytes>{});
        memory.store(GuestAddress{buffer},abi.bits,message.window);memory.store(GuestAddress{buffer+abi.unit},abi.bits,view.id);
        memory.store(GuestAddress{buffer+abi.notify_code},32,static_cast<std::uint32_t>(view.list_unicode?-177:-150));
        memory.store(GuestAddress{buffer+header},32,(text?1U:0U)|(image?2U:0U));memory.store(GuestAddress{buffer+header+4},32,static_cast<std::uint32_t>(row));memory.store(GuestAddress{buffer+header+8},32,subitem);
        memory.store(GuestAddress{buffer+header+abi.list_text},abi.bits,buffer+112);memory.store(GuestAddress{buffer+header+abi.list_capacity},32,2048);
        memory.store(GuestAddress{buffer+header+abi.list_image},32,static_cast<std::uint32_t>(item.image));memory.store(GuestAddress{buffer+header+abi.list_data},abi.bits,item.data);
        gui_.control_notifications.emplace(buffer,GuiControlNotification{message.window,item.handle,subitem,true,view.list_unicode});
        operation.callback=resolve("user32.dll",view.list_unicode?"SendMessageW":"SendMessageA").value;
    }
    if(gui_.control_operations.size()>=128)throw HostCallFault("ListView callback 操作超過上限");
    const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(operation));
    try{
        const auto& active=gui_.control_operations.at(token);
        if(active.sort){CallbackFrame frame;frame.value=token;(void)resume_gui_control(state,memory,frame,0);}
        else start_callbacks(state,memory,{{active.callback,{view.parent,0x4e,view.id,active.buffer}}},Continuation::GuiControlOperation,token);
    }catch(...){discard_gui_control(token);throw;}
    return true;
}
bool WindowsServices::begin_gui_tree_selection(GuestState& state,GuestMemory& memory,const GuiMessage& message,bool wide){
    if((message.message!=0x110b || message.wparam!=9) && message.message!=0x10010 && message.message!=0x1101)return false;
    auto& window=gui_.windows.at(message.window);
    if(message.message==0x1101){
        GuiControlOperation operation;operation.message=message;operation.thread=threads_.current;operation.wide=wide;operation.tree_select=operation.tree_delete=true;
        std::map<std::uint64_t,std::vector<const GuiItem*>> children;std::map<std::uint64_t,const GuiItem*> index;
        for(const auto& item:window.items){children[item.parent].push_back(&item);index[item.handle]=&item;}
        const bool all=!message.lparam || static_cast<std::uint32_t>(message.lparam)==0xffff0000U;
        if(!all && !index.contains(message.lparam)){complete_call(state,memory,0);return true;}
        std::vector<std::pair<const GuiItem*,bool>> pending;
        if(all){for(auto it=children[0].rbegin();it!=children[0].rend();++it)pending.emplace_back(*it,false);}
        else pending.emplace_back(index.at(message.lparam),false);
        while(!pending.empty()){const auto [item,after]=pending.back();pending.pop_back();
            if(after){GuiItem removed;removed.handle=item->handle;removed.data=item->data;operation.removed_items.push_back(std::move(removed));continue;}
            pending.emplace_back(item,true);if(const auto descendants=children.find(item->handle);descendants!=children.end())for(auto it=descendants->second.rbegin();it!=descendants->second.rend();++it)pending.emplace_back(*it,false);
        }
        if(gui_.control_operations.size()>=128)throw HostCallFault("TreeView 刪除回呼超過上限");
        const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(operation));
        try{(void)gui_control_message(memory,message,wide);(void)resume_gui_tree_selection(state,memory,token,0);}catch(...){discard_gui_control(token);throw;}return true;
    }
    const auto target=std::find_if(window.items.begin(),window.items.end(),[&](const auto& item){return item.handle==message.lparam;});
    if(message.lparam && target==window.items.end()){complete_call(state,memory,0);return true;}
    const auto old=window.selection>=0 && static_cast<std::size_t>(window.selection)<window.items.size()?window.items[static_cast<std::size_t>(window.selection)].handle:0;
    if(old==message.lparam){present_gui(message.window);complete_call(state,memory,1);return true;}
    if(gui_.control_operations.size()>=128)throw HostCallFault("TreeView 選取回呼超過上限");
    GuiControlOperation operation;operation.message=message;operation.thread=threads_.current;operation.wide=wide;operation.tree_select=true;
    operation.item=message.lparam;operation.callback=old;operation.parameter=window.tree_revision;
    operation.subitem=message.message==0x10010?static_cast<std::uint32_t>(message.wparam):0U;
    const auto token=gui_.next_object++;gui_.control_operations.emplace(token,std::move(operation));
    try{(void)resume_gui_tree_selection(state,memory,token,0);}catch(...){discard_gui_control(token);throw;}return true;
}
std::optional<std::uint32_t> WindowsServices::resume_gui_tree_selection(GuestState& state,GuestMemory& memory,std::uint64_t token,std::uint64_t result){
    auto& operation=gui_.control_operations.at(token);const auto handle=operation.message.window;
    const auto finish=[&](std::uint64_t value)->std::optional<std::uint32_t>{discard_gui_control(token);if(gui_.windows.contains(handle))present_gui(handle);complete_call(state,memory,value);return {};};
    if(operation.phase==2)return finish(1);
    const auto live=gui_.windows.find(handle);if(live==gui_.windows.end())return finish(0);
    auto& window=live->second;
    if(!operation.tree_delete && (window.tree_revision!=operation.parameter || (operation.item && std::none_of(window.items.begin(),window.items.end(),[&](const auto& item){return item.handle==operation.item;}))))return finish(0);
    const GuiControlAbi abi(x86_);
    const auto notify=[&](std::int32_t code){
        if(!operation.buffer){if(gui_.control_notification_buffers.empty())operation.buffer=runtime_allocate(memory,{},display_bytes);else {operation.buffer=gui_.control_notification_buffers.back();gui_.control_notification_buffers.pop_back();}}
        const auto buffer=operation.buffer;memory.write(GuestAddress{buffer},std::array<std::uint8_t,152>{});
        memory.store(GuestAddress{buffer},abi.bits,handle);memory.store(GuestAddress{buffer+abi.unit},abi.bits,window.id);
        memory.store(GuestAddress{buffer+abi.notify_code},32,static_cast<std::uint32_t>(code));memory.store(GuestAddress{buffer+abi.notify_size},32,operation.subitem);
        const auto item_at=abi.notify_size+abi.unit;
        const auto put=[&](std::uint64_t at,std::uint64_t identity){const auto item=std::find_if(window.items.begin(),window.items.end(),[&](const auto& entry){return entry.handle==identity;});if(item==window.items.end())return;
            memory.store(GuestAddress{at},32,0x1c);memory.store(GuestAddress{at+abi.tree_handle},abi.bits,item->handle);
            memory.store(GuestAddress{at+abi.tree_state},32,item->state);memory.store(GuestAddress{at+abi.tree_mask},32,UINT32_MAX);memory.store(GuestAddress{at+abi.tree_data},abi.bits,item->data);
        };
        if(operation.tree_delete){const auto& removed=operation.removed_items.at(operation.start++);const auto at=buffer+item_at;
            memory.store(GuestAddress{at},32,0x14);memory.store(GuestAddress{at+abi.tree_handle},abi.bits,removed.handle);memory.store(GuestAddress{at+abi.tree_data},abi.bits,removed.data);
        }else {put(buffer+item_at,operation.callback);put(buffer+item_at+abi.tree_size,operation.item);}
        start_callbacks(state,memory,{{resolve("user32.dll",window.list_unicode?"SendMessageW":"SendMessageA").value,{window.parent,0x4e,window.id,buffer}}},Continuation::GuiControlOperation,token);
    };
    if(operation.tree_delete){if(operation.start>=operation.removed_items.size() || !gui_.windows.contains(window.parent))return finish(1);notify(window.list_unicode?-458:-409);return {};}
    if(!operation.phase){operation.phase=1;if(gui_.windows.contains(window.parent)){notify(window.list_unicode?-450:-401);return {};}}
    if(result)return finish(0);
    (void)gui_control_message(memory,{handle,9,operation.item,0x110b},operation.wide);operation.phase=2;
    if(gui_.windows.contains(window.parent)){notify(window.list_unicode?-451:-402);return {};}
    return finish(1);
}
void WindowsServices::discard_gui_control(std::uint64_t token){
    const auto found=gui_.control_operations.find(token);if(found==gui_.control_operations.end())return;
    if(found->second.clipboard_opened && gui_.clipboard_open_thread==found->second.thread){gui_.clipboard_open_thread=gui_.clipboard_open_window=0;if(gui_.clipboard_dirty)notify_clipboard();}
    if(found->second.calendar && found->second.calendar->callback_active){const auto window=gui_.windows.find(found->second.message.window);if(window!=gui_.windows.end() && window->second.calendar && window->second.calendar->callback_depth)--window->second.calendar->callback_depth;}
    if(found->second.header && found->second.header->filter_buffer)gui_.control_notification_buffers.push_back(found->second.header->filter_buffer);
    if(found->second.header && found->second.header->drag_image){const auto& paint=*found->second.header;
        rasters_.erase(paint.paint_bitmap);raster_shared_.erase(paint.paint_bitmap);gui_.bitmaps.erase(paint.paint_bitmap);gui_.contexts.erase(paint.paint_dc);gui_.dc_objects.erase(paint.paint_dc);gui_.saved_contexts.erase(paint.paint_dc);
    }
    if(found->second.header && found->second.header->kind==GuiControlOperation::Header::Kind::Paint && found->second.parameter){
        const auto& paint=*found->second.header;const auto window=gui_.windows.find(found->second.message.window);
        if(window!=gui_.windows.end()){window->second.text_color=paint.previous_text_color;window->second.background_mode=paint.previous_background_mode;}
        else {rasters_.erase(paint.paint_dc);raster_shared_.erase(paint.paint_dc);}
        if(paint.previous_dc_object_present)gui_.dc_objects[paint.paint_dc]=paint.previous_dc_object;else gui_.dc_objects.erase(paint.paint_dc);
    }
    if(found->second.owner_draw && found->second.parameter){const auto dc=0x10000000+found->second.message.window,depth=found->second.parameter;const auto saved=gui_.saved_contexts.find(dc);
        if(gui_.contexts.contains(dc) && saved!=gui_.saved_contexts.end() && saved->second.size()>=depth){gui_.contexts.at(dc)=saved->second[static_cast<std::size_t>(depth-1)];saved->second.resize(static_cast<std::size_t>(depth-1));}
    }
    if(found->second.buffer){if(found->second.combo_end || found->second.owner_draw || found->second.tab_select || found->second.tree_select || found->second.tree || found->second.tooltip || found->second.header || found->second.calendar || found->second.style)gui_.control_notification_buffers.push_back(found->second.buffer);else discard_gui_control_notify({found->second.message.window,0,found->second.buffer,0x4e});}
    gui_.control_operations.erase(found);
}
std::optional<std::uint32_t> WindowsServices::resume_gui_control(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t result){
    const auto found=gui_.control_operations.find(frame.value);if(found==gui_.control_operations.end())throw HostCallFault("ListView callback 缺少操作狀態");
    try{
    auto& operation=found->second;const auto window=gui_.windows.find(operation.message.window);
    if(operation.enable){
        auto& change=*operation.enable;const auto handle=operation.message.window;
        const auto finish=[&](std::uint64_t value){discard_gui_control(frame.value);complete_call(state,memory,value);};
        const auto again=[&](std::uint64_t target,std::uint32_t message,std::uint64_t parameter){start_callbacks(state,memory,{{resolve("user32.dll",operation.wide?"SendMessageW":"SendMessageA").value,{target,message,parameter,0}}},Continuation::GuiControlOperation,frame.value);};
        // WM_ENABLE 內可重入或銷毀 HWND；仍回傳本次真正提交前的狀態。
        if(operation.phase==3){finish(change.previous);return {};}
        if(window==gui_.windows.end() || window->second.destroying){last_error_=1400;finish(0);return {};}
        if(!operation.phase){operation.phase=1;if(!change.enabled){again(handle,0x1f,0);return {};}}
        if(operation.phase==1){
            change.previous=!window->second.enabled;
            if(window->second.enabled==change.enabled){finish(change.previous);return {};}
            gui_set_window_enabled(window->second,change.enabled);present_gui(handle);
            if(gui_control_class(window->second)==u"button" && (window->second.style&15U)==11)invalidate_window(handle);
            operation.phase=2;
            if(!change.enabled && gui_.focus==handle){
                // 系統停用可由不同 UI 執行緒發起；不能用呼叫者佇列的 SetFocus。
                gui_.focus=0;if(native_gui_)native_gui_->focus(0);
                again(handle,8,0);return {};
            }
        }
        operation.phase=3;again(handle,0xa,change.enabled);return {};
    }
    if(operation.style){
        auto& change=*operation.style;
        const auto finish=[&](std::uint64_t value){discard_gui_control(frame.value);complete_call(state,memory,value);};
        if(change.committed){finish(change.previous);return {};}
        if(window==gui_.windows.end() || window->second.destroying){last_error_=1400;finish(0);return {};}
        auto& control=window->second;
        auto style=static_cast<std::uint32_t>(memory.load(GuestAddress{operation.buffer+4},32));
        // 重入期間可能已有另一項變更；回傳真正提交前的值，不採用可改寫的 styleOld。
        change.previous=change.index==-16?control.style:control.ex_style;
        if(change.index==-16){
            if(!control.parent)style|=0x4000000U; // 頂層視窗保留 WS_CLIPSIBLINGS。
            if(control.style&0x20000000U)style|=0x20000000U; // 最小化狀態經 ShowWindow 還原。
            control.style=style;control.visible=(style&0x10000000U)!=0;gui_set_window_enabled(control,(style&0x8000000U)==0);
            if(gui_control_class(control)==u"msctls_progress32")control.progress_position=control.progress_min;
        }else {
            style=(style&~8U)|(control.ex_style&8U); // TOPMOST 經 SetWindowPos 改變。
            control.ex_style=style;
            if(!(style&0x80000U)){clear_layered_content(operation.message.window);control.layered_initialized=false;control.layered_alpha=255;control.layered_color=0;if(native_gui_)native_gui_->set_opacity(operation.message.window,255);}
        }
        change.committed=true;
        memory.store(GuestAddress{operation.buffer},32,change.previous);memory.store(GuestAddress{operation.buffer+4},32,style);
        present_gui(operation.message.window);
        start_callbacks(state,memory,{{resolve("user32.dll",operation.wide?"SendMessageW":"SendMessageA").value,{operation.message.window,0x7d,static_cast<std::uint64_t>(static_cast<std::int64_t>(change.index)),operation.buffer}}},Continuation::GuiControlOperation,frame.value);
        return {};
    }
    if(operation.calendar){
        auto& request=*operation.calendar;const auto finish=[&]()->std::optional<std::uint32_t>{const auto value=request.return_value;if(operation.message.message==0x100f && gui_.windows.contains(operation.message.window))(void)gui_control_message(memory,operation.message,operation.wide);if(request.text_buffer){(void)free_heap(memory,request.text_buffer,state);request.text_buffer=0;}if(request.locale_buffer){(void)free_heap(memory,request.locale_buffer,state);request.locale_buffer=0;}discard_gui_control(frame.value);complete_call(state,memory,value);return {};};
        if(window==gui_.windows.end()){
            // 建立月曆的回呼中父視窗已被刪除，回收剛建立的真正 HWND。
            if((request.stage==2 || request.stage==19 || request.stage==26) && result && gui_.windows.contains(result)){request.stage=5;start_callbacks(state,memory,{{resolve("user32.dll","DestroyWindow").value,{result}}},Continuation::GuiControlOperation,frame.value);return {};}
            return finish();
        }
        auto& control=window->second;const GuiControlAbi abi(x86_);
        if(request.stage==7 || request.stage==30){if(request.locale_buffer){if(request.locale_revision==gui_.calendar_locale_revision){auto locale=gui_calendar_read_locale(memory,request.locale_buffer);if(!gui_.calendar_locale || *gui_.calendar_locale!=*locale)gui_.calendar_locale=std::move(locale);}(void)free_heap(memory,request.locale_buffer,state);request.locale_buffer=0;}}
        if(request.stage==30){gui_calendar_apply_locale(*control.calendar,gui_.calendar_locale);present_gui(operation.message.window);gui_date_capture_callbacks(control,request,0x30,control.calendar->picker_revision);request.notices=std::move(control.calendar->notices);control.calendar->notices.clear();request.stage=4;
            if(const auto child=gui_.windows.find(control.calendar->month_window);child!=gui_.windows.end() && child->second.calendar){gui_calendar_apply_locale(*child->second.calendar,gui_.calendar_locale);present_gui(child->first);const auto& month=*child->second.calendar;const auto insets=gui_frame_insets(child->second);request.resize_window=child->first;request.resize_width=month.minimum_width+insets.left+insets.right;request.resize_height=month.minimum_height+insets.top+insets.bottom;request.stage=15;}
        }
        if(request.stage==7 || request.stage==9){GuiSystemTime seed{};for(unsigned n=0;n<8;++n)seed[n]=static_cast<std::uint16_t>(memory.load(GuestAddress{operation.buffer+64+n*2},16));
            if(request.stage==9){control.calendar->today=gui_date_normalize(seed,true);present_gui(operation.message.window);return finish();}
            gui_calendar_initialize(control,seed);
            gui_calendar_apply_locale(*control.calendar,gui_.calendar_locale);
            control.list_unicode=control.unicode;request.stage=8;
            const auto parent=control.parent?control.parent:control.owner;if(gui_.windows.contains(parent)){start_callbacks(state,memory,{{resolve("user32.dll","SendMessageW").value,{parent,0x55,operation.message.window,3}}},Continuation::GuiControlOperation,frame.value);return {};}
            result=0;
        }
        if(request.stage==8){if(result==1 || result==2)control.list_unicode=result==2;request.return_value=gui_control_message(memory,operation.message,operation.wide).value_or(0);request.notices=std::move(control.calendar->notices);control.calendar->notices.clear();gui_date_capture_callbacks(control,request,1,control.calendar->picker_revision);request.stage=control.calendar->picker && (control.style&1U) && !control.calendar->updown_window?25U:4U;}
        auto& date=*control.calendar;
        const auto again=[&](std::vector<GuestCallback> callbacks){start_callbacks(state,memory,std::move(callbacks),Continuation::GuiControlOperation,frame.value);};
        const auto text_memory=[&](const std::vector<std::uint8_t>& bytes){if(request.text_buffer)(void)free_heap(memory,request.text_buffer,state);request.text_buffer=allocate_heap(memory,bytes.size());if(!request.text_buffer)throw HostCallFault("日期編輯文字配置失敗");memory.write(GuestAddress{request.text_buffer},bytes);return request.text_buffer;};
        const auto callback_text=[&](std::u16string_view value){if(request.callback_wide)return utf16_bytes(value);const auto text=utf16_to_utf8(value);std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);return bytes;};
        if(request.stage==27){
            if(date.callback_revision==request.callback_revision && date.picker_revision==request.callback_value_revision){(void)gui_control_message(memory,{operation.message.window,0,operation.buffer+(x86_?20U:40U),0x1002},operation.wide);request.callback_value_revision=date.picker_revision;auto notices=std::move(date.notices);date.notices.clear();request.notices.insert(request.notices.end(),notices.begin(),notices.end());}
            request.stage=4;
        }
        if(request.stage==28 || request.stage==29){
            if(date.callback_revision==request.callback_revision && date.picker_revision==request.callback_value_revision && request.callback_next<date.callback_fields.size()){
                auto& field=date.callback_fields[request.callback_next];
                if(request.stage==28){for(unsigned n=0;n<2;++n){const auto value=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{operation.buffer+abi.notify_size+abi.unit+n*4},32)));field.maximum[n]=std::max(0,value);}request.callback_query_done=true;}
                else {const auto display=operation.buffer+abi.notify_size+abi.unit+16;const auto pointer=memory.load(GuestAddress{display},abi.bits);const auto limit=pointer==display+abi.unit?64U:65535U;field.text=pointer?(request.callback_wide?read_utf16(memory,GuestAddress{pointer},limit):utf8_to_utf16_lossy(read_ansi(memory,pointer,limit))):std::u16string{};++request.callback_next;request.callback_query_done=false;}
            }
            request.stage=4;
        }
        const auto configure_editor=[&](){const auto editor=date.edit_window;if(!gui_.windows.contains(editor)){request.stage=4;return false;}auto& child=gui_.windows.at(editor);child.calendar_owner=operation.message.window;child.font=control.font;date.editing=true;++date.edit_generation;present_gui(operation.message.window);request.stage=20;
            again({{resolve("user32.dll","SetWindowTextW").value,{editor,text_memory(utf16_bytes(operation.text))}},{resolve("user32.dll","SendMessageW").value,{editor,0x30,control.font,1}},{resolve("user32.dll","ShowWindow").value,{editor,5}},{resolve("user32.dll","SetFocus").value,{editor}},{resolve("user32.dll","SendMessageW").value,{editor,0xb1,0,UINT64_MAX}}});return true;};
        if(request.stage==25){memory.write(GuestAddress{operation.buffer+128},utf16_bytes(u"msctls_updown32"));request.stage=26;again({{resolve("user32.dll","CreateWindowExW").value,{0,operation.buffer+128,0,0x50000024U,control.width>20?control.width-20:0,0,20,control.height,operation.message.window,0,control.instance,0}}});return {};}
        if(request.stage==26){if(result && gui_.windows.contains(result)){date.updown_window=result;gui_.windows.at(result).calendar_owner=operation.message.window;present_gui(operation.message.window);}request.stage=4;}
        if(request.stage==19){if(result && gui_.windows.contains(result)){date.edit_window=result;if(configure_editor())return {};}return finish();}
        if(request.stage==20)return finish();
        if(request.stage==21){
            if(date.edit_generation!=request.edit_generation){request.stage=4;}
            else {
            if(date.edit_generation==request.edit_generation && date.picker_revision==request.revision){const auto flags=static_cast<std::uint32_t>(memory.load(GuestAddress{operation.buffer+abi.notify_size+abi.unit+16},32));const auto previous=date.picker_revision;(void)gui_control_message(memory,{operation.message.window,flags,operation.buffer+abi.notify_size+abi.unit,0x1002},operation.wide);gui_date_capture_callbacks(control,request,0x1002,previous);}
            const auto notices=std::move(date.notices);date.notices.clear();request.notices.insert(request.notices.end(),notices.begin(),notices.end());present_gui(operation.message.window);request.stage=22;
            if(gui_.focus==date.edit_window){again({{resolve("user32.dll","SetFocus").value,{operation.message.window}}});return {};}
            }
        }
        if(request.stage==22){request.stage=4;if(gui_.windows.contains(date.edit_window)){again({{resolve("user32.dll","ShowWindow").value,{date.edit_window,0}}});return {};}}
        if(request.stage==17){
            const auto& sent=request.notices.at(request.next-1);
            if(date.day_state_revision==sent.revision && date.day_states.size()==sent.flags && (control.style&1U) && !date.view){const auto pointer=operation.buffer+(x86_?32U:48U);const auto states=memory.load(GuestAddress{pointer},abi.bits);if(states){memory.check(GuestAddress{states},std::uint64_t(sent.flags)*4,Access::Read);std::vector<std::uint32_t> values;values.reserve(sent.flags);for(unsigned n=0;n<sent.flags;++n)values.push_back(static_cast<std::uint32_t>(memory.load(GuestAddress{states+n*4},32)));date.day_states=std::move(values);present_gui(operation.message.window);}}
            request.stage=4;
        }
        if(request.stage==15){request.stage=16;again({{resolve("user32.dll","SetWindowPos").value,{request.resize_window,0,0,0,request.resize_width,request.resize_height,0x216}}});return {};}
        if(request.stage==16)request.stage=4;
        if(request.stage==12){request.stage=13;again({{resolve("user32.dll","SetFocus").value,{operation.message.window}}});return {};}
        if(request.stage==13){request.stage=4;present_gui(operation.message.window);}
        if(request.stage==14)request.stage=4;
        if(!request.stage){
            if(request.edit && !date.editing && (control.style&0x10U)){
                if(gui_.windows.contains(date.edit_window)){if(configure_editor())return {};}
                else {memory.write(GuestAddress{operation.buffer+128},utf16_bytes(u"Edit"));request.stage=19;again({{resolve("user32.dll","CreateWindowExW").value,{0,operation.buffer+128,text_memory(utf16_bytes(operation.text)),0x50000080U,(control.style&2U)?24U:5U,2,control.width>50?control.width-((control.style&2U)?45U:26U):1U,control.height>4?control.height-4:1U,operation.message.window,1,control.instance,0}}});return {};}
            }
            if((request.commit || request.cancel) && date.editing && date.edit_generation==request.edit_generation){date.editing=false;present_gui(operation.message.window);
                if(request.commit){const auto parent=control.parent?control.parent:control.owner;if(gui_.windows.contains(parent)){const auto bytes=control.list_unicode?utf16_bytes(operation.text):[&]{const auto value=utf16_to_utf8(operation.text);std::vector<std::uint8_t> result(value.begin(),value.end());result.push_back(0);return result;}();const auto text=text_memory(bytes);memory.write(GuestAddress{operation.buffer},std::vector<std::uint8_t>(gui_control_notification_bytes));memory.store(GuestAddress{operation.buffer},abi.bits,operation.message.window);memory.store(GuestAddress{operation.buffer+abi.unit},abi.bits,control.id);memory.store(GuestAddress{operation.buffer+abi.notify_code},32,static_cast<std::uint32_t>(control.list_unicode?-745:-758));memory.store(GuestAddress{operation.buffer+abi.notify_size},abi.bits,text);gui_date_write(memory,operation.buffer+abi.notify_size+abi.unit,date.value);memory.store(GuestAddress{operation.buffer+abi.notify_size+abi.unit+16},32,date.none?1U:0U);request.stage=21;again({{resolve("user32.dll",control.list_unicode?"SendMessageW":"SendMessageA").value,{parent,0x4e,control.id,operation.buffer}}});return {};}}
                request.stage=22;if(gui_.focus==date.edit_window){again({{resolve("user32.dll","SetFocus").value,{operation.message.window}}});return {};}
                if(gui_.windows.contains(date.edit_window)){request.stage=4;again({{resolve("user32.dll","ShowWindow").value,{date.edit_window,0}}});return {};}request.stage=4;
            }
            if(!date.picker && request.close && date.picker_owner){request.stage=6;again({{resolve("user32.dll","SendMessageW").value,{date.picker_owner,0x100d,0,0}}});return {};}
            if(request.open && !date.open){request.stage=1;again({{resolve("user32.dll","GetWindowRect").value,{operation.message.window,operation.buffer+96}}});return {};}
            if(request.close && date.open){request.popup=date.month_window;date.month_window=0;date.open=false;request.stage=3;present_gui(operation.message.window);
                if(gui_.windows.contains(request.popup)){again({{resolve("user32.dll","DestroyWindow").value,{request.popup}}});return {};}}
            else request.stage=4;
        }
        if(request.stage==1){
            if(!result)return finish();GuiWindow month=control;month.style=0x90800000U|date.month_style;month.ex_style=0x08000080;month.frame_insets.reset();month.calendar->picker=false;month.width=month.calendar->minimum_width;month.height=month.calendar->minimum_height;
            month.control_font=gui_font(gui_,date.month_font?date.month_font:control.font);gui_calendar_project(month,native_gui_.get());const auto insets=gui_frame_insets(month);
            const auto extent=month.calendar->minimum_width+insets.left+insets.right;const auto left=(control.style&0x20U)?static_cast<std::uint32_t>(memory.load(GuestAddress{operation.buffer+104},32)-extent):static_cast<std::uint32_t>(memory.load(GuestAddress{operation.buffer+96},32));const auto bottom=memory.load(GuestAddress{operation.buffer+108},32);memory.write(GuestAddress{operation.buffer+128},utf16_bytes(u"SysMonthCal32"));request.stage=2;
            again({{resolve("user32.dll","CreateWindowExW").value,{0x08000080,operation.buffer+128,0,0x90800000U|date.month_style,left,bottom,month.calendar->minimum_width+insets.left+insets.right,month.calendar->minimum_height+insets.top+insets.bottom,operation.message.window,0,control.instance,0}}});return {};
        }
        if(request.stage==2){
            if(!result || !gui_.windows.contains(result))return finish();auto& month=gui_.windows.at(result);month.font=date.month_font?date.month_font:control.font;month.calendar=date;auto& child=*month.calendar;
            child.picker=false;child.picker_owner=operation.message.window;child.open=child.open_request=child.close_request=false;child.month_window=child.edit_window=child.updown_window=0;child.editing=child.edit_request=child.commit_request=child.cancel_request=false;child.callback_fields.clear();child.callback_schema.clear();child.callback_depth=child.callback_key=0;child.none=false;child.value=gui_date_normalize(date.value,true);child.last=child.value;child.minimum=(date.range_flags&1U)?gui_date_normalize(date.minimum,true):GuiSystemTime{};child.maximum=(date.range_flags&2U)?gui_date_normalize(date.maximum,true):GuiSystemTime{};child.notices.clear();
            date.open=true;date.month_window=result;request.notices.push_back({-754});request.stage=14;present_gui(result);present_gui(operation.message.window);again({{resolve("user32.dll","SetFocus").value,{result}}});return {};
        }
        if(request.stage==3){request.notices.push_back({-753});request.stage=4;if(!gui_.focus || gui_.focus==request.popup){request.stage=10;again({{resolve("user32.dll","SetFocus").value,{operation.message.window}}});return {};}}
        if(request.stage==10)request.stage=4;
        if(request.stage==5 || request.stage==6)return finish();
        if(request.stage==4){
            const auto parent=control.parent?control.parent:control.owner;
            if(request.callbacks){
                if(date.callback_revision!=request.callback_revision || date.picker_revision!=request.callback_value_revision || !gui_.windows.contains(parent))request.callbacks=false;
                else {
                    const auto header=[&](std::int32_t code,std::u16string_view format,std::uint32_t pointer){memory.write(GuestAddress{operation.buffer},std::vector<std::uint8_t>(gui_control_notification_bytes));memory.store(GuestAddress{operation.buffer},abi.bits,operation.message.window);memory.store(GuestAddress{operation.buffer+abi.unit},abi.bits,control.id);memory.store(GuestAddress{operation.buffer+abi.notify_code},32,static_cast<std::uint32_t>(code));memory.store(GuestAddress{operation.buffer+pointer},abi.bits,text_memory(callback_text(format)));};
                    const auto notify=[&](){again({{resolve("user32.dll",request.callback_wide?"SendMessageW":"SendMessageA").value,{parent,0x4e,control.id,operation.buffer}}});};
                    if(request.callback_key && request.callback_field<date.callback_fields.size()){header(request.callback_wide?-744:-757,date.callback_fields[request.callback_field].format,x86_?16U:32U);memory.store(GuestAddress{operation.buffer+abi.notify_size},32,request.callback_key);gui_date_write(memory,operation.buffer+(x86_?20U:40U),date.value);request.callback_key=0;request.stage=27;notify();return {};}
                    if(request.callback_next<date.callback_fields.size()){const auto& field=date.callback_fields[request.callback_next];
                        if(request.callback_query && !request.callback_query_done){header(request.callback_wide?-742:-755,field.format,abi.notify_size);request.stage=28;}
                        else {header(request.callback_wide?-743:-756,field.format,abi.notify_size);gui_date_write(memory,operation.buffer+abi.notify_size+abi.unit,date.value);const auto display=operation.buffer+abi.notify_size+abi.unit+16;memory.store(GuestAddress{display},abi.bits,display+abi.unit);request.stage=29;}
                        notify();return {};
                    }
                    if(request.callback_query)date.callback_query_dirty=false;date.callback_text_dirty=false;request.callbacks=false;present_gui(operation.message.window);
                }
            }
            while(request.next<request.notices.size()){
                const auto message=request.notices[request.next++];if(!gui_.windows.contains(parent))continue;memory.write(GuestAddress{operation.buffer},std::vector<std::uint8_t>(gui_control_notification_bytes));
                memory.store(GuestAddress{operation.buffer},abi.bits,operation.message.window);memory.store(GuestAddress{operation.buffer+abi.unit},abi.bits,control.id);memory.store(GuestAddress{operation.buffer+abi.unit*2},32,static_cast<std::uint32_t>(message.code));
                if(message.code==-759){memory.store(GuestAddress{operation.buffer+abi.notify_size},32,message.flags);gui_date_write(memory,operation.buffer+abi.notify_size+4,message.first);}
                if(message.code==-743){if(!(control.style&1U) || date.view || date.day_state_revision!=message.revision)continue;gui_date_write(memory,operation.buffer+abi.notify_size,message.first);memory.store(GuestAddress{operation.buffer+abi.notify_size+16},32,message.flags);memory.store(GuestAddress{operation.buffer+(x86_?32U:48U)},abi.bits,operation.buffer+256);request.stage=17;}
                if(message.code==-750){memory.store(GuestAddress{operation.buffer+abi.notify_size},32,message.old_view);memory.store(GuestAddress{operation.buffer+abi.notify_size+4},32,message.new_view);}
                if(message.code==-749 || message.code==-746){gui_date_write(memory,operation.buffer+abi.notify_size,message.first);gui_date_write(memory,operation.buffer+abi.notify_size+16,message.last);}
                again({{resolve("user32.dll",control.list_unicode?"SendMessageW":"SendMessageA").value,{parent,0x4e,control.id,operation.buffer}}});return {};
            }
        }
        return finish();
    }
    if(operation.header)return resume_gui_header(state,memory,frame.value,result);
    if(operation.tree)return resume_gui_tree(state,memory,frame.value,result);
    if(operation.tooltip)return resume_gui_tooltip(state,memory,frame.value,result);
    if(operation.tab_select)return resume_gui_tab(state,memory,frame.value,result);
    if(operation.tree_select)return resume_gui_tree_selection(state,memory,frame.value,result);
    if(operation.edit_paste){
        if(!operation.phase){
            if(!result){discard_gui_control(frame.value);complete_call(state,memory,0);return {};}
            operation.clipboard_opened=true;operation.phase=1;
            start_callbacks(state,memory,{{resolve("user32.dll","GetClipboardData").value,{operation.parameter}}},Continuation::GuiControlOperation,frame.value);return {};
        }
        if(operation.phase==1 && operation.clipboard_opened){operation.phase=2;
            start_callbacks(state,memory,{{resolve("user32.dll","CloseClipboard").value,{}}},Continuation::GuiControlOperation,frame.value);return {};
        }
        if(window!=gui_.windows.end())(void)gui_edit_input(memory,operation.message,operation.wide);
        discard_gui_control(frame.value);complete_call(state,memory,0);return {};
    }
    if(operation.button_click){discard_gui_control(frame.value);complete_call(state,memory,0);return std::nullopt;}
    if(operation.owner_draw){
        if(window!=gui_.windows.end() && gui_control_class(window->second)==u"msctls_statusbar32"){
            auto& control=window->second;const GuiControlAbi abi(x86_);const auto buffer=operation.buffer;
            while(!control.status_simple && gui_.windows.contains(control.parent) && operation.start<operation.identities.size()){
                const auto identity=operation.identities[operation.start++];const auto part=std::find_if(control.status_parts.begin(),control.status_parts.end(),[&](const auto& value){return value.identity==identity;});
                if(part==control.status_parts.end() || !(part->flags&0x1000U))continue;
                const auto index=static_cast<std::size_t>(part-control.status_parts.begin());const auto rect=gui_status_rectangle(control,index);
                if(rect[2]<=rect[0] || rect[3]<=0 || rect[0]>=static_cast<std::int32_t>(control.width) || rect[2]<=0)continue;
                memory.write(GuestAddress{buffer},std::array<std::uint8_t,64>{});memory.store(GuestAddress{buffer+4},32,control.id);memory.store(GuestAddress{buffer+8},32,index);
                const auto item_window=x86_?20U:24U,rectangle=item_window+abi.unit*2;const auto dc=0x10000000+operation.message.window;
                memory.store(GuestAddress{buffer+item_window},abi.bits,operation.message.window);memory.store(GuestAddress{buffer+item_window+abi.unit},abi.bits,dc);
                for(unsigned n=0;n<4;++n)memory.store(GuestAddress{buffer+rectangle+n*4},32,static_cast<std::uint32_t>(rect[n]));memory.store(GuestAddress{buffer+rectangle+16},abi.bits,part->data);
                auto& context=gui_.contexts[dc];context.font=control.font;context.clipped=true;context.clip=rect;context.excluded.clear();
                start_callbacks(state,memory,{{resolve("user32.dll",operation.wide?"SendMessageW":"SendMessageA").value,{control.parent,0x2b,control.id,buffer}}},Continuation::GuiControlOperation,frame.value);return std::nullopt;
            }
        }
        if(window!=gui_.windows.end() && window->second.tab && gui_control_class(window->second)==u"systabcontrol32"){
            auto& control=window->second;auto& tab=*control.tab;const GuiControlAbi abi(x86_);const auto buffer=operation.buffer;
            const auto notify=[&](std::uint32_t message){start_callbacks(state,memory,{{resolve("user32.dll",operation.wide?"SendMessageW":"SendMessageA").value,{control.parent,message,control.id,buffer}}},Continuation::GuiControlOperation,frame.value);};
            if(operation.message.message==1){
                if(!operation.pending && gui_.windows.contains(control.parent)){
                    operation.pending=true;operation.left=static_cast<std::size_t>(tab.item_width);operation.right=static_cast<std::size_t>(tab.item_height);
                    memory.write(GuestAddress{buffer},std::array<std::uint8_t,32>{});memory.store(GuestAddress{buffer},32,101);memory.store(GuestAddress{buffer+4},32,control.id);
                    memory.store(GuestAddress{buffer+12},32,96);memory.store(GuestAddress{buffer+16},32,static_cast<std::uint32_t>(std::max(1,fallback_font_metrics(control.control_font).height+10)));
                    notify(0x2c);return std::nullopt;
                }
                if(operation.pending && result){
                    // 回呼若使用 TCM_SETITEMSIZE，保留其明確更新，不以舊量測覆蓋。
                    if(tab.item_width==static_cast<std::int32_t>(operation.left))tab.item_width=static_cast<std::int32_t>(std::clamp<std::uint64_t>(memory.load(GuestAddress{buffer+12},32),1,65535));
                    if(tab.item_height==static_cast<std::int32_t>(operation.right))tab.item_height=static_cast<std::int32_t>(std::clamp<std::uint64_t>(memory.load(GuestAddress{buffer+16},32),1,65535));
                }
            }else if(gui_.windows.contains(control.parent) && (control.style&0x2000U)){
                while(operation.start<operation.identities.size()){
                    const auto identity=operation.identities[operation.start++];const auto item=std::find_if(tab.items.begin(),tab.items.end(),[&](const auto& value){return value.identity==identity;});
                    if(item==tab.items.end())continue;
                    const auto index=static_cast<std::size_t>(item-tab.items.begin());layout_gui_tab(control);const auto rect=tab.rectangles[index];
                    if(rect[2]<=0 || rect[3]<=0 || rect[0]>=static_cast<std::int32_t>(control.width) || rect[1]>=static_cast<std::int32_t>(control.height))continue;
                    memory.write(GuestAddress{buffer},std::array<std::uint8_t,64>{});memory.store(GuestAddress{buffer},32,101);memory.store(GuestAddress{buffer+4},32,control.id);memory.store(GuestAddress{buffer+8},32,index);memory.store(GuestAddress{buffer+12},32,1);
                    memory.store(GuestAddress{buffer+16},32,(control.selection==static_cast<std::int32_t>(index)?1U:0U)|(!control.enabled?4U:0U)|(tab.focused && tab.focus==static_cast<std::int32_t>(index)?16U:0U));
                    const auto item_window=x86_?20U:24U,rectangle=item_window+abi.unit*2;const auto dc=0x10000000+operation.message.window;
                    memory.store(GuestAddress{buffer+item_window},abi.bits,operation.message.window);memory.store(GuestAddress{buffer+item_window+abi.unit},abi.bits,dc);
                    for(unsigned i=0;i<4;++i)memory.store(GuestAddress{buffer+rectangle+i*4},32,static_cast<std::uint32_t>(rect[i]));
                    std::uint64_t data=0;
                    if(tab.extra_bytes==UINT32_MAX){for(std::size_t i=0;i<std::min<std::size_t>(abi.unit,item->data.size());++i)data|=static_cast<std::uint64_t>(item->data[i])<<(i*8);}
                    else {data=buffer+64;memory.write(GuestAddress{data},item->data);}
                    memory.store(GuestAddress{buffer+rectangle+16},abi.bits,data);
                    auto& context=gui_.contexts[dc];context.font=control.font;context.clipped=true;context.clip=rect;context.excluded.clear();
                    notify(0x2b);return std::nullopt;
                }
            }
        }
        const auto handle=operation.message.window;discard_gui_control(frame.value);
        if(gui_.windows.contains(handle)){present_gui(handle);++gui_.paints;}
        complete_call(state,memory,0);return std::nullopt;
    }
    if(operation.combo_end){
        const auto owner=gui_.windows.find(operation.item);
        if(owner!=gui_.windows.end()){
            auto& combo=owner->second;
            // 父視窗可以在通知中重新設定文字；不能以通知前的快照覆蓋客體的決定。
            if(combo.title==operation.text){
                if(result || operation.parameter==3)combo.title=combo.combo_edit_item.text;
                else {combo.selection=static_cast<std::int32_t>(operation.subitem);combo.combo_edit_item.text=combo.title;}
            }
            present_gui(operation.item);
        }
        discard_gui_control(frame.value);complete_call(state,memory,0);return std::nullopt;
    }
    if(window==gui_.windows.end()){discard_gui_control(frame.value);complete_call(state,memory,0);return std::nullopt;}
    auto& view=window->second;
    if(!operation.sort){
        const auto message=operation.message;const auto wide=operation.wide;const auto identity=operation.item;const auto subitem=operation.subitem;
        const auto actual=std::find_if(view.items.begin(),view.items.end(),[&](const auto& item){return item.handle==identity;});
        if(actual==view.items.end()){discard_gui_control(frame.value);complete_call(state,memory,0);return std::nullopt;}
        const auto expected=(message.message==0x102d || message.message==0x1073)?static_cast<std::uint32_t>(message.wparam):static_cast<std::uint32_t>(memory.load(GuestAddress{message.lparam+4},32));
        if(expected>=view.items.size() || view.items[expected].handle!=identity){discard_gui_control(frame.value);complete_call(state,memory,0);return std::nullopt;}
        const bool requested_text=(message.message==0x102d || message.message==0x1073) || (memory.load(GuestAddress{message.lparam},32)&1);
        finish_gui_control_notify(memory,{view.parent,view.id,operation.buffer,0x4e});operation.buffer=0;
        // 原生畫面可以保留本次快取；沒有 LVIF_DI_SETITEM 的同步查詢仍會重新呼叫客體。
        if(requested_text && callback_text(*actual,subitem) && !actual->resolved_subitems.contains(subitem))throw HostCallFault("ListView 顯示回呼沒有提供請求的文字");
        const auto value=gui_control_message(memory,message,wide).value_or(0);gui_.control_operations.erase(found);
        complete_call(state,memory,value);return std::nullopt;
    }
    if(view.items.size()!=operation.identities.size() || !std::equal(view.items.begin(),view.items.end(),operation.identities.begin(),[](const auto& item,auto id){return item.handle==id;})){
        discard_gui_control(frame.value);throw HostCallFault("ListView 比較回呼不得新增、移除或重新排序列表項目");
    }
    if(operation.pending){
        operation.output[operation.out++]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(result))<=0?operation.order[operation.left++]:operation.order[operation.right++];operation.pending=false;
    }
    const auto count=operation.order.size();
    for(;;){
        if(operation.left<operation.left_end && operation.right<operation.right_end){
            const auto left=operation.order[operation.left],right=operation.order[operation.right];
            const auto left_value=operation.sort_indices?left:view.items[left].data,right_value=operation.sort_indices?right:view.items[right].data;
            operation.pending=true;start_callbacks(state,memory,{{operation.callback,{left_value,right_value,operation.parameter}}},Continuation::GuiControlOperation,frame.value);return std::nullopt;
        }
        while(operation.left<operation.left_end)operation.output[operation.out++]=operation.order[operation.left++];
        while(operation.right<operation.right_end)operation.output[operation.out++]=operation.order[operation.right++];
        operation.start+=operation.width*2;
        if(operation.start>=count){
            operation.order.swap(operation.output);operation.width*=2;operation.start=0;
            if(operation.width>=count){
                const auto selected=view.selection>=0 && static_cast<std::size_t>(view.selection)<view.items.size()?view.items[static_cast<std::size_t>(view.selection)].handle:0;
                std::vector<GuiItem> sorted;sorted.reserve(count);for(const auto index:operation.order)sorted.push_back(std::move(view.items[index]));view.items=std::move(sorted);view.selection=-1;
                for(std::size_t i=0;i<view.items.size();++i)if(view.items[i].handle==selected)view.selection=static_cast<std::int32_t>(i);
                const auto handle=operation.message.window;gui_.control_operations.erase(found);present_gui(handle);complete_call(state,memory,1);return std::nullopt;
            }
        }
        operation.left=operation.start;operation.left_end=std::min(operation.start+operation.width,count);operation.right=operation.left_end;operation.right_end=std::min(operation.start+operation.width*2,count);operation.out=operation.start;
    }
    }catch(...){if(const auto live=gui_.control_operations.find(frame.value);live!=gui_.control_operations.end() && live->second.calendar){auto& request=*live->second.calendar;if(request.text_buffer){const auto text=request.text_buffer;request.text_buffer=0;(void)free_heap(memory,text,state);}if(request.locale_buffer){const auto locale=request.locale_buffer;request.locale_buffer=0;(void)free_heap(memory,locale,state);}}discard_gui_control(frame.value);throw;}
}
}
