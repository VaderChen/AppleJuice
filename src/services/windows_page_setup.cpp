#include "x64arm/windows_services.hpp"
#include "x64arm/windows_window_state.hpp"
#include "x64arm/windows_gui_abi.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <charconv>

namespace x64arm {
namespace {
struct PageLayout {
    unsigned size,owner,mode,names,flags,paper,minimum,margins,instance,hook,paint,templ,resource;
    explicit PageLayout(bool small):size(small?84:128),owner(small?4:8),mode(small?8:16),names(small?12:24),flags(small?16:32),paper(small?20:36),minimum(small?28:44),margins(small?44:60),instance(small?60:80),hook(small?68:96),paint(small?72:104),templ(small?76:112),resource(small?80:120){}
};
std::int32_t page_units(std::uint32_t microns,std::uint32_t flags){return static_cast<std::int32_t>(flags&8U?(microns+5)/10:(std::uint64_t{microns}*10+127)/254);}
std::array<std::int32_t,2> paper_size(const GuiPageSetup& data){std::array size{page_units(data.settings.paper_width,data.flags),page_units(data.settings.paper_height,data.flags)};if(data.settings.landscape)std::swap(size[0],size[1]);return size;}
std::u16string margin_text(std::int32_t value,std::uint32_t flags){const int unit=flags&8U?100:1000;auto fraction=std::to_string(value%unit);fraction.insert(0,(flags&8U?2U:3U)-fraction.size(),'0');return utf8_to_utf16_lossy(std::to_string(value/unit)+"."+fraction);}
std::optional<std::int32_t> margin_value(std::u16string_view text,std::uint32_t flags){
    const auto s=utf16_to_utf8(text);const auto dot=s.find('.');const auto whole=std::string_view(s).substr(0,dot);std::uint32_t value{};
    const auto result=std::from_chars(whole.data(),whole.data()+whole.size(),value);if(result.ec!=std::errc{} || result.ptr!=whole.data()+whole.size())return {};
    const unsigned places=flags&8U?2:3,unit=flags&8U?100:1000;std::uint32_t part=0;
    if(dot!=s.npos){const auto fraction=std::string_view(s).substr(dot+1);if(fraction.size()>places)return {};for(const auto c:fraction){if(c<'0' || c>'9')return {};part=part*10+static_cast<unsigned>(c-'0');}for(auto i=fraction.size();i<places;++i)part*=10;}
    const auto total=std::uint64_t{value}*unit+part;if(total>INT32_MAX)return {};return static_cast<std::int32_t>(total);
}
bool valid_margins(const GuiPageSetup& data){const auto size=paper_size(data);for(unsigned i=0;i<4;++i)if(data.minimum[i]<0 || data.margins[i]<data.minimum[i])return false;return std::int64_t{data.margins[0]}+data.margins[2]<size[0] && std::int64_t{data.margins[1]}+data.margins[3]<size[1];}
std::vector<std::uint8_t> page_template(std::uint32_t flags){
    std::vector<std::uint8_t> bytes;const auto put=[&](std::uint32_t value,unsigned count){for(unsigned i=0;i<count;++i)bytes.push_back(static_cast<std::uint8_t>(value>>(8*i)));};
    const auto text=[&](std::u16string_view value){for(const auto c:value)put(c,2);put(0,2);};
    put(0x90c80040U,4);put(1,4);put(20+(flags&0x800U?1:0),2);put(30,2);put(30,2);put(330,2);put(252,2);put(0,2);put(0,2);text(u"版面設定");put(9,2);text(u"Segoe UI");
    const auto item=[&](unsigned id,unsigned cls,std::u16string_view label,unsigned x,unsigned y,unsigned w,unsigned h,unsigned style=0){while(bytes.size()%4)put(0,1);put(0x50000000U|style,4);put(0,4);put(x,2);put(y,2);put(w,2);put(h,2);put(id,2);put(0xffff,2);put(cls,2);text(label);put(0,2);};
    item(0xffff,0x82,u"印表機：Win64 PDF",10,8,300,12);
    item(0xffff,0x82,u"紙張大小",10,30,64,12);item(0x471,0x85,u"",78,28,126,90,0x10003);
    item(0xffff,0x82,u"紙張來源",10,53,64,12);item(0x472,0x85,u"",78,51,126,70,0x10003);
    item(0x420,0x80,u"直向",215,29,50,14,0x30009);item(0x421,0x80,u"橫向",270,29,50,14,0x10009);
    item(0x440,0x82,u"",10,79,190,130,0);
    item(0xffff,0x82,flags&8U?u"邊界（毫米）":u"邊界（英吋）",215,79,100,14);
    constexpr std::array<std::u16string_view,4> labels{u"左",u"上",u"右",u"下"};
    for(unsigned i=0;i<4;++i){item(0xffff,0x82,labels[i],215,100+i*24,25,14);item(0x480+i,0x81,u"",242,98+i*24,72,16,0x810080);}
    item(499,0x82,u"",10,211,310,14);item(1,0x80,u"確定",210,230,50,16,0x10001);item(2,0x80,u"取消",266,230,50,16,0x10000);
    if(flags&0x800U)item(0x40e,0x80,u"說明",10,230,50,16,0x10000);return bytes;
}
}
std::uint32_t WindowsServices::commit_page_setup(GuestState& state,GuestMemory& memory,const GuiPageSetup& data){
    const PageLayout layout(x86_);const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto p=data.request;
    memory.check(GuestAddress{p},layout.size,Access::Write);
    const std::array old{memory.load(GuestAddress{p+layout.mode},bits),memory.load(GuestAddress{p+layout.names},bits)};
    if(old[0] && old[0]==old[1])return 10;
    for(const auto h:old)if(h){const auto f=gui_.global_memory.find(h);if(f==gui_.global_memory.end() || f->second.locks || f->second.clipboard_owned)return 10;}
    const auto handles=allocate_printer_globals(state,memory,data.wide,data.settings);if(!handles[0])return 9;
    for(const auto h:old)if(h){const auto at=gui_.global_memory.at(h).address;gui_.global_memory.erase(h);(void)free_heap(memory,at,state);}
    memory.store(GuestAddress{p+layout.mode},bits,handles[0]);memory.store(GuestAddress{p+layout.names},bits,handles[1]);memory.store(GuestAddress{p+layout.flags},32,data.flags);
    const auto paper=paper_size(data);for(unsigned i=0;i<2;++i)memory.store(GuestAddress{p+layout.paper+i*4},32,static_cast<std::uint32_t>(paper[i]));
    for(unsigned i=0;i<4;++i){memory.store(GuestAddress{p+layout.minimum+i*4},32,static_cast<std::uint32_t>(data.minimum[i]));memory.store(GuestAddress{p+layout.margins+i*4},32,static_cast<std::uint32_t>(data.margins[i]));}return 0;
}
std::optional<std::uint32_t> WindowsServices::invoke_page_setup(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto bits=static_cast<std::uint8_t>(x86_?32:64);const PageLayout layout(x86_);
    if(service.family=="internal-page-setup"){
        const auto root=arg(0);const auto message=static_cast<std::uint32_t>(arg(1));
        if(message==0x110){const auto found=gui_.page_setups.find(arg(3));if(found==gui_.page_setups.end()){arg.return_to(state,0);return {};}found->second.window=root;
            auto& data=found->second;const auto child=[&](unsigned id)->GuiWindow*{const auto h=gui_child(root,id);return h?&gui_.windows.at(h):nullptr;};
            if(auto* paper=child(0x471)){paper->items.clear();for(const auto& form:data.forms){if(form.paper==data.settings.paper)paper->selection=static_cast<int>(paper->items.size());paper->items.push_back({gui_.next_item++,0,form.paper,form.form.empty()?u"自訂尺寸":form.form,0});}}
            if(auto* source=child(0x472)){source->items={{gui_.next_item++,0,7,u"自動選擇",0}};source->selection=0;}
            for(unsigned i=0;i<4;++i)if(auto* edit=child(0x480+i))edit->title=margin_text(data.margins[i],data.flags);
            for(unsigned id:{0x420U,0x421U})if(auto* radio=child(id))radio->check=(id==0x421)==data.settings.landscape;
            for(auto& [h,w]:gui_.windows)if(w.parent==root){const auto id=w.id;const auto disabled=(id>=0x480 && id<=0x483)?0x10U:(id==0x420 || id==0x421)?0x100U:(id==0x471 || id==0x472)?0x200U:0U;if(disabled && (data.flags&disabled))gui_set_window_enabled(w,false);present_gui(h);}
            data.preview=gui_child(root,0x440);
        }
        const auto found=std::find_if(gui_.page_setups.begin(),gui_.page_setups.end(),[&](const auto& entry){return entry.second.window==root;});
        if(found==gui_.page_setups.end()){arg.return_to(state,0);return {};}
        const auto id=found->first;const auto& data=found->second;
        if(data.hook){start_callbacks(state,memory,{{data.hook,{root,message,arg(2),message==0x110?data.request:arg(3)}}},Continuation::PageSetupHook,id);return {};}
        return page_setup_message(state,memory,id,{root,arg(2),arg(3),message});
    }
    gui_.common_dialog_error=0;const auto p=arg(0);std::uint32_t error=2;GuiPageSetup data;
    const auto fail=[&](std::uint32_t code){gui_.common_dialog_error=code;arg.return_to(state,0);return std::optional<std::uint32_t>{};};
    try {
        if(!p)return fail(error);if(memory.load(GuestAddress{p},32)!=layout.size)return fail(1);memory.check(GuestAddress{p},layout.size,Access::Write);
        const auto get=[&](unsigned offset,bool pointer=false){return memory.load(GuestAddress{p+offset},pointer?bits:32);};
        data.request=p;data.wide=service.symbol.ends_with('W');data.owner=get(layout.owner,true);data.flags=static_cast<std::uint32_t>(get(layout.flags));data.instance=get(layout.instance,true);
        if((data.flags&~0x2eafbfU) || (data.flags&12U)==12U)return fail(2);
        if(data.owner && !gui_.windows.contains(data.owner))return fail(2);
        if((data.flags&0x800U) && !data.owner)return fail(2);
        data.hook=data.flags&0x2000U?get(layout.hook,true):0;data.paint_hook=data.flags&0x40000U?get(layout.paint,true):0;
        if(((data.flags&0x2000U) && !data.hook) || ((data.flags&0x40000U) && !data.paint_hook))return fail(11);
        for(const auto h:{data.hook,data.paint_hook})if(h && !handles(GuestAddress{h}))memory.check(GuestAddress{h},1,Access::Execute);
        const auto mode=get(layout.mode,true),names=get(layout.names,true);if((data.flags&0x400U) && (mode || names))return fail(0x1003);
        if(mode && mode==names)return fail(10);
        error=10;for(const auto h:{mode,names})if(h){const auto f=gui_.global_memory.find(h);if(f==gui_.global_memory.end() || !(f->second.flags&2U) || !f->second.address || f->second.locks || f->second.clipboard_owned)return fail(error);}
        error=0x1005;if(mode){const auto& b=gui_.global_memory.at(mode);if(b.size<(data.wide?104U:72U))return fail(error);const auto size=memory.load(GuestAddress{b.address+(data.wide?68:36)},16),extra=memory.load(GuestAddress{b.address+(data.wide?70:38)},16);if(size+extra>b.size || read_printer_settings(memory,b.address,data.wide,data.settings))return fail(error);}
        error=0x100b;if(names){const auto& b=gui_.global_memory.at(names);if(b.size<8)return fail(error);const unsigned unit=data.wide?2:1;
            for(unsigned field:{0U,2U,4U}){const auto offset=memory.load(GuestAddress{b.address+field},16)*unit;if(offset<8 || offset>=b.size)return fail(error);std::u16string value;bool terminated=false;
                for(auto at=offset;at+unit<=b.size;at+=unit){const auto c=memory.load(GuestAddress{b.address+at},static_cast<std::uint8_t>(unit*8));if(!c){terminated=true;break;}value.push_back(static_cast<char16_t>(c));}
                if(!terminated || (field==2 && value!=u"Win64 PDF") || (field==4 && value!=u"FILE:"))return fail(error);}}
        unsigned forms_error=0;data.forms=available_printer_settings(forms_error);if(forms_error || data.forms.empty())return fail(0x1008);if(!data.settings.paper)data.forms.push_back(data.settings);
        error=9;if(gui_.page_setups.size()>=64)return fail(error);data.locale_buffer=allocate_gui_scratch(memory,data.scratch,{},4);
        const auto id=gui_.next_object++;gui_.page_setups.emplace(id,std::move(data));const auto& saved=gui_.page_setups.at(id);
        std::vector<GuestCallback> callbacks;if(!(saved.flags&12U))callbacks.push_back({resolve("kernel32.dll","GetLocaleInfoW").value,{0x400,0x2000000d,saved.locale_buffer,2}});
        start_callbacks(state,memory,std::move(callbacks),Continuation::PageSetupStart,id);return {};
    }catch(const std::bad_alloc&){release_gui_scratch(memory,state,data.scratch);return fail(9);}catch(const MemoryFault&){release_gui_scratch(memory,state,data.scratch);return fail(error);}catch(const HostCallFault&){release_gui_scratch(memory,state,data.scratch);return fail(error);}catch(const std::invalid_argument&){release_gui_scratch(memory,state,data.scratch);return fail(error);}
}
std::optional<std::uint32_t> WindowsServices::resume_page_setup(GuestState& state,GuestMemory& memory,const CallbackFrame& frame,std::uint64_t value){
    const auto id=frame.value;const auto found=gui_.page_setups.find(id);if(found==gui_.page_setups.end()){complete_call(state,memory,0);return {};}
    auto& data=found->second;const PageLayout layout(x86_);const auto bits=static_cast<std::uint8_t>(x86_?32:64);
    const auto finish=[&](std::uint32_t error,bool success){gui_.common_dialog_error=error;release_gui_scratch(memory,state,data.scratch);gui_.page_setups.erase(id);complete_call(state,memory,success?1:0);};
    if(frame.continuation==Continuation::PageSetupResult){if(value==1){try{const auto error=commit_page_setup(state,memory,data);finish(error,!error);}catch(const MemoryFault&){finish(2,false);}catch(const std::bad_alloc&){finish(9,false);}return {};}finish(static_cast<std::uint32_t>(value)==UINT32_MAX?0xffff:0,false);return {};}
    if(frame.continuation==Continuation::PageSetupHook){const WindowsCall arg(frame.saved,memory,4);if(arg(1)==0x110){enqueue_gui_message({data.window,0,0,0xf});complete_call(state,memory,value);return {};}if(value){complete_call(state,memory,value);return {};}return page_setup_message(state,memory,id,{arg(0),arg(2),arg(3),static_cast<std::uint32_t>(arg(1))});}
    if(frame.continuation==Continuation::PageSetupStart){
        std::uint32_t error=2;
        try {
            if(!(data.flags&12U)){if(!value){finish(2,false);return {};}data.flags|=memory.load(GuestAddress{data.locale_buffer},32)?4U:8U;}
            for(unsigned i=0;i<4;++i){data.minimum[i]=data.flags&1U?std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{data.request+layout.minimum+i*4},32))):0;data.margins[i]=data.flags&2U?std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{data.request+layout.margins+i*4},32))):(data.flags&8U?2540:1000);}
            if(!valid_margins(data)){finish(0x1002,false);return {};}
            if(data.flags&0x400U){const auto code=commit_page_setup(state,memory,data);finish(code,!code);return {};}
            if(data.flags&0x20000U){error=8;const auto h=memory.load(GuestAddress{data.request+layout.resource},bits);const auto f=gui_.global_memory.find(h);if(f==gui_.global_memory.end() || !f->second.address){finish(error,false);return {};}data.source=f->second.address;(void)gui_dialog_layout(memory,data.source,f->second.size);}
            else if(data.flags&0x8000U){error=4;if(!data.instance){finish(error,false);return {};}const auto name=memory.load(GuestAddress{data.request+layout.templ},bits);if(!name){finish(3,false);return {};}error=6;const auto entry=gui_resource(memory,data.instance,name,5,data.wide);if(!entry){finish(error,false);return {};}data.source=resource_image(entry,true)->base+memory.load(GuestAddress{entry},32);(void)gui_dialog_layout(memory,data.source,memory.load(GuestAddress{entry+4},32));}
            else{error=9;const auto bytes=page_template(data.flags);data.source=allocate_gui_scratch(memory,data.scratch,bytes,bytes.size());}
            start_callbacks(state,memory,{{resolve("user32.dll",data.wide?"DialogBoxIndirectParamW":"DialogBoxIndirectParamA").value,{data.instance,data.source,data.owner,resolve("internal-page-setup","DialogProc").value,id}}},Continuation::PageSetupResult,id);return {};
        }catch(const MemoryFault&){finish(error,false);return {};}catch(const HostCallFault&){finish(error,false);return {};}catch(const std::bad_alloc&){finish(9,false);return {};}catch(const std::invalid_argument&){finish(error,false);return {};}
    }
    // 預覽沿用客體 GDI 的 DC、繪圖與回呼；不另外建立宿主繪圖後台。
    const auto api=[&](std::string_view dll,std::string_view name,std::vector<std::uint64_t> args){return GuestCallback{resolve(dll,name).value,std::move(args)};};
    const auto close_paint=[&]{data.painting=false;std::vector<GuestCallback> calls;if(data.paint_dc){const auto saved=memory.load(GuestAddress{data.paint_buffer+112},32);if(saved)calls.push_back(api("gdi32.dll","RestoreDC",{data.paint_dc,saved}));calls.push_back(api("user32.dll","ReleaseDC",{data.preview,data.paint_dc}));}calls.push_back(api("user32.dll","ValidateRect",{data.window,0}));start_callbacks(state,memory,std::move(calls),Continuation::Return,1);};
    if(data.paint_stage==0){data.paint_dc=value;if(!value){close_paint();return {};}data.paint_stage=UINT32_MAX;start_callbacks(state,memory,{api("gdi32.dll","SaveDC",{value})},Continuation::PageSetupPaint,id);return {};}
    if(data.paint_stage==UINT32_MAX){memory.store(GuestAddress{data.paint_buffer+112},32,value);data.paint_stage=1;}
    else if(data.paint_stage%2==0){ // 已收到 PagePaintHook 的回覆。
        const auto message=(data.paint_stage-2)/2;
        if(value && message<3){close_paint();return {};}
        std::vector<GuestCallback> draws;
        if(!value && !(data.flags&0x80000U)){
            const auto rect=data.paint_buffer+(message==0?0:(message-1)*16);const auto dc=data.paint_dc;
            if(message==1){draws.push_back(api("user32.dll","FillRect",{dc,data.paint_buffer+96,0xb002}));draws.push_back(api("user32.dll","FillRect",{dc,rect,0xb000}));draws.push_back(api("user32.dll","FrameRect",{dc,rect,0xb004}));}
            if(message==3)draws.push_back(api("user32.dll","DrawFocusRect",{dc,rect}));
            if(message==4)draws.push_back(api("user32.dll","DrawTextW",{dc,data.paint_buffer+128,UINT32_MAX,rect,0x10}));
        }
        ++data.paint_stage;if(!draws.empty()){start_callbacks(state,memory,std::move(draws),Continuation::PageSetupPaint,id);return {};}
    }
    for(;;){const auto message=(data.paint_stage-1)/2;if(message>6){close_paint();return {};}
        if(message==5){data.paint_stage+=2;continue;} // 既有後台紙張目錄沒有信封。
        const auto parameter=message==0?(data.settings.paper|((data.settings.landscape?1ULL:5ULL)<<16)):data.paint_dc;
        const auto pointer=message==0?data.request:data.paint_buffer+(message-1)*16;
        ++data.paint_stage;
        start_callbacks(state,memory,data.paint_hook?std::vector<GuestCallback>{{data.paint_hook,{data.window,0x400+message,parameter,pointer}}}:std::vector<GuestCallback>{},Continuation::PageSetupPaint,id);return {};
    }
}
std::optional<std::uint32_t> WindowsServices::page_setup_message(GuestState& state,GuestMemory& memory,std::uint64_t id,const GuiMessage& msg){
    auto& data=gui_.page_setups.at(id);const auto root=data.window;
    const auto done=[&](std::uint64_t result){complete_call(state,memory,result);return std::optional<std::uint32_t>{};};
    if(!gui_.windows.contains(root))return done(0);
    if(msg.message==0x10 || (msg.message==0x111 && ((msg.wparam&65535U)==2 || (msg.wparam&65535U)==3))){gui_.windows.at(root).ended=true;gui_.windows.at(root).dialog_result=0;return done(1);}
    const auto child=[&](unsigned n)->GuiWindow*{const auto h=gui_child(root,n);return h?&gui_.windows.at(h):nullptr;};
    const auto repaint=[&]{enqueue_gui_message({root,0,0,0xf});};
    if(msg.message==0x110){repaint();return done(1);}
    if(msg.message==0x111){const auto control=static_cast<unsigned>(msg.wparam&65535U);
        if(control==0x40e && (data.flags&0x800U) && data.owner){const auto message=gui_registered_message(u"commdlg_help");start_callbacks(state,memory,{{resolve("user32.dll","SendMessageW").value,{data.owner,message,root,data.request}}},Continuation::Return,1);return {};}
        auto updated=data;
        if(!(data.flags&0x100U) && (control==0x420 || control==0x421)){updated.settings.landscape=control==0x421;for(unsigned n:{0x420U,0x421U})if(auto* radio=child(n)){radio->check=n==control;present_gui(gui_child(root,n));}}
        if(!(data.flags&0x200U))if(const auto* paper=child(0x471);paper && paper->selection>=0 && static_cast<std::size_t>(paper->selection)<paper->items.size()){const auto number=paper->items[static_cast<std::size_t>(paper->selection)].data;for(const auto& form:data.forms)if(form.paper==number){updated.settings.paper=form.paper;updated.settings.form=form.form;updated.settings.paper_width=form.paper_width;updated.settings.paper_height=form.paper_height;break;}}
        bool valid=true;for(unsigned i=0;i<4;++i)if(!(data.flags&0x10U))if(const auto* edit=child(0x480+i)){const auto value=margin_value(edit->title,data.flags);if(!value)valid=false;else updated.margins[i]=*value;}
        // 無效的編輯不寫回呼叫者；確定前仍可重新編輯或取消。
        if(control==1 && (!valid || !valid_margins(updated))){if(auto* status=child(499)){status->title=u"邊界須大於最小值，且不能超過紙張尺寸。";present_gui(gui_child(root,499));}return done(1);}
        data.settings=updated.settings;if(valid)data.margins=updated.margins;
        data.settings.width=static_cast<unsigned>((std::uint64_t{data.settings.paper_width}*96+12700)/25400);data.settings.height=static_cast<unsigned>((std::uint64_t{data.settings.paper_height}*96+12700)/25400);if(data.settings.landscape)std::swap(data.settings.width,data.settings.height);
        if(control==1){gui_.windows.at(root).ended=true;gui_.windows.at(root).dialog_result=1;}else if(!data.painting)repaint();return done(1);
    }
    if(msg.message==0xf && !data.painting && data.preview && gui_.windows.contains(data.preview)){
        const auto& view=gui_.windows.at(data.preview);const auto size=paper_size(data);const auto width=std::max(1,static_cast<int>(std::min(4096U,view.width))-12),height=std::max(1,static_cast<int>(std::min(4096U,view.height))-12);
        const double scale=std::min(static_cast<double>(width)/size[0],static_cast<double>(height)/size[1]);const int w=std::max(1,static_cast<int>(size[0]*scale)),h=std::max(1,static_cast<int>(size[1]*scale)),x=(width-w)/2+6,y=(height-h)/2+6;
        const std::array<std::int32_t,4> full{x,y,x+w,y+h};auto minimum=full,margin=full;for(unsigned i=0;i<4;++i){const int sign=i<2?1:-1;minimum[i]+=sign*static_cast<int>(data.minimum[i]*scale);margin[i]+=sign*static_cast<int>(data.margins[i]*scale);}
        if(!data.paint_buffer){const auto bytes=utf16_bytes(u"The quick brown fox jumps over the lazy dog.\n0123456789  ABCDEFGHIJKLMNOPQRSTUVWXYZ\nThe quick brown fox jumps over the lazy dog.");data.paint_buffer=allocate_gui_scratch(memory,data.scratch,{},128+bytes.size());memory.write(GuestAddress{data.paint_buffer+128},bytes);}
        const std::array rects{full,minimum,margin,margin,full,full,std::array<std::int32_t,4>{0,0,static_cast<std::int32_t>(view.width),static_cast<std::int32_t>(view.height)}};for(unsigned n=0;n<rects.size();++n)for(unsigned i=0;i<4;++i)memory.store(GuestAddress{data.paint_buffer+n*16+i*4},32,static_cast<std::uint32_t>(rects[n][i]));
        data.painting=true;data.paint_stage=0;start_callbacks(state,memory,{{resolve("user32.dll","GetDC").value,{data.preview}}},Continuation::PageSetupPaint,id);return {};
    }
    return done(0);
}
}
