#include "x64arm/windows_services.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <cstdio>

namespace x64arm {
namespace {
struct DialogFailure {std::uint32_t error;};
struct DialogField {std::u16string text;std::optional<std::uint16_t> ordinal;};
class DialogReader {
    const GuestMemory& memory_;
    std::uint64_t begin_,size_,cursor_;
public:
    DialogReader(const GuestMemory& memory,std::uint64_t begin,std::uint64_t size):memory_(memory),begin_(begin),size_(size),cursor_(begin){}
    std::uint64_t address()const{return cursor_;}
    std::uint64_t number(unsigned bytes){
        if(cursor_<begin_ || cursor_-begin_>size_ || bytes>size_-(cursor_-begin_) || cursor_>UINT64_MAX-bytes)throw DialogFailure{13};
        const auto value=memory_.load(GuestAddress{cursor_},static_cast<std::uint8_t>(bytes*8));cursor_+=bytes;return value;
    }
    void align(){while(cursor_%4)(void)number(1);}
    DialogField field(){
        DialogField result;auto value=number(2);
        if(value==0xffff){result.ordinal=static_cast<std::uint16_t>(number(2));return result;}
        while(value){if(result.text.size()>=32768)throw DialogFailure{13};result.text.push_back(static_cast<char16_t>(value));value=number(2);}
        return result;
    }
    std::int32_t coordinate(){return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(number(2)))*2;}
    std::uint32_t dimension(){const auto value=coordinate();if(value<0 || value>8192)throw DialogFailure{87};return static_cast<std::uint32_t>(value);}
};
bool dialog_direct_result(std::uint32_t message){
    return message==0x2e || message==0x2f || message==0x39 || message==0x37 || message==0x110 || (message>=0x132 && message<=0x138);
}
bool dialog_descendant(const GuiState& gui,std::uint64_t root,std::uint64_t child){
    for(std::size_t depth=0;child && depth<=gui.windows.size();++depth){
        if(child==root)return true;const auto found=gui.windows.find(child);if(found==gui.windows.end())break;child=found->second.parent;
    }return false;
}
bool dialog_navigable(const GuiState& gui,std::uint64_t root,std::uint64_t child){
    for(std::size_t depth=0;child && child!=root && depth<=gui.windows.size();++depth){
        const auto found=gui.windows.find(child);if(found==gui.windows.end() || !found->second.visible || !found->second.enabled || (found->second.style&0x08000000U))return false;
        child=found->second.parent;if(child && child!=root){const auto parent=gui.windows.find(child);if(parent==gui.windows.end() || !(parent->second.ex_style&0x10000U))return false;}
    }return child==root;
}
}

GuiDialogLayout WindowsServices::gui_dialog_layout(GuestMemory& memory,std::uint64_t begin,std::uint64_t size){
    DialogReader reader(memory,begin,size);GuiDialogLayout layout;
    const auto first=reader.number(2),second=reader.number(2);const bool extended=second==65535;
    std::uint32_t style;
    if(extended){if(first!=1)throw DialogFailure{13};(void)reader.number(4);(void)reader.number(4);style=static_cast<std::uint32_t>(reader.number(4));}
    else {style=static_cast<std::uint32_t>(first|(second<<16));(void)reader.number(4);}
    (void)reader.number(2);(void)reader.number(2);(void)reader.number(2);const auto width=reader.dimension()/2,height=reader.dimension()/2;
    (void)reader.field();(void)reader.field();layout.title=reader.field().text;
    if(style&0x40U){GuiFont font;const auto points=reader.number(2);if(!points || points>384)throw DialogFailure{87};font.height=-static_cast<std::int32_t>((points*96+36)/72);
        if(extended){font.weight=static_cast<std::int32_t>(reader.number(2));font.italic=reader.number(1)!=0;font.charset=static_cast<std::uint8_t>(reader.number(1));}
        const auto face=reader.field();if(face.ordinal || face.text.size()>31)throw DialogFailure{13};font.face=face.text;
        const auto metrics=native_gui_?native_gui_->font_metrics(font).value_or(fallback_font_metrics(font)):fallback_font_metrics(font);
        layout.base_x=static_cast<std::uint32_t>(std::max(1,metrics.average_width));layout.base_y=static_cast<std::uint32_t>(std::max(1,metrics.height));layout.font=std::move(font);
    }
    layout.width=(width*layout.base_x+2)/4;layout.height=(height*layout.base_y+4)/8;return layout;
}

