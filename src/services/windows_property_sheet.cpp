#include "x64arm/windows_services.hpp"
#include "x64arm/windows_window_state.hpp"
#include "x64arm/windows_geometry.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t page_create=1,page_activate=2,page_kill=3,page_apply=4,page_query_cancel=5,page_reset=6,page_help=7,page_precreate=8;
}
std::optional<std::uint32_t> WindowsServices::resume_property_sheet(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t value){
    return advance_property_sheet(state,memory,frame.value,value);
}
std::optional<std::uint32_t> WindowsServices::advance_property_sheet(GuestState& state,GuestMemory& memory,std::uint64_t handle,std::uint64_t value){
    const auto found=gui_.property_sheets.find(handle);
    if(found==gui_.property_sheets.end() || !gui_.windows.contains(handle)){complete_call(state,memory,0);return std::nullopt;}
    auto& sheet=found->second;auto& root=gui_.windows.at(handle);
    const auto done=[&](std::uint64_t result){sheet.phase=0;complete_call(state,memory,result);};
    const auto notify=[&](std::size_t index,std::int32_t code,std::uint32_t phase){
        const auto window=sheet.pages[index].window;sheet.phase=phase;
        memory.write(GuestAddress{sheet.notification},std::array<std::uint8_t,32>{});
        const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
        memory.store(GuestAddress{sheet.notification},bits,handle);memory.store(GuestAddress{sheet.notification+unit*2},32,static_cast<std::uint32_t>(code));
        memory.store(GuestAddress{sheet.notification+(x86_?12:24)},static_cast<std::uint8_t>(x86_?32:64),code==-202 && sheet.action==1?1:0);
        start_callbacks(state,memory,{{gui_.windows.at(window).procedure,{window,0x4e,0,sheet.notification}}},Continuation::GuiPropertySheet,handle);
    };
    const auto activate=[&](){
        auto& page=sheet.pages[sheet.target];
        if(!page.window){if(page.callback){sheet.phase=page_precreate;start_callbacks(state,memory,{{page.callback,{0,2,page.descriptor}}},Continuation::GuiPropertySheet,handle);return;}sheet.phase=page_create;
            start_callbacks(state,memory,{{resolve("user32.dll",page.wide?(page.flags&1?"CreateDialogIndirectParamW":"CreateDialogParamW"):(page.flags&1?"CreateDialogIndirectParamA":"CreateDialogParamA")).value,{page.instance,page.resource,handle,page.procedure,page.descriptor}}},Continuation::GuiPropertySheet,handle);
        }else notify(sheet.target,-200,page_activate);
    };
    if(sheet.phase==page_help){done(0);return std::nullopt;}
    if(sheet.phase==page_precreate){if(!value){done(0);return {};}auto& page=sheet.pages[sheet.target];sheet.phase=page_create;
        start_callbacks(state,memory,{{resolve("user32.dll",page.wide?(page.flags&1?"CreateDialogIndirectParamW":"CreateDialogParamW"):(page.flags&1?"CreateDialogIndirectParamA":"CreateDialogParamA")).value,{page.instance,page.resource,handle,page.procedure,page.descriptor}}},Continuation::GuiPropertySheet,handle);return {};
    }
    if(sheet.phase==page_create){
        if(!value || !gui_.windows.contains(value)){root.ended=true;root.dialog_result=UINT64_MAX;done(0);return std::nullopt;}
        auto& page=sheet.pages[sheet.target];page.window=value;
        auto& window=gui_.windows.at(value);window.parent=handle;window.owner=0;window.x=8;window.y=static_cast<std::int32_t>(sheet.page_y);window.visible=false;gui_set_window_enabled(window,true);window.style&=~0x08c00000U;window.frame_insets.reset();gui_refresh_frame(window);present_gui(value);
        notify(sheet.target,-200,page_activate);return std::nullopt;
    }
    if(sheet.phase==page_activate){
        if(value){done(0);return std::nullopt;}
        for(std::size_t i=0;i<sheet.pages.size();++i){const auto window=sheet.pages[i].window;
            if(window && gui_.windows.contains(window)){gui_.windows.at(window).visible=i==sheet.target;present_gui(window);if(native_gui_)show_native_window(window,i==sheet.target);}
            if(const auto tab=gui_child(handle,static_cast<std::uint32_t>(0x5000+i));tab){gui_.windows.at(tab).check=i==sheet.target;present_gui(tab);}
        }
        sheet.selected=sheet.target;done(1);return std::nullopt;
    }
    if(sheet.phase==page_query_cancel){if(value){done(0);return std::nullopt;}sheet.index=0;sheet.phase=page_reset;}
    else if(sheet.phase==page_reset){++sheet.index;}
    else if(sheet.phase==page_apply){
        if(value){if(value==1){sheet.target=sheet.index;activate();}else done(0);return std::nullopt;}
        sheet.pages[sheet.index].dirty=false;++sheet.index;
    }else if(sheet.phase==page_kill){
        if(value){done(0);return std::nullopt;}
        if(sheet.action==0){activate();return std::nullopt;}
        sheet.index=0;sheet.phase=page_apply;
    }else if(!sheet.phase){
        if(sheet.action==4){notify(sheet.selected,-205,page_help);return std::nullopt;}
        if(sheet.action==2){
            if(sheet.pages[sheet.selected].window){notify(sheet.selected,-209,page_query_cancel);return std::nullopt;}
            sheet.index=0;sheet.phase=page_reset;
        }else if(sheet.pages[sheet.selected].window){notify(sheet.selected,-201,page_kill);return std::nullopt;}
        else {activate();return std::nullopt;}
    }
    if(sheet.phase==page_apply || sheet.phase==page_reset){
        while(sheet.index<sheet.pages.size() && !sheet.pages[sheet.index].window)++sheet.index;
        if(sheet.index<sheet.pages.size()){notify(sheet.index,sheet.phase==page_apply?-202:-203,sheet.phase);return std::nullopt;}
        if(sheet.phase==page_apply){sheet.saved=true;if(const auto apply=gui_child(handle,0x3021);apply){gui_set_window_enabled(gui_.windows.at(apply),false);present_gui(apply);}}
        if(sheet.action==1 || sheet.action==2){root.ended=true;root.dialog_result=sheet.saved?1:0;}
        done(1);return std::nullopt;
    }
    done(0);return std::nullopt;
}
std::optional<std::uint32_t> WindowsServices::invoke_property_sheet(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool wide=service.symbol.ends_with('W');
    const auto text=[&](std::uint64_t pointer){return pointer?(wide?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16_lossy(read_ansi(memory,pointer))):std::u16string{};};
    if(service.symbol.starts_with("DefDlgProc")){
        const auto handle=arg(0);auto& sheet=gui_.property_sheets.at(handle);const auto message=static_cast<std::uint32_t>(arg(1));
        if(message==0x81){arg.return_to(state,1);return std::nullopt;}
        if(message==0x82){release_gui_scratch(memory,state,sheet.scratch);gui_.property_sheets.erase(handle);arg.return_to(state,0);return std::nullopt;}
        if(message==2){std::vector<GuestCallback> callbacks;
            for(auto i=sheet.pages.rbegin();i!=sheet.pages.rend();++i)if(i->window && gui_.windows.contains(i->window)){const auto proc=gui_.windows.at(i->window).procedure;callbacks.push_back({proc,{i->window,2,0,0}});callbacks.push_back({proc,{i->window,0x82,0,0}});}
            for(const auto& page:sheet.pages)if(page.handle && gui_.property_pages.contains(page.handle)){gui_.property_pages.at(page.handle).used=false;callbacks.push_back({resolve("comctl32.dll","DestroyPropertySheetPage").value,{page.handle}});}
            start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;
        }
        if(message==0x468 || message==0x46d){ // PSM_CHANGED / PSM_UNCHANGED
            for(auto& page:sheet.pages)if(page.window==arg(2))page.dirty=message==0x468;
            if(const auto apply=gui_child(handle,0x3021);apply){gui_set_window_enabled(gui_.windows.at(apply),std::any_of(sheet.pages.begin(),sheet.pages.end(),[](const auto& p){return p.dirty;}));present_gui(apply);}
            arg.return_to(state,0);return std::nullopt;
        }
        if(message==0x476){arg.return_to(state,sheet.pages[sheet.selected].window);return std::nullopt;}
        if(sheet.phase){arg.return_to(state,0);return std::nullopt;}
        const auto command=arg(2)&65535U;
        if(message==0x110){sheet.action=0;return advance_property_sheet(state,memory,handle,0);}
        if(message==0x465 || (message==0x111 && command>=0x5000 && command<0x5000+sheet.pages.size())){
            const auto index=message==0x465?arg(2):command-0x5000;if(index>=sheet.pages.size()){arg.return_to(state,0);return std::nullopt;}
            sheet.target=static_cast<std::size_t>(index);sheet.action=0;return advance_property_sheet(state,memory,handle,0);
        }
        if(message==0x10 || message==0x46e || (message==0x111 && (command==1 || command==2 || command==9 || command==0x3021))){
            sheet.action=command==9 && message==0x111?4:message==0x10?2:message==0x46e?3:command==0x3021?3:static_cast<std::uint32_t>(command);
            return advance_property_sheet(state,memory,handle,0);
        }
        arg.return_to(state,0);return std::nullopt;
    }
    const auto fail=[&](std::uint32_t error){last_error_=error;arg.return_to(state,UINT64_MAX);};
    const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto header=arg(0);if(!header){fail(87);return std::nullopt;}
    const auto size=memory.load(GuestAddress{header},32);if(size<(x86_?40U:72U) || size>1024){fail(87);return std::nullopt;}memory.check(GuestAddress{header},static_cast<std::size_t>(size),Access::Read);
    const auto flags=memory.load(GuestAddress{header+4},32),count=memory.load(GuestAddress{header+(x86_?24:40)},32),owner=memory.load(GuestAddress{header+8},bits);
    // 一般 modal 支援描述陣列及 CreatePropertySheetPage 的 handle 陣列。
    if(flags&~UINT64_C(0x02000288)){fail(50);return std::nullopt;}
    if(!count || count>128 || (owner && !gui_.windows.contains(owner))){fail(87);return std::nullopt;}
    GuiPropertySheet sheet;sheet.wide=wide;sheet.target=static_cast<std::size_t>(memory.load(GuestAddress{header+(x86_?28:48)},32));if(sheet.target>=count)sheet.target=0;
    std::uint32_t width=360,height=200;auto address=memory.load(GuestAddress{header+(x86_?32:56)},bits);
    try {
        for(std::uint64_t i=0;i<count;++i){
            if(!(flags&8U)){
                const auto h=memory.load(GuestAddress{address},bits);const auto found=gui_.property_pages.find(h);
                if(found==gui_.property_pages.end() || found->second.used || found->second.phase){release_gui_scratch(memory,state,sheet.scratch);fail(6);return {};}
                auto page=found->second;const auto size2=memory.load(GuestAddress{page.descriptor},32);std::vector<std::uint8_t> data(size2);memory.read(GuestAddress{page.descriptor},data);page.descriptor=allocate_gui_scratch(memory,sheet.scratch,data);
                width=std::max(width,page.width);height=std::max(height,page.height);sheet.pages.push_back(std::move(page));address+=unit;continue;
            }
            const auto bytes=memory.load(GuestAddress{address},32);if(bytes<(x86_?40U:72U) || bytes>1024){release_gui_scratch(memory,state,sheet.scratch);fail(87);return std::nullopt;}
            std::vector<std::uint8_t> data(static_cast<std::size_t>(bytes));memory.read(GuestAddress{address},data);
            GuiPropertyPage page;page.wide=wide;page.descriptor=allocate_gui_scratch(memory,sheet.scratch,data);page.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{address+4},32));
            if(page.flags&~0x2fU){release_gui_scratch(memory,state,sheet.scratch);fail(50);return std::nullopt;}
            page.instance=memory.load(GuestAddress{address+8},bits);page.resource=memory.load(GuestAddress{address+8+unit},bits);page.procedure=memory.load(GuestAddress{address+8+4*unit},bits);
            if(!page.procedure){release_gui_scratch(memory,state,sheet.scratch);fail(87);return std::nullopt;}
            std::uint64_t templ=page.resource,extent=65536;
            if(!(page.flags&1U)){const auto entry=gui_resource(memory,page.instance,page.resource,5,wide);if(!entry){release_gui_scratch(memory,state,sheet.scratch);fail(1814);return std::nullopt;}templ=resource_image(entry,true)->base+memory.load(GuestAddress{entry},32);extent=memory.load(GuestAddress{entry+4},32);}
            const auto layout=gui_dialog_layout(memory,templ,extent);width=std::max(width,layout.width);height=std::max(height,layout.height);
            page.title=page.flags&8U?text(memory.load(GuestAddress{address+8+3*unit},bits)):layout.title;
            sheet.pages.push_back(std::move(page));address+=bytes;
        }
        if(width>4096 || height>4096){release_gui_scratch(memory,state,sheet.scratch);fail(87);return std::nullopt;}
        sheet.notification=allocate_gui_scratch(memory,sheet.scratch,{},32);
        const auto handle=allocate_gui_window();GuiWindow root;root.dpi_context=effective_dpi_context();root.thread=threads_.current;root.dialog=true;root.modal=true;root.owner=owner;root.class_name=u"#32770";root.style=0x80c80000U;root.procedure=resolve("user32.dll",wide?"DefDlgProcW":"DefDlgProcA").value;root.extra.resize(40);root.title=text(memory.load(GuestAddress{header+(x86_?20:32)},bits));root.width=width+16;root.height=height+90;root.x=80;root.y=80;root.default_id=1;
        std::vector<GuiWindowCreation> plans{{handle,root,wide}};
        const auto button=[&](std::uint32_t id,const std::u16string& title,std::uint32_t x,std::uint32_t y,std::uint32_t w,std::uint32_t style,bool enabled){GuiWindow child;child.dpi_context=effective_dpi_context();child.thread=threads_.current;child.parent=handle;child.id=id;child.title=title;child.class_name=child.control_class=u"button";child.builtin=true;child.procedure=stock_gui_proc(u"button",wide);child.style=0x50010000U|style;child.visible=true;gui_set_window_enabled(child,enabled);child.x=static_cast<std::int32_t>(x);child.y=static_cast<std::int32_t>(y);child.width=w;child.height=28;plans.push_back({allocate_gui_window(),child,wide});};
        std::uint32_t x=8,y=8;for(std::size_t i=0;i<sheet.pages.size();++i){const auto& title=sheet.pages[i].title;const auto measured=native_gui_?native_gui_->text_width(GuiFont{},title).value_or(static_cast<std::int32_t>(title.size()*8)):static_cast<std::int32_t>(title.size()*8);const auto w=static_cast<std::uint32_t>(std::clamp(measured+24,60,static_cast<int>(width)));if(x+w>width+8){x=8;y+=30;}button(static_cast<std::uint32_t>(0x5000+i),title,x,y,w,0x1003U,true);x+=w+2;}
        sheet.page_y=y+36;plans.front().window.height=height+sheet.page_y+50;const auto bottom=height+sheet.page_y+10;
        if((flags&0x200U) || std::any_of(sheet.pages.begin(),sheet.pages.end(),[](const auto& page){return (page.flags&0x20U)!=0;}))button(9,u"Help",8,bottom,80,0,true);
        button(1,u"OK",width-254,bottom,80,1,true);button(2,u"Cancel",width-168,bottom,80,0,true);if(!(flags&0x80U))button(0x3021,u"Apply",width-82,bottom,80,0,false);
        for(const auto& page:sheet.pages)if(page.handle)gui_.property_pages.at(page.handle).used=true;
        gui_.property_sheets.emplace(handle,std::move(sheet));return start_gui_creation(state,memory,std::move(plans),0);
    }catch(...){release_gui_scratch(memory,state,sheet.scratch);throw;}
}
}
