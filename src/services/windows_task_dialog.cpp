#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <map>
#include <tuple>

namespace x64arm {
// comctl32 v6 TaskDialog／TaskDialogIndirect。
// 以 DLGTEMPLATE 組成標準對話框，經 DialogBoxIndirectParamW 與既有模態迴圈、原生控制項呈現；
// 內部對話框程序把 WM_INITDIALOG／WM_COMMAND／WM_TIMER 與 TDM_* 訊息轉為客體 TDN_* 回呼。
// 參考：https://learn.microsoft.com/windows/win32/api/commctrl/nf-commctrl-taskdialogindirect
struct TaskDialogState {
    struct Dialog {
        std::uint64_t window{},callback{},data{},template_address{};
        std::uint64_t button_out{},radio_out{},verification_out{};
        std::uint32_t flags{};
        std::vector<std::int32_t> buttons,radios;
        std::int32_t result{},radio{},pending{};
        bool verification{},ended{},cancel{};
        std::uint64_t timer_reset{};
    };
    std::map<std::uint64_t,Dialog> dialogs;
    std::uint64_t next{1};
    bool reported_navigation{};
};
namespace {
constexpr std::uint32_t failure=0x80004005U,invalid=0x80070057U,no_memory=0x8007000eU;
// TASKDIALOG_FLAGS。
constexpr std::uint32_t enable_hyperlinks=0x1,allow_cancellation=0x8,command_links=0x10,command_links_no_icon=0x20,
    expanded_by_default=0x80,verification_checked=0x100,progress_bar=0x200,marquee_progress=0x400,callback_timer=0x800,
    can_be_minimized=0x8000;
// TDN_*。
constexpr std::uint32_t tdn_created=0,tdn_button_clicked=2,tdn_timer=4,tdn_destroyed=5,tdn_radio_clicked=6,tdn_constructed=7,tdn_verification_clicked=8;
// 子控制項 ID；客體自訂按鈕與單選鈕沿用自己的 ID。
constexpr std::int32_t main_id=0xf001,content_id=0xf002,expanded_id=0xf003,footer_id=0xf004,progress_id=0xf010,verification_id=0xf020;
constexpr std::uint64_t timer_id=0x7d00;
constexpr std::int32_t cancel_button=2;
struct Config {
    std::uint64_t owner{},instance{},callback{},data{};
    std::uint32_t flags{},common{},width{};
    std::u16string title,main,content,verification,expanded,footer;
    std::vector<std::pair<std::int32_t,std::u16string>> buttons,radios;
    std::int32_t default_button{},default_radio{};
};
// TDF_ENABLE_HYPERLINKS 的 <a href="...">文字</a> 以純文字顯示。
std::u16string strip_links(const std::u16string& text){
    std::u16string out;
    for(std::size_t at=0;at<text.size();){
        if(text[at]==u'<' && at+1<text.size() && (text[at+1]==u'a' || text[at+1]==u'A' || text[at+1]==u'/')){
            const auto end=text.find(u'>',at);if(end==std::u16string::npos){out.append(text.substr(at));break;}
            at=end+1;continue;
        }
        out.push_back(text[at++]);
    }
    return out;
}
// Segoe UI 9pt 下平均字元寬 4 DLU、行高 8 DLU；以段落估計換行數。
std::int32_t text_height(const std::u16string& text,std::int32_t width){
    if(text.empty())return 0;
    const auto per_line=std::max<std::int32_t>(1,(width*9/10)/4);std::int32_t lines=0;
    for(std::size_t at=0;at<=text.size();){
        auto end=text.find(u'\n',at);if(end==std::u16string::npos)end=text.size();
        const auto length=static_cast<std::int32_t>(end-at);lines+=std::max<std::int32_t>(1,(length+per_line-1)/per_line);
        at=end+1;
    }
    return lines*9;
}
struct Template {
    std::vector<std::uint8_t> bytes;std::uint16_t count{};
    void word(std::uint32_t value){bytes.push_back(static_cast<std::uint8_t>(value));bytes.push_back(static_cast<std::uint8_t>(value>>8));}
    void dword(std::uint32_t value){word(value&0xffffU);word(value>>16);}
    void text(const std::u16string& value){for(const auto c:value)word(c);word(0);}
    void align(){while(bytes.size()%4)bytes.push_back(0);}
    void item(std::uint32_t style,std::int32_t x,std::int32_t y,std::int32_t cx,std::int32_t cy,std::int32_t id,std::uint16_t atom,const std::u16string& cls,const std::u16string& title){
        align();dword(0x50000000U|style);dword(0);word(static_cast<std::uint16_t>(x));word(static_cast<std::uint16_t>(y));
        word(static_cast<std::uint16_t>(cx));word(static_cast<std::uint16_t>(cy));word(static_cast<std::uint16_t>(id));
        if(atom){word(0xffff);word(atom);}else text(cls);
        text(title);word(0);++count;
    }
};
}
std::span<const WindowsServiceDescriptor> task_dialog_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"comctl32.dll","TaskDialog",8},{"comctl32.dll","TaskDialogIndirect",4},
        {"internal-taskdialog","DialogProc",4},{"internal-taskdialog","Finish",1},
    };
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_task_dialog(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(!task_dialogs_)task_dialogs_=std::make_shared<TaskDialogState>();
    else if(task_dialogs_.use_count()>1)task_dialogs_=std::make_shared<TaskDialogState>(*task_dialogs_);
    auto& bridge=*task_dialogs_;
    const auto load_string=[&](std::uint64_t instance,std::uint64_t id)->std::u16string{
        const auto entry=gui_resource(memory,instance,(id>>4)+1,6,true);
        const auto* image=resource_image(instance?instance:runtime_.image.base);if(!entry || !image)return {};
        auto at=image->base+memory.load(GuestAddress{entry},32);const auto end=at+memory.load(GuestAddress{entry+4},32);
        for(unsigned i=0;i<16 && at+2<=end;++i){
            const auto length=memory.load(GuestAddress{at},16);at+=2;if(length*2>end-at)return {};
            if(i==(id&15U))return read_utf16_counted(memory,GuestAddress{at},length);
            at+=length*2;
        }
        return {};
    };
    const auto text=[&](std::uint64_t instance,std::uint64_t pointer)->std::u16string{
        if(!pointer)return {};
        if(pointer<0x10000)return load_string(instance,pointer);
        return read_utf16(memory,GuestAddress{pointer},65536);
    };
    if(name=="TaskDialog" || name=="TaskDialogIndirect"){
        Config config;std::uint64_t button_out{},radio_out{},verification_out{};
        if(name=="TaskDialog"){
            config.owner=arg(0);config.instance=arg(1);config.common=static_cast<std::uint32_t>(arg(5));button_out=arg(7);
            config.title=text(config.instance,arg(2));config.main=text(config.instance,arg(3));config.content=text(config.instance,arg(4));
            config.flags=allow_cancellation;
        }else{
            const auto p=arg(0);if(!p)return finish(invalid);
            const auto size=memory.load(GuestAddress{p},32);if(size!=(x86_?96U:160U))return finish(invalid);
            memory.check(GuestAddress{p},size,Access::Read);
            // TASKDIALOGCONFIG 以 1-byte packing 宣告；欄位依序排列。
            std::uint64_t at=p+4;
            const auto pointer=[&]{const auto value=memory.load(GuestAddress{at},bits);at+=unit;return value;};
            const auto number=[&]{const auto value=memory.load(GuestAddress{at},32);at+=4;return value;};
            config.owner=pointer();config.instance=pointer();config.flags=static_cast<std::uint32_t>(number());config.common=static_cast<std::uint32_t>(number());
            const auto title=pointer();(void)pointer();const auto main=pointer(),content=pointer();
            const auto button_count=number();const auto buttons=pointer();config.default_button=static_cast<std::int32_t>(number());
            const auto radio_count=number();const auto radios=pointer();config.default_radio=static_cast<std::int32_t>(number());
            const auto verification=pointer(),expanded=pointer();(void)pointer();(void)pointer();(void)pointer();const auto footer=pointer();
            config.callback=pointer();config.data=pointer();config.width=static_cast<std::uint32_t>(number());
            if(button_count>256 || radio_count>256 || (button_count && !buttons) || (radio_count && !radios))return finish(invalid);
            config.title=text(config.instance,title);config.main=text(config.instance,main);config.content=text(config.instance,content);
            config.verification=text(config.instance,verification);config.expanded=text(config.instance,expanded);config.footer=text(config.instance,footer);
            const auto stride=unit+4;
            for(std::uint64_t i=0;i<button_count;++i){const auto item=buttons+i*stride;config.buttons.push_back({static_cast<std::int32_t>(memory.load(GuestAddress{item},32)),text(config.instance,memory.load(GuestAddress{item+4},bits))});}
            for(std::uint64_t i=0;i<radio_count;++i){const auto item=radios+i*stride;config.radios.push_back({static_cast<std::int32_t>(memory.load(GuestAddress{item},32)),text(config.instance,memory.load(GuestAddress{item+4},bits))});}
            button_out=arg(1);radio_out=arg(2);verification_out=arg(3);
            if(config.callback && !handles(GuestAddress{config.callback}))memory.check(GuestAddress{config.callback},1,Access::Execute);
        }
        if(config.owner && !gui_.windows.contains(config.owner))return finish(0x80070578U); // HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE)。
        if(config.flags&enable_hyperlinks)for(auto* value:{&config.main,&config.content,&config.expanded,&config.footer})*value=strip_links(*value);
        TaskDialogState::Dialog dialog;dialog.callback=config.callback;dialog.data=config.data;dialog.flags=config.flags;
        dialog.button_out=button_out;dialog.radio_out=radio_out;dialog.verification_out=verification_out;
        dialog.verification=(config.flags&verification_checked)!=0;
        // 常用按鈕依 Windows 呈現順序：OK、Yes、No、Retry、Cancel、Close。
        std::vector<std::pair<std::int32_t,std::u16string>> row;
        const bool links=(config.flags&(command_links|command_links_no_icon))!=0;
        if(!links)row=config.buttons;
        for(const auto& [bit,id,caption]:std::initializer_list<std::tuple<std::uint32_t,std::int32_t,std::u16string_view>>{
                {1,1,u"OK"},{2,6,u"Yes"},{4,7,u"No"},{0x10,4,u"Retry"},{8,2,u"Cancel"},{0x20,8,u"Close"}})
            if(config.common&bit)row.push_back({id,std::u16string(caption)});
        if(row.empty() && config.buttons.empty())row.push_back({1,u"OK"});
        for(const auto& button:config.buttons)dialog.buttons.push_back(button.first);
        for(const auto& button:row)if(std::find(dialog.buttons.begin(),dialog.buttons.end(),button.first)==dialog.buttons.end())dialog.buttons.push_back(button.first);
        for(const auto& radio:config.radios)dialog.radios.push_back(radio.first);
        dialog.cancel=(config.flags&allow_cancellation) || std::find(dialog.buttons.begin(),dialog.buttons.end(),cancel_button)!=dialog.buttons.end();
        if(!config.radios.empty()){
            const bool found=std::find(dialog.radios.begin(),dialog.radios.end(),config.default_radio)!=dialog.radios.end();
            dialog.radio=found?config.default_radio:(config.flags&0x4000U)?0:dialog.radios.front();
        }
        const auto default_button=std::find(dialog.buttons.begin(),dialog.buttons.end(),config.default_button)!=dialog.buttons.end()?config.default_button:dialog.buttons.front();
        // 版面以 DLU 配置（Segoe UI 9pt）；cxWidth 為工作區寬度 DLU。
        const std::int32_t width=config.width?static_cast<std::int32_t>(std::clamp<std::uint32_t>(config.width,120,1000)):240,margin=7,inner=width-2*margin;
        Template t;std::int32_t y=margin;const bool dynamic=config.callback!=0;
        const auto label=[&](std::int32_t id,const std::u16string& value,bool reserve){
            auto height=text_height(value,inner);if(!height && reserve)height=9;if(!height)return;
            t.item(0x80U,margin,y,inner,height,id,0x82,{},value);y+=height+5;
        };
        label(main_id,config.main,dynamic);label(content_id,config.content,dynamic);
        if(config.flags&expanded_by_default)label(expanded_id,config.expanded,false);
        if(config.flags&(progress_bar|marquee_progress)){t.item((config.flags&marquee_progress)?8U:0U,margin,y,inner,8,progress_id,0,u"msctls_progress32",{});y+=8+7;}
        for(std::size_t i=0;i<config.radios.size();++i){t.item(0x00010009U|(i?0U:0x20000U),margin+7,y,inner-7,10,config.radios[i].first,0x80,{},config.radios[i].second);y+=12;}
        if(links)for(const auto& [id,value]:config.buttons){
            auto caption=value;std::replace(caption.begin(),caption.end(),u'\n',u' ');
            t.item(0x00010000U|(id==default_button?1U:0U),margin+7,y,inner-7,20,id,0x80,{},caption);y+=23;
        }
        y+=3;const std::int32_t button_width=50,button_height=14;
        auto x=width-margin-static_cast<std::int32_t>(row.size())*(button_width+4)+4;
        if(!config.verification.empty()){
            // 核取方塊寬度＝方塊 14 DLU＋文字；與按鈕列同排放不下時獨立一列。
            const auto needed=static_cast<std::int32_t>(config.verification.size())*4+14;
            if(needed<=x-margin-4)t.item(0x00010003U,margin,y+2,x-margin-4,10,verification_id,0x80,{},config.verification);
            else {t.item(0x00010003U,margin,y,inner,10,verification_id,0x80,{},config.verification);y+=14;}
        }
        for(const auto& [id,value]:row){t.item(0x00010000U|(id==default_button?1U:0U),x,y,button_width,button_height,id,0x80,{},value);x+=button_width+4;}
        y+=button_height+margin;
        if(!config.footer.empty()){const auto height=text_height(config.footer,inner);t.item(0x80U,margin,y,inner,height,footer_id,0x82,{},config.footer);y+=height+margin;}
        if(!(config.flags&expanded_by_default) && !config.expanded.empty())runtime_.module_events.push_back("task_dialog_note=TaskDialog 展開資訊需展開按鈕，尚未呈現");
        Template header;
        const std::uint32_t style=0x80000000U|0x00c00000U|0x80U|0x40U|0x800U|(dialog.cancel?0x80000U:0U)|((config.flags&can_be_minimized)?0x20000U:0U);
        header.dword(style);header.dword(0);header.word(t.count);header.word(0);header.word(0);header.word(static_cast<std::uint32_t>(width));header.word(static_cast<std::uint32_t>(y));
        header.word(0);header.word(0);header.text(config.title);header.word(9);header.text(u"Segoe UI");
        header.align();header.bytes.insert(header.bytes.end(),t.bytes.begin(),t.bytes.end());
        const auto address=allocate_heap(memory,header.bytes.size());if(!address)return finish(no_memory);
        memory.write(GuestAddress{address},header.bytes);dialog.template_address=address;
        if(button_out)memory.store(GuestAddress{button_out},32,0);
        const auto token=bridge.next++;bridge.dialogs.emplace(token,std::move(dialog));
        start_callbacks(state,memory,{
            {resolve("user32.dll","DialogBoxIndirectParamW").value,{config.instance,address,config.owner,resolve("internal-taskdialog","DialogProc").value,token}},
            {resolve("internal-taskdialog","Finish").value,{token}}},Continuation::ReturnCallback);
        return std::nullopt;
    }
    if(name=="Finish"){
        const auto found=bridge.dialogs.find(arg(0));if(found==bridge.dialogs.end())return finish(failure);
        auto dialog=std::move(found->second);bridge.dialogs.erase(found);
        if(dialog.template_address)(void)free_heap(memory,dialog.template_address,state);
        if(!dialog.ended)return finish(failure);
        if(dialog.button_out)memory.store(GuestAddress{dialog.button_out},32,static_cast<std::uint32_t>(dialog.result));
        if(dialog.radio_out)memory.store(GuestAddress{dialog.radio_out},32,static_cast<std::uint32_t>(dialog.radio));
        if(dialog.verification_out)memory.store(GuestAddress{dialog.verification_out},32,dialog.verification?1:0);
        return finish(0);
    }
    // DialogProc(hwnd,message,wParam,lParam)：回傳值為 DLGPROC 的 BOOL。
    const auto window=arg(0);const auto message=static_cast<std::uint32_t>(arg(1));const auto wparam=arg(2),lparam=arg(3);
    if(message==0x110){
        const auto found=bridge.dialogs.find(lparam);if(found==bridge.dialogs.end())return finish(0);
        auto& dialog=found->second;dialog.window=window;dialog.timer_reset=runtime_.clock_ticks;
        const auto set_check=[&](std::int32_t id,std::uint32_t value){if(const auto child=gui_child(window,static_cast<std::uint64_t>(id))){gui_.windows.at(child).check=value;present_gui(child);}};
        if(dialog.radio)set_check(dialog.radio,1);
        if(dialog.verification)set_check(verification_id,1);
        if(dialog.flags&marquee_progress)if(const auto child=gui_child(window,progress_id)){gui_.windows.at(child).progress_marquee=true;present_gui(child);}
        std::vector<GuestCallback> callbacks;
        if(dialog.flags&callback_timer)callbacks.push_back({resolve("user32.dll","SetTimer").value,{window,timer_id,200,0}});
        if(dialog.callback){
            callbacks.push_back({dialog.callback,{window,tdn_constructed,0,0,dialog.data}});
            callbacks.push_back({dialog.callback,{window,tdn_created,0,0,dialog.data}});
        }
        if(callbacks.empty())return finish(1);
        start_callbacks(state,memory,std::move(callbacks),Continuation::Return,1);return std::nullopt;
    }
    const auto found=std::find_if(bridge.dialogs.begin(),bridge.dialogs.end(),[&](const auto& entry){return entry.second.window==window;});
    if(found==bridge.dialogs.end())return finish(0);
    const auto token=found->first;auto& dialog=found->second;
    const auto contains=[](const std::vector<std::int32_t>& list,std::int32_t id){return std::find(list.begin(),list.end(),id)!=list.end();};
    const auto end_dialog=[&](std::int32_t id){
        dialog.result=id;dialog.ended=true;
        if(const auto target=gui_.windows.find(window);target!=gui_.windows.end() && target->second.modal){target->second.ended=true;target->second.dialog_result=static_cast<std::uint64_t>(id);}
    };
    const auto notify=[&](std::uint32_t code,std::uint64_t first,std::uint64_t second,std::uint64_t value){
        if(!dialog.callback)return finish(value);
        start_callbacks(state,memory,{{dialog.callback,{window,code,first,second,dialog.data}}},Continuation::Return,value);return std::optional<std::uint32_t>{};
    };
    const auto click=[&](std::int32_t id){
        if(!dialog.callback){end_dialog(id);return finish(1);}
        dialog.pending=id;
        start_callbacks(state,memory,{{dialog.callback,{window,tdn_button_clicked,static_cast<std::uint32_t>(id),0,dialog.data}}},Continuation::TaskDialogOperation,token*4+1);
        return std::optional<std::uint32_t>{};
    };
    const auto child_check=[&](std::int32_t id){const auto child=gui_child(window,static_cast<std::uint64_t>(id));return child && gui_.windows.at(child).check!=0;};
    const auto record=[&]{
        if(gui_child(window,verification_id))dialog.verification=child_check(verification_id);
        for(const auto id:dialog.radios)if(child_check(id))dialog.radio=id;
    };
    const auto guest=[&](std::string_view symbol,std::vector<std::uint64_t> arguments){
        start_callbacks(state,memory,{{resolve("user32.dll",symbol).value,std::move(arguments)}},Continuation::Return,1);return std::optional<std::uint32_t>{};
    };
    const auto progress=[&](std::uint32_t command,std::uint64_t first,std::uint64_t second){
        const auto child=gui_child(window,progress_id);if(!child)return finish(0);
        return guest("SendMessageW",{child,command,first,second});
    };
    switch(message){
    case 0x111:{
        const auto id=static_cast<std::int32_t>(wparam&0xffffU);if((wparam>>16)&0xffffU)return finish(0);
        if(id==verification_id){record();return notify(tdn_verification_clicked,dialog.verification?1:0,0,1);}
        if(contains(dialog.radios,id)){dialog.radio=id;return notify(tdn_radio_clicked,static_cast<std::uint32_t>(id),0,1);}
        if(id==cancel_button && !contains(dialog.buttons,id))return dialog.cancel?click(id):finish(1);
        if(contains(dialog.buttons,id))return click(id);
        return finish(0);
    }
    case 0x113:
        if(wparam!=timer_id)return finish(0);
        if(!dialog.callback)return finish(1);
        start_callbacks(state,memory,{{dialog.callback,{window,tdn_timer,(runtime_.clock_ticks-dialog.timer_reset)/10000,0,dialog.data}}},Continuation::TaskDialogOperation,token*4+2);
        return std::nullopt;
    case 0x2:{record();dialog.window=0;
        if(!dialog.callback)return finish(0);
        start_callbacks(state,memory,{{dialog.callback,{window,tdn_destroyed,0,0,dialog.data}}},Continuation::Return,0);return std::nullopt;}
    case 0x466:return click(static_cast<std::int32_t>(wparam));                            // TDM_CLICK_BUTTON
    case 0x467:                                                                             // TDM_SET_MARQUEE_PROGRESS_BAR
        if(const auto child=gui_child(window,progress_id)){auto& control=gui_.windows.at(child);control.style=wparam?(control.style|8U):(control.style&~8U);if(!wparam)control.progress_marquee=false;present_gui(child);}
        return finish(1);
    case 0x468:return progress(0x410,wparam,0);                                             // TDM_SET_PROGRESS_BAR_STATE
    case 0x469:return progress(0x401,0,lparam);                                             // TDM_SET_PROGRESS_BAR_RANGE
    case 0x46a:return progress(0x402,wparam,0);                                             // TDM_SET_PROGRESS_BAR_POS
    case 0x46b:return progress(0x40a,wparam,lparam);                                        // TDM_SET_PROGRESS_BAR_MARQUEE
    case 0x46c:case 0x472:{                                                                 // TDM_SET／UPDATE_ELEMENT_TEXT
        static constexpr std::int32_t elements[]{content_id,expanded_id,footer_id,main_id};
        if(wparam>3)return finish(0);
        const auto child=gui_child(window,static_cast<std::uint64_t>(elements[wparam]));if(!child)return finish(0);
        auto value=text(0,lparam);if(dialog.flags&enable_hyperlinks)value=strip_links(value);
        const auto bytes=utf16_bytes(value);const auto copy=allocate_heap(memory,bytes.size());if(!copy)return finish(0);
        memory.write(GuestAddress{copy},bytes);
        start_callbacks(state,memory,{{resolve("user32.dll","SetWindowTextW").value,{child,copy}}},Continuation::ReturnCallbackFreeHeap,copy);return std::nullopt;
    }
    case 0x46e:{                                                                            // TDM_CLICK_RADIO_BUTTON
        const auto id=static_cast<std::int32_t>(wparam);if(!contains(dialog.radios,id))return finish(0);
        for(const auto radio:dialog.radios)if(const auto child=gui_child(window,static_cast<std::uint64_t>(radio))){gui_.windows.at(child).check=radio==id;present_gui(child);}
        dialog.radio=id;return notify(tdn_radio_clicked,static_cast<std::uint32_t>(id),0,1);
    }
    case 0x46f:case 0x470:{                                                                 // TDM_ENABLE_BUTTON／RADIO_BUTTON
        const auto child=gui_child(window,wparam&0xffffffffU);if(!child)return finish(0);
        return guest("EnableWindow",{child,lparam?1U:0U});
    }
    case 0x471:{                                                                            // TDM_CLICK_VERIFICATION
        const auto child=gui_child(window,verification_id);if(!child)return finish(0);
        gui_.windows.at(child).check=wparam?1:0;present_gui(child);dialog.verification=wparam!=0;
        return notify(tdn_verification_clicked,wparam?1:0,0,1);
    }
    case 0x473:case 0x474:return finish(1);                                                 // 盾牌圖示與主／頁尾圖示只影響外觀。
    case 0x465:                                                                             // TDM_NAVIGATE_PAGE
        if(!bridge.reported_navigation){bridge.reported_navigation=true;runtime_.module_events.push_back("task_dialog_note=TDM_NAVIGATE_PAGE 尚未支援");}
        return finish(0);
    default:return finish(0);
    }
}
std::optional<std::uint32_t> WindowsServices::resume_task_dialog(GuestState& state,GuestMemory& memory,std::uint64_t value,std::uint64_t result){
    const auto phase=value&3U;const auto token=value>>2;
    if(task_dialogs_){
        if(task_dialogs_.use_count()>1)task_dialogs_=std::make_shared<TaskDialogState>(*task_dialogs_);
        const auto found=task_dialogs_->dialogs.find(token);
        if(found!=task_dialogs_->dialogs.end() && found->second.window){
            auto& dialog=found->second;const auto status=static_cast<std::uint32_t>(result);
            // TDN_BUTTON_CLICKED 回傳 S_OK 才關閉；TDN_TIMER 回傳 S_FALSE 重設計時。
            if(phase==1 && status==0){
                dialog.result=dialog.pending;dialog.ended=true;
                if(const auto target=gui_.windows.find(dialog.window);target!=gui_.windows.end() && target->second.modal){target->second.ended=true;target->second.dialog_result=static_cast<std::uint64_t>(dialog.pending);}
            }
            if(phase==2 && status==1)dialog.timer_reset=runtime_.clock_ticks;
        }
    }
    complete_call(state,memory,1);return std::nullopt;
}
}