std::optional<std::uint32_t> WindowsServices::resume_gui_dialog(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t value){
    // continuation 的 2／4／6／8 階段分別查詢輸入、導覽、預設按鈕及目標控制項；低位保留 A/W。
    if(frame.value>=2){
        const WindowsCall arg(frame.saved,memory,2);const auto root=arg(0);const bool wide=(frame.value&1U)!=0;
        if(!gui_.windows.contains(root)){complete_call(state,memory,1);return std::nullopt;}
        const auto msg=gui_read_message(memory,arg(1));const auto source=gui_.windows.find(msg.window);
        if(source==gui_.windows.end() || !dialog_descendant(gui_,root,msg.window)){complete_call(state,memory,1);return std::nullopt;}
        const auto send=resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value;
        // 複合控制項的內部子視窗以可導覽的祖先作為起點；不把內部 Edit 加入外層 Tab 順序。
        const auto navigation_source=[&](){auto handle=msg.window;for(std::size_t depth=0;handle!=root && depth<=gui_.windows.size();++depth){if(dialog_navigable(gui_,root,handle))return handle;const auto child=gui_.windows.find(handle);if(child==gui_.windows.end())return std::uint64_t{};handle=child->second.parent;}return std::uint64_t{};};
        const auto command=[&](std::uint64_t id,std::uint64_t child){
            if(child && (!gui_.windows.contains(child) || !gui_.windows.at(child).enabled)){complete_call(state,memory,1);return;}
            if(common_dialog_message(memory,{root,id,child,0x111})){complete_call(state,memory,1);return;}
            start_callbacks(state,memory,{{send,{root,0x111,id&65535U,child}}},Continuation::Return,1);
        };
        if((frame.value&~UINT64_C(1))==4){
            const auto next=gui_.windows.find(value);if(next==gui_.windows.end()){complete_call(state,memory,1);return std::nullopt;}
            start_callbacks(state,memory,{{send,{value,0x87,msg.wparam,0}}},Continuation::GuiDialogProc,8U|wide);return std::nullopt;
        }
        if((frame.value&~UINT64_C(1))==8){
            const auto target=frame.callbacks.at(0).arguments.at(0);const auto next=gui_.windows.find(target);
            if(next==gui_.windows.end() || !dialog_navigable(gui_,root,target) || (value&0x100U)){complete_call(state,memory,1);return std::nullopt;}
            std::vector<GuestCallback> callbacks{{resolve("user32.dll","SetFocus").value,{target}}};
            if(value&8U)callbacks.push_back({send,{target,0xb1,0,UINT64_MAX}});
            if(msg.wparam>=37 && msg.wparam<=40 && (value&0x40U) && gui_control_class(next->second)==u"button" && (next->second.style&15U)==9)callbacks.push_back({send,{target,0xf5,0,0}});
            start_callbacks(state,memory,std::move(callbacks),Continuation::Return,1);return std::nullopt;
        }
        if((frame.value&~UINT64_C(1))==6){
            auto id=(value>>16)==0x534b?value&65535U:gui_.windows.at(root).default_id;
            if(!id){for(const auto& [h,w]:gui_.windows)if(w.parent==root && gui_control_class(w)==u"button" && (w.style&15U)==1){id=w.id;break;}}
            if(!id)id=1;command(id,gui_child(root,id));return std::nullopt;
        }
        if(msg.message==0x100 && !(value&4U)){
            if(msg.wparam==9 && !(value&2U)){
                start_callbacks(state,memory,{{resolve("user32.dll","GetNextDlgTabItem").value,{root,navigation_source(),(gui_.keyboard[16]&128U)!=0}}},Continuation::GuiDialogProc,4U|wide);return std::nullopt;
            }
            if(msg.wparam>=37 && msg.wparam<=40 && !(value&1U)){
                start_callbacks(state,memory,{{resolve("user32.dll","GetNextDlgGroupItem").value,{root,navigation_source(),msg.wparam==37 || msg.wparam==38}}},Continuation::GuiDialogProc,4U|wide);return std::nullopt;
            }
            if(msg.wparam==27){command(2,gui_child(root,2));return std::nullopt;}
            if(msg.wparam==13){
                if(value&(0x10U|0x20U)){command(source->second.id,msg.window);return std::nullopt;}
                start_callbacks(state,memory,{{send,{root,0x400,0,0}}},Continuation::GuiDialogProc,6U|wide);return std::nullopt;
            }
        }
        if((msg.message==0x106 || msg.message==0x102) && !(value&(4U|128U))){
            const auto fold=[](char16_t c){return unicode_case_unit(c,true);};
            for(auto h:gui_.z_order){const auto item=gui_.windows.find(h);if(item==gui_.windows.end() || !dialog_descendant(gui_,root,h) || h==root)continue;
                const auto& w=item->second;if(!dialog_navigable(gui_,root,h) || (gui_control_class(w)==u"static" && (w.style&0x80U)))continue;bool match=false;
                for(std::size_t n=0;n+1<w.title.size();++n)if(w.title[n]==u'&'){if(w.title[n+1]==u'&'){++n;continue;}if(fold(w.title[n+1])==fold(static_cast<char16_t>(msg.wparam))){match=true;break;}}
                if(!match)continue;
                if(gui_control_class(w)==u"button"){start_callbacks(state,memory,{{resolve("user32.dll","SetFocus").value,{h}},{send,{h,0xf5,0,0}}},Continuation::Return,1);return std::nullopt;}
                start_callbacks(state,memory,{{resolve("user32.dll","GetNextDlgTabItem").value,{root,h,0}}},Continuation::GuiDialogProc,4U|wide);return std::nullopt;
            }
        }
        start_callbacks(state,memory,{{resolve("user32.dll","TranslateMessage").value,{arg(1)}},{resolve("user32.dll",wide?"DispatchMessageW":"DispatchMessageA").value,{arg(1)}}},Continuation::Return,1);return std::nullopt;
    }
    const WindowsCall arg(frame.saved,memory,4);
    const auto handle=arg(0);
    const auto message=static_cast<std::uint32_t>(arg(1));
    const bool wide=frame.value!=0;
    const auto wparam=arg(2),lparam=arg(3);
    const auto found=gui_.windows.find(handle);
    if(found==gui_.windows.end()){complete_call(state,memory,0);return std::nullopt;}
    auto& window=found->second;
    if(dialog_direct_result(message)){complete_call(state,memory,value);return std::nullopt;}
    if(value){
        std::uint64_t result=0;for(unsigned i=0;i<(x86_?4U:8U) && i<window.extra.size();++i)result|=std::uint64_t{window.extra[i]}<<(i*8);
        complete_call(state,memory,result);return std::nullopt;
    }
    // DefDlgProc 是 WNDPROC；只有這裡會將未處理訊息轉成預設視窗行為。
    // 不重新 SendMessage，否則 subclass 呼叫舊 DefDlgProc 時會無限遞迴。
    if(message==0x400){complete_call(state,memory,window.default_id?(UINT64_C(0x534b)<<16)|(window.default_id&0xffff):0);return std::nullopt;}
    if(message==0x401){window.default_id=wparam;complete_call(state,memory,1);return std::nullopt;}
    if(window.modal && !window.dialog_proc && (message==0x111 || message==0x10)){
        window.ended=true;window.dialog_result=message==0x111?(wparam&65535):gui_child(handle,2)?2:1;
        complete_call(state,memory,0);return std::nullopt;
    }
    start_callbacks(state,memory,{{resolve("user32.dll",wide?"DefWindowProcW":"DefWindowProcA").value,{handle,message,wparam,lparam}}},Continuation::ReturnCallback);
    return std::nullopt;
}

std::optional<std::uint32_t> WindowsServices::invoke_gui_dialog(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const bool wide=symbol.ends_with('W');
    if(symbol.starts_with("IsDialogMessage")){
        if(!arg(1) || !gui_.windows.contains(arg(0))){arg.return_to(state,0);return std::nullopt;}
        const auto msg=gui_read_message(memory,arg(1));
        if(msg.message<0x100 || msg.message>0x109 || !dialog_descendant(gui_,arg(0),msg.window)){arg.return_to(state,0);return std::nullopt;}
        start_callbacks(state,memory,{{resolve("user32.dll",wide?"SendMessageW":"SendMessageA").value,{msg.window,0x87,msg.wparam,arg(1)}}},Continuation::GuiDialogProc,2U|wide);return std::nullopt;
    }
    if(symbol.starts_with("DefDlgProc") && gui_.property_sheets.contains(arg(0)))return invoke_property_sheet(state,memory,service);
    if(symbol.starts_with("DefDlgProc")){
        const auto found=gui_.windows.find(arg(0));
        if(found==gui_.windows.end()){last_error_=1400;arg.return_to(state,0);return std::nullopt;}
        auto& window=found->second;
        // DWLP_MSGRESULT 是每次訊息的暫存槽，巢狀訊息可以改寫它。
        if(window.extra.size()<40)window.extra.resize(40);
        std::fill_n(window.extra.begin(),x86_?4:8,0);
        std::vector<GuestCallback> callbacks;if(window.dialog_proc)callbacks.push_back({window.dialog_proc,{arg(0),arg(1),arg(2),arg(3)}});
        start_callbacks(state,memory,std::move(callbacks),Continuation::GuiDialogProc,wide);return std::nullopt;
    }
    const bool modal=symbol.starts_with("DialogBox"),indirect=symbol.find("Indirect")!=std::string_view::npos;
    const auto fail=[&](std::uint32_t error){last_error_=error;arg.return_to(state,modal?UINT64_MAX:0);};
    if(!arg(1)){fail(87);return std::nullopt;}
    if(arg(2) && !gui_.windows.contains(arg(2))){last_error_=1400;arg.return_to(state,0);return std::nullopt;}
    try {
        std::uint64_t begin=arg(1),size=output_limit;
        if(!indirect){
            const auto resource=gui_resource(memory,arg(0),arg(1),5,wide);
            if(!resource)throw DialogFailure{1814};
            const auto* image=resource_image(resource,true);if(!image)throw DialogFailure{1814};
            begin=image->base+memory.load(GuestAddress{resource},32);size=memory.load(GuestAddress{resource+4},32);
        }
        if(begin%4 || size<18)throw DialogFailure{13};
        DialogReader reader(memory,begin,size);const auto first=reader.number(2),second=reader.number(2);const bool extended=second==65535;
        GuiWindow window;window.dpi_context=effective_dpi_context();window.thread=threads_.current;window.dialog=true;window.modal=modal;window.instance=arg(0)?arg(0):runtime_.image.base;
        window.owner=arg(2);window.dialog_proc=arg(3);window.font=0xb011;
        if(extended){if(first!=1)throw DialogFailure{13};(void)reader.number(4);window.ex_style=static_cast<std::uint32_t>(reader.number(4));window.style=static_cast<std::uint32_t>(reader.number(4));}
        else {window.style=static_cast<std::uint32_t>(first|(second<<16));window.ex_style=static_cast<std::uint32_t>(reader.number(4));}
        const auto count=reader.number(2);if(count>=4096 || gui_.windows.size()>4095-count)throw DialogFailure{8};
        window.x=reader.coordinate();window.y=reader.coordinate();window.width=reader.dimension();window.height=reader.dimension();
        const auto menu_address=reader.address();const auto menu=reader.field();
        const auto menu_name=menu.ordinal?std::uint64_t{*menu.ordinal}:menu.text.empty()?0:menu_address;
        const auto cls=reader.field(),title=reader.field();if(title.ordinal)throw DialogFailure{13};
        window.class_name=cls.ordinal?(*cls.ordinal==32770?u"#32770":std::u16string{}):cls.text.empty()?u"#32770":cls.text;
        auto registered=gui_.classes.end();
        if(cls.ordinal && *cls.ordinal!=32770)registered=std::find_if(gui_.classes.begin(),gui_.classes.end(),[&](const auto& item){return item.second.atom==*cls.ordinal;});
        else registered=gui_.classes.find(window.class_name);
        if(registered!=gui_.classes.end()){window.class_name=registered->first;window.class_atom=registered->second.atom;window.procedure=registered->second.procedure;window.extra.resize(std::max<std::uint32_t>(40,registered->second.extra_size));}
        else if(window.class_name==u"#32770"){window.procedure=resolve("user32.dll",wide?"DefDlgProcW":"DefDlgProcA").value;window.extra.resize(40);}
        else {
            if(!trace_services_.empty())std::fprintf(stderr,"[dialog] pid=%llu template=%llx unknown_parent_class=%s ordinal=%u\n",static_cast<unsigned long long>(process_id()),static_cast<unsigned long long>(begin),utf16_to_utf8(window.class_name).c_str(),cls.ordinal.value_or(0));
            throw DialogFailure{1407};
        }
        window.title=title.text;window.visible=(window.style&0x10000000)!=0;window.enabled=(window.style&0x08000000)==0;
        if(window.style&0x40000000){window.parent=window.owner;window.owner=0;}
        if(window.style&0x40){(void)reader.number(2);if(extended){(void)reader.number(2);(void)reader.number(1);(void)reader.number(1);}if(reader.field().ordinal)throw DialogFailure{13};}
        const auto layout=gui_dialog_layout(memory,begin,size);window.dialog_base_x=layout.base_x;window.dialog_base_y=layout.base_y;
        const auto horizontal=[&](std::int32_t v){return static_cast<std::int32_t>((static_cast<std::int64_t>(v)*layout.base_x+(v<0?-4:4))/8);};
        const auto vertical=[&](std::int32_t v){return static_cast<std::int32_t>((static_cast<std::int64_t>(v)*layout.base_y+(v<0?-8:8))/16);};
        window.x=horizontal(window.x);window.y=vertical(window.y);window.width=layout.width;window.height=layout.height;
        std::vector<std::uint8_t> dialog_font;
        if(layout.font){const auto& font=*layout.font;dialog_font.resize(92);const auto put=[&](unsigned at,std::uint32_t value){for(unsigned n=0;n<4;++n)dialog_font[at+n]=static_cast<std::uint8_t>(value>>(n*8));};put(0,static_cast<std::uint32_t>(font.height));put(16,static_cast<std::uint32_t>(font.weight));dialog_font[20]=font.italic;dialog_font[23]=font.charset;for(std::size_t n=0;n<font.face.size();++n){dialog_font[28+n*2]=static_cast<std::uint8_t>(font.face[n]);dialog_font[29+n*2]=static_cast<std::uint8_t>(font.face[n]>>8);}}
        if(window.dialog_proc && !handles(GuestAddress{window.dialog_proc}))memory.check(GuestAddress{window.dialog_proc},1,Access::Execute);
        gui_refresh_frame(window);
        const auto handle=allocate_gui_window();
        std::vector<GuiWindowCreation> plans;plans.push_back({handle,std::move(window),wide});
        plans.back().class_pointer=cls.ordinal?std::optional<std::uint64_t>{*cls.ordinal}:std::nullopt;
        for(std::uint64_t i=0;i<count;++i){
            reader.align();GuiWindow child;child.dpi_context=effective_dpi_context();child.thread=threads_.current;child.parent=handle;child.instance=plans.front().window.instance;child.font=plans.front().window.font;
            if(extended){(void)reader.number(4);child.ex_style=static_cast<std::uint32_t>(reader.number(4));child.style=static_cast<std::uint32_t>(reader.number(4));}
            else {child.style=static_cast<std::uint32_t>(reader.number(4));child.ex_style=static_cast<std::uint32_t>(reader.number(4));}
            child.style|=0x40000000;child.x=reader.coordinate();child.y=reader.coordinate();child.width=reader.dimension();child.height=reader.dimension();child.id=reader.number(extended?4:2);
            const auto child_class=reader.field(),child_title=reader.field();child.class_name=child_class.text;
            if(child_class.ordinal && *child_class.ordinal>=0x80 && *child_class.ordinal<=0x85){constexpr std::u16string_view names[]{u"button",u"edit",u"static",u"listbox",u"scrollbar",u"combobox"};child.class_name=names[*child_class.ordinal-0x80];}
            child.procedure=stock_gui_proc(child.class_name,wide);child.builtin=child.procedure!=0;
            if(child.builtin)child.control_class=std::u16string(stock_gui_class(child.procedure));
            else {const auto custom=gui_.classes.find(child.class_name);if(custom!=gui_.classes.end()){child.procedure=custom->second.procedure;child.class_atom=custom->second.atom;child.extra.resize(custom->second.extra_size);}}
            child.x=horizontal(child.x);child.y=vertical(child.y);child.width=static_cast<std::uint32_t>(horizontal(static_cast<std::int32_t>(child.width)));child.height=static_cast<std::uint32_t>(vertical(static_cast<std::int32_t>(child.height)));
            child.title=child_title.text;const auto extra=reader.address(),length=reader.number(2);
            if(length){if(!extended && length<2)throw DialogFailure{13};for(std::uint64_t n=extended?0:2;n<length;++n)(void)reader.number(1);}
            // DS_NOFAILCREATE 只允許略過無法建立的控制項；仍須完整解析資料，不能掩蓋模板截斷。
            if(!child.procedure){
                if(!trace_services_.empty())std::fprintf(stderr,"[dialog] pid=%llu template=%llx unknown_child_class=%s ordinal=%u id=%llu\n",static_cast<unsigned long long>(process_id()),static_cast<unsigned long long>(begin),utf16_to_utf8(child.class_name).c_str(),child_class.ordinal.value_or(0),static_cast<unsigned long long>(child.id));
                if(plans.front().window.style&0x10U)continue;throw DialogFailure{1407};
            }
            child.visible=(child.style&0x10000000)!=0;child.enabled=(child.style&0x08000000)==0;
            const bool combo=child.control_class==u"combobox" || child.control_class==u"comboboxex32";
            if(combo)child.combo_drop_height=child.height>24?child.height-24:100;
            gui_refresh_frame(child);const auto client=gui_client_size(child.width,combo && (child.style&3U)!=1U?24U:child.height,gui_frame_insets(child));child.width=client[0];child.height=client[1];
            plans.push_back({allocate_gui_window(),std::move(child),wide});
            auto& plan=plans.back();plan.class_pointer=child_class.ordinal?std::optional<std::uint64_t>{*child_class.ordinal}:std::nullopt;plan.name_pointer=child_title.ordinal?std::optional<std::uint64_t>{*child_title.ordinal}:std::nullopt;
            if(length){plan.parameter_bytes.resize(static_cast<std::size_t>(reader.address()-extra));memory.read(GuestAddress{extra},plan.parameter_bytes);}
        }
        if(menu_name){
            // DLGTEMPLATE 的資源名稱一律 UTF-16，包括 CreateDialog* A 入口。
            const auto entry=gui_resource(memory,plans.front().window.instance,menu_name,4,true);
            if(!entry)throw DialogFailure{1814};
            const auto* image=resource_image(entry,true);if(!image)throw DialogFailure{1814};
            try{plans.front().window.menu=load_gui_menu(memory,image->base+memory.load(GuestAddress{entry},32),memory.load(GuestAddress{entry+4},32));}
            catch(const HostCallFault&){throw DialogFailure{13};}
            plans.front().owned_menu=true;gui_refresh_frame(plans.front().window);
        }
        if(!dialog_font.empty()){const auto font=gui_.next_object++;gui_.fonts.emplace(font,std::move(dialog_font));plans.front().window.owned_font=true;for(auto& plan:plans){plan.window.font=font;plan.window.control_font=gui_font(gui_,font);}}
        return start_gui_creation(state,memory,std::move(plans),arg(4));
    } catch(const DialogFailure& error){fail(error.error);}catch(const std::bad_alloc&){fail(8);}
    return std::nullopt;
}
}
