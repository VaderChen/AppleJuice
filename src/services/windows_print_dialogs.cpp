#include "x64arm/windows_services.hpp"
#include "x64arm/windows_window_state.hpp"
#include "x64arm/windows_printing.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/windows_geometry.hpp"
#include <algorithm>
#include <charconv>

namespace x64arm {
namespace {
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,handle_error=0x80070006U;
constexpr std::array<std::uint8_t,16> callback_iid{0xc3,0xa2,0x52,0x58,0x30,0x65,0xd1,0x11,0xb6,0xa3,0,0,0xf8,0x75,0x7b,0xf9};
constexpr std::array<std::uint8_t,16> services_iid{0xda,0xae,0x9a,0x50,0x39,0x56,0xd1,0x11,0xb6,0xa1,0,0,0xf8,0x75,0x7b,0xf9};
constexpr std::array<std::uint8_t,16> site_iid{0xa3,0x09,0xc7,0xfc,0x14,0xb8,0xd0,0x11,0xa3,0xa4,0,0xaa,0,0x6c,0,0x98};
constexpr std::array<std::uint8_t,16> unknown_iid{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
struct Layout {
    unsigned size,owner,mode,names,dc,flags,flags2,exclude,count,capacity,ranges,minimum,maximum,copies,instance,templ,callback,pages,handles,start,action;
    explicit Layout(bool small):Layout(small?Layout{84,4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,72,76,80}:Layout{136,8,16,24,32,40,44,48,52,56,64,72,76,80,88,96,104,112,120,128,132}){}
    Layout(unsigned a,unsigned b,unsigned c,unsigned d,unsigned e,unsigned f,unsigned g,unsigned h,unsigned i,unsigned j,unsigned k,unsigned l,unsigned m,unsigned n,unsigned o,unsigned q,unsigned r,unsigned s,unsigned t,unsigned u,unsigned v):size(a),owner(b),mode(c),names(d),dc(e),flags(f),flags2(g),exclude(h),count(i),capacity(j),ranges(k),minimum(l),maximum(m),copies(n),instance(o),templ(q),callback(r),pages(s),handles(t),start(u),action(v){}
};
std::uint32_t hresult(unsigned error){return error?0x80070000U|(error&65535U):0;}
std::optional<unsigned> integer(std::u16string_view text){const auto bytes=utf16_to_utf8(text);unsigned value{};const auto parsed=std::from_chars(bytes.data(),bytes.data()+bytes.size(),value);return parsed.ec==std::errc{} && parsed.ptr==bytes.data()+bytes.size()?std::optional{value}:std::nullopt;}
std::u16string number(unsigned value){return utf8_to_utf16_lossy(std::to_string(value));}
GuestCallback method(GuestMemory& memory,bool small,std::uint64_t object,unsigned slot,std::vector<std::uint64_t> args={}){
    const unsigned unit=small?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    if(!object)throw HostCallFault("列印回呼的 COM 物件為空");const auto table=memory.load(GuestAddress{object},bits);
    const auto address=memory.load(GuestAddress{table+slot*unit},bits);if(!address)throw HostCallFault("列印回呼的 COM 方法為空");
    args.insert(args.begin(),object);return {address,std::move(args)};
}
std::vector<std::uint8_t> devnames(bool wide){
    std::vector<std::uint8_t> bytes(8);const unsigned unit=wide?2:1;
    const auto append=[&](unsigned at,std::u16string_view text){const auto offset=bytes.size()/unit;bytes[at]=static_cast<std::uint8_t>(offset);bytes[at+1]=static_cast<std::uint8_t>(offset>>8);
        if(wide){const auto data=utf16_bytes(text);bytes.insert(bytes.end(),data.begin(),data.end());}else{const auto data=utf16_to_utf8(text);bytes.insert(bytes.end(),data.begin(),data.end());bytes.push_back(0);}};
    append(0,u"CoreGraphics PDF");append(2,u"Win64 PDF");append(4,u"FILE:");bytes[6]=1;return bytes;
}
}
std::span<const WindowsServiceDescriptor> print_dialog_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"comdlg32.dll","PrintDlgExA",1},{"comdlg32.dll","PrintDlgExW",1},
        {"comctl32.dll","CreatePropertySheetPageA",1},{"comctl32.dll","CreatePropertySheetPageW",1},{"comctl32.dll","DestroyPropertySheetPage",1},
        {"internal-print-dialog","RootProc",4},{"internal-print-dialog","GeneralProc",4},
        {"internal-print-services","QueryInterface",3},{"internal-print-services","AddRef",1},{"internal-print-services","Release",1},
        {"internal-print-services","GetCurrentDevMode",3},{"internal-print-services","GetCurrentPrinterName",3},{"internal-print-services","GetCurrentPortName",3}
    };return table;
}
std::optional<GuiPropertyPage> WindowsServices::read_property_page(GuestMemory& memory,std::uint64_t at,bool wide,std::uint32_t& error){
    error=87;if(!at)return {};const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto size=memory.load(GuestAddress{at},32);if(size<(x86_?40U:72U) || size>1024)return {};memory.check(GuestAddress{at},size,Access::Read);
    GuiPropertyPage page;page.wide=wide;page.flags=static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32));
    if(page.flags&~0xe9U){error=50;return {};}
    page.instance=memory.load(GuestAddress{at+8},bits);page.resource=memory.load(GuestAddress{at+8+unit},bits);
    page.procedure=memory.load(GuestAddress{at+8+4*unit},bits);if(!page.procedure)return {};
    if(page.flags&0x80U){page.callback=memory.load(GuestAddress{at+8+6*unit},bits);if(!page.callback)return {};}
    if(page.flags&0x40U){page.ref_parent=memory.load(GuestAddress{at+8+7*unit},bits);if(!page.ref_parent)return {};memory.check(GuestAddress{page.ref_parent},4,Access::Write);if(memory.load(GuestAddress{page.ref_parent},32)==UINT32_MAX)return {};}
    auto templ=page.resource;std::uint64_t extent=65536;
    if(!(page.flags&1U)){const auto entry=gui_resource(memory,page.instance,page.resource,5,wide);if(!entry){error=1814;return {};}templ=resource_image(entry,true)->base+memory.load(GuestAddress{entry},32);extent=memory.load(GuestAddress{entry+4},32);}
    const auto layout=gui_dialog_layout(memory,templ,extent);page.width=layout.width;page.height=layout.height;
    if(page.width>4096 || page.height>4096)return {};
    const auto title=memory.load(GuestAddress{at+8+3*unit},bits);page.title=(page.flags&8U) && title?(wide?read_utf16(memory,GuestAddress{title}):utf8_to_utf16_lossy(read_ansi(memory,title))):layout.title;
    error=0;return page;
}
std::optional<std::uint32_t> WindowsServices::invoke_property_page(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);
    if(service.symbol=="DestroyPropertySheetPage"){
        const auto found=gui_.property_pages.find(arg(0));if(found==gui_.property_pages.end() || found->second.used || found->second.phase){last_error_=6;arg.return_to(state,0);return {};}
        auto& page=found->second;page.phase=2;
        start_callbacks(state,memory,page.callback?std::vector<GuestCallback>{{page.callback,{0,1,page.descriptor}}}:std::vector<GuestCallback>{},Continuation::PrintPropertyPageOperation,arg(0));return {};
    }
    std::uint32_t error{};auto page=read_property_page(memory,arg(0),service.symbol.ends_with('W'),error);
    if(!page || gui_.property_pages.size()>=256){last_error_=page?8:error;arg.return_to(state,0);return {};}
    const auto size=memory.load(GuestAddress{arg(0)},32);std::vector<std::uint8_t> data(size);memory.read(GuestAddress{arg(0)},data);
    const auto descriptor=allocate_heap(memory,size);if(!descriptor){last_error_=8;arg.return_to(state,0);return {};}
    memory.write(GuestAddress{descriptor},data);page->descriptor=descriptor;page->phase=1;page->handle=gui_.next_object++;
    if(page->ref_parent)memory.store(GuestAddress{page->ref_parent},32,memory.load(GuestAddress{page->ref_parent},32)+1);
    const auto handle=page->handle;gui_.property_pages.emplace(handle,*page);
    start_callbacks(state,memory,page->callback?std::vector<GuestCallback>{{page->callback,{0,0,descriptor}}}:std::vector<GuestCallback>{},Continuation::PrintPropertyPageOperation,handle);return {};
}
std::optional<std::uint32_t> WindowsServices::resume_property_page(GuestState& state,GuestMemory& memory,std::uint64_t handle,std::uint64_t){
    const auto found=gui_.property_pages.find(handle);if(found==gui_.property_pages.end()){complete_call(state,memory,0);return {};}
    auto& page=found->second;if(page.phase==1){page.phase=0;complete_call(state,memory,handle);return {};}
    const auto descriptor=page.descriptor,ref=page.ref_parent;gui_.property_pages.erase(found);
    if(ref){const auto value=memory.load(GuestAddress{ref},32);if(value)memory.store(GuestAddress{ref},32,value-1);}
    (void)free_heap(memory,descriptor,state);complete_call(state,memory,1);return {};
}
std::uint64_t WindowsServices::release_print_services(GuestState& state,GuestMemory& memory,std::uint64_t object){
    const auto found=gui_.print_services.find(object);if(found==gui_.print_services.end() || !found->second.references)return 0;
    const auto references=--found->second.references;if(!references){gui_.print_services.erase(found);(void)free_heap(memory,object,state);}return references;
}
std::uint32_t WindowsServices::commit_print_dialog(GuestState& state,GuestMemory& memory,GuiPrintDialog& dialog){
    const Layout layout(x86_);const auto bits=static_cast<std::uint8_t>(x86_?32:64);const auto p=dialog.request;
    std::vector<std::uint64_t> new_handles;std::uint64_t dc{};
    const auto discard=[&]{if(dc){delete_printer_dc(dc);gui_.contexts.erase(dc);}for(const auto h:new_handles){const auto at=gui_.global_memory.at(h).address;gui_.global_memory.erase(h);(void)free_heap(memory,at,state);}};
    const auto global=[&](std::span<const std::uint8_t> data){if(gui_.global_memory.size()>=65536)return std::uint64_t{};const auto at=allocate_heap(memory,data.size());if(!at)return std::uint64_t{};memory.write(GuestAddress{at},data);const auto h=gui_.next_object++;gui_.global_memory.emplace(h,GuiGlobalMemory{at,data.size(),2,0,false});new_handles.push_back(h);return h;};
    try {
        for(const auto offset:{layout.mode,layout.names}){const auto h=memory.load(GuestAddress{p+offset},bits);if(h){const auto f=gui_.global_memory.find(h);if(f==gui_.global_memory.end() || f->second.locks || f->second.clipboard_owned)return handle_error;}}
        const auto mode=global(printer_devmode(dialog.wide,dialog.settings)),names=global(devnames(dialog.wide));
        if(!mode || !names){discard();return 0x8007000eU;}
        if(dialog.flags&0x300U){dc=create_printer_dc(memory,gui_.global_memory.at(mode).address,dialog.wide,(dialog.flags&0x100U)==0);if(!dc){const auto error=hresult(last_error_);discard();return error;}}
        // Vista 起 RETURNDC 的 DC 保存真正份數；沒有 USEDEVMODECOPIES 時，
        // 對外 DEVMODE 仍是單份，應用程式可由 nCopies 取得所選份數。
        if(!(dialog.flags&0x40000U)){auto exported=dialog.settings;exported.copies=1;exported.collate=false;memory.write(GuestAddress{gui_.global_memory.at(mode).address},printer_devmode(dialog.wide,exported));}
        // 全部輸出先驗證，成功後才取代呼叫者的 HGLOBAL 與設定。
        memory.check(GuestAddress{p},layout.size,Access::Write);
        const auto range_address=memory.load(GuestAddress{p+layout.ranges},bits);if(!dialog.ranges.empty())memory.check(GuestAddress{range_address},dialog.ranges.size()*8,Access::Write);
        for(const auto offset:{layout.mode,layout.names}){const auto h=memory.load(GuestAddress{p+offset},bits);if(h){const auto at=gui_.global_memory.at(h).address;gui_.global_memory.erase(h);if(at)(void)free_heap(memory,at,state);}}
        if(dialog.applied){const auto old=memory.load(GuestAddress{p+layout.dc},bits);if(is_printer_dc(old)){delete_printer_dc(old);gui_.contexts.erase(old);}}
        memory.store(GuestAddress{p+layout.mode},bits,mode);memory.store(GuestAddress{p+layout.names},bits,names);memory.store(GuestAddress{p+layout.dc},bits,dc);
        const auto flags=dialog.flags&0x40000U?dialog.flags&~0x10U:dialog.flags;
        memory.store(GuestAddress{p+layout.flags},32,flags);memory.store(GuestAddress{p+layout.copies},32,dialog.flags&0x40000U?1:dialog.settings.copies);
        memory.store(GuestAddress{p+layout.count},32,dialog.ranges.size());
        for(std::size_t i=0;i<dialog.ranges.size();++i){memory.store(GuestAddress{range_address+i*8},32,dialog.ranges[i].first);memory.store(GuestAddress{range_address+i*8+4},32,dialog.ranges[i].second);}
        dialog.applied=true;return 0;
    }catch(...){discard();throw;}
}
void WindowsServices::refresh_print_settings(std::uint64_t root){
    auto& data=gui_.print_dialogs.at(root);const auto child=[&](unsigned id)->GuiWindow&{return gui_.windows.at(gui_child(data.general,id));};
    if(const auto copies=integer(child(0x482).title);copies && *copies>=1 && *copies<=99)data.settings.copies=*copies;
    data.settings.collate=child(0x411).check!=0;data.flags=(data.flags&~0x30U)|(data.settings.collate?0x10U:0U)|(child(0x410).check?0x20U:0U);
    data.settings.width=child(0x471).selection==1?794:816;data.settings.height=child(0x471).selection==1?1123:1056;
    if(child(0x472).selection==1)std::swap(data.settings.width,data.settings.height);data.settings.monochrome=child(0x412).check!=0;
}
void WindowsServices::select_print_page(std::uint64_t root,std::uint32_t selected){
    auto& data=gui_.print_dialogs.at(root);data.selected=selected;
    const auto show=[&](std::uint64_t h,bool visible){if(h && gui_.windows.contains(h)){gui_.windows.at(h).visible=visible;present_gui(h);if(native_gui_)show_native_window(h,visible);}};
    show(data.general,selected==0);for(std::size_t i=0;i<data.pages.size();++i)show(data.pages[i].window,selected==i+1);
    for(std::size_t i=0;i<=data.pages.size();++i)if(const auto h=gui_child(root,static_cast<std::uint32_t>(0x5000+i));h){gui_.windows.at(h).check=selected==i;present_gui(h);}
}
std::optional<std::uint32_t> WindowsServices::invoke_print_dialog(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    if(service.family=="internal-print-services"){
        const auto f=gui_.print_services.find(arg(0));if(f==gui_.print_services.end())return finish(handle_error);auto& object=f->second;
        if(service.symbol=="Release")return finish(release_print_services(state,memory,arg(0)));
        if(service.symbol=="AddRef"){if(object.references==UINT32_MAX)return finish(UINT32_MAX);return finish(++object.references);}
        if(service.symbol=="QueryInterface"){
            if(!arg(2))return finish(pointer_error);memory.store(GuestAddress{arg(2)},bits,0);if(!arg(1))return finish(pointer_error);std::array<std::uint8_t,16> iid;memory.read(GuestAddress{arg(1)},iid);
            if(iid!=unknown_iid && iid!=services_iid)return finish(0x80004002U);if(object.references==UINT32_MAX)return finish(0x8007000eU);++object.references;memory.store(GuestAddress{arg(2)},bits,arg(0));return finish(0);
        }
        if(!arg(2))return finish(pointer_error);const auto capacity=memory.load(GuestAddress{arg(2)},32);
        const auto d=gui_.print_dialogs.find(object.window);if(d==gui_.print_dialogs.end() || d->second.closing){memory.store(GuestAddress{arg(2)},32,0);return finish(0);}
        refresh_print_settings(object.window);const auto& dialog=d->second;
        const auto bytes=service.symbol=="GetCurrentDevMode"?printer_devmode(object.wide,dialog.settings):utf16_bytes(service.symbol=="GetCurrentPrinterName"?u"Win64 PDF":u"FILE:");
        const auto needed=bytes.size()/(service.symbol=="GetCurrentDevMode"?1:2);memory.store(GuestAddress{arg(2)},32,needed);
        if(!capacity)return finish(0);if(capacity<needed)return finish(hresult(122));if(!arg(1))return finish(pointer_error);memory.write(GuestAddress{arg(1)},bytes);return finish(0);
    }
    if(service.family=="internal-print-dialog"){
        const auto window=gui_.windows.find(arg(0));if(window==gui_.windows.end())return finish(0);
        const bool general=service.symbol=="GeneralProc";const auto root=general?window->second.parent:arg(0);const auto found=gui_.print_dialogs.find(root);if(found==gui_.print_dialogs.end())return finish(0);
        auto& data=found->second;const auto msg=static_cast<std::uint32_t>(arg(1)),id=static_cast<unsigned>(arg(2)&65535U);
        if(msg==0x81)return finish(1);
        if(msg==0x82 && !general){
            for(const auto& page:data.pages)if(const auto f=gui_.property_pages.find(page.handle);f!=gui_.property_pages.end())f->second.used=false;
            gui_.print_services.at(data.services).window=0;(void)release_print_services(state,memory,data.services);release_gui_scratch(memory,state,data.scratch);gui_.print_dialogs.erase(found);return finish(0);
        }
        if(msg==2 && !general){data.closing=true;data.phase=40;data.index=0;return resume_print_dialog(state,memory,root,0);}
        if(msg==0x476)return finish(data.selected?data.pages[data.selected-1].window:data.general);
        if(data.phase)return finish(0);
        if(general && data.callback && msg!=2 && msg!=0x82){data.pending={arg(0),arg(2),arg(3),msg};data.phase=20;memory.store(GuestAddress{data.result},bits,0);start_callbacks(state,memory,{method(memory,x86_,data.callback,5,{arg(0),msg,arg(2),arg(3),data.result})},Continuation::PrintDialogOperation,root);return {};}
        if(general && msg==0x111){
            if(id>=0x420 && id<=0x423){const std::uint32_t choice=id==0x421?1U:id==0x422?2U:id==0x423?0x400000U:0U;const auto child=gui_child(data.general,id);if(child && gui_.windows.at(child).enabled){data.flags=(data.flags&~0x400003U)|choice;for(unsigned radio=0x420;radio<=0x423;++radio){const auto h=gui_child(data.general,radio);gui_.windows.at(h).check=radio==id;present_gui(h);}}}
            refresh_print_settings(root);if(data.callback){data.phase=21;start_callbacks(state,memory,{method(memory,x86_,data.callback,4)},Continuation::PrintDialogOperation,root);return {};}
        }
        if(msg==0x465 || (msg==0x111 && id>=0x5000 && id<=0x5000+data.pages.size())){
            data.target=msg==0x465?static_cast<std::uint32_t>(arg(2)):id-0x5000;if(data.target>data.pages.size())return finish(0);data.action=0;if(data.selected){data.phase=8;data.index=data.selected-1;return resume_print_dialog(state,memory,root,UINT64_MAX);}data.phase=9;return resume_print_dialog(state,memory,root,0);
        }
        if(msg==0x10 || (msg==0x111 && (id==1 || id==2 || id==3 || id==0x3021))){
            data.action=msg==0x10 || id==2 || id==3?2:id==0x3021?3:1;data.phase=data.action==2?30:25;data.index=0;
            return resume_print_dialog(state,memory,root,UINT64_MAX);
        }
        return finish(0);
    }
    const Layout layout(x86_);const auto p=arg(0);if(!p)return finish(pointer_error);
    try {
        if(memory.load(GuestAddress{p},32)!=layout.size)return finish(invalid);memory.check(GuestAddress{p},layout.size,Access::Write);
        const auto get=[&](unsigned at,bool pointer=false){return memory.load(GuestAddress{p+at},pointer?bits:32);};
        GuiPrintDialog data;data.request=p;data.wide=service.symbol.ends_with('W');data.flags=static_cast<std::uint32_t>(get(layout.flags));data.minimum=static_cast<std::uint32_t>(get(layout.minimum));data.maximum=static_cast<std::uint32_t>(get(layout.maximum));data.range_capacity=static_cast<std::uint32_t>(get(layout.capacity));
        const auto owner=get(layout.owner,true);if(!owner || !gui_.windows.contains(owner))return finish(handle_error);data.owner_enabled=gui_.windows.at(owner).enabled;
        if(get(layout.flags2) || ((data.flags&0x1000000U) && (get(layout.exclude)&~0x8100U)) || (data.flags&~0x11dd47bfU))return finish(invalid);
        if(data.flags&0x14000U)return finish(hresult(50)); // 自訂列印模板仍須提供相同的客體控制項契約。
        const auto choices=data.flags&0x400003U;if(choices && (choices&(choices-1)))return finish(invalid);
        if((choices==1 && (data.flags&4U)) || (choices==2 && (data.flags&8U)) || (choices==0x400000U && (data.flags&0x800000U)))return finish(invalid);
        if(data.flags&8U){data.range_capacity=0;data.minimum=data.maximum=0;}
        const auto count=data.flags&8U?0:get(layout.count),pages=get(layout.pages),start=get(layout.start);if(count>data.range_capacity || data.range_capacity>4096 || pages>128 || (start!=UINT32_MAX && start>=pages))return finish(invalid);
        if(!(data.flags&8U) && (!data.range_capacity || !get(layout.ranges,true) || data.minimum>data.maximum))return finish(invalid);
        if(data.range_capacity){if(!get(layout.ranges,true))return finish(pointer_error);memory.check(GuestAddress{get(layout.ranges,true)},data.range_capacity*8,Access::Write);}
        for(std::uint64_t i=0;i<count;++i){const auto at=get(layout.ranges,true)+i*8;const auto first=memory.load(GuestAddress{at},32),last=memory.load(GuestAddress{at+4},32);if(first>last || first<data.minimum || last>data.maximum)return finish(invalid);data.ranges.emplace_back(static_cast<std::uint32_t>(first),static_cast<std::uint32_t>(last));}
        if(choices==2 && !count)return finish(invalid);
        const auto mode=get(layout.mode,true),names=get(layout.names,true);if((data.flags&0x400U) && (mode || names))return finish(invalid);
        for(const auto h:{mode,names})if(h){const auto f=gui_.global_memory.find(h);if(f==gui_.global_memory.end() || !(f->second.flags&2U) || !f->second.address || f->second.locks || f->second.clipboard_owned)return finish(handle_error);}
        if(mode){const auto& block=gui_.global_memory.at(mode);if(block.size<(data.wide?104U:72U))return finish(invalid);const auto size=memory.load(GuestAddress{block.address+(data.wide?68:36)},16);if(size>block.size)return finish(invalid);if(const auto error=read_printer_settings(memory,block.address,data.wide,data.settings))return finish(hresult(error));}
        if(names){const auto& block=gui_.global_memory.at(names);if(block.size<8)return finish(invalid);const unsigned character=data.wide?2:1;const auto name=[&](unsigned field){const auto offset=memory.load(GuestAddress{block.address+field},16)*character;if(offset<8 || offset>=block.size)throw std::invalid_argument("DEVNAMES 偏移不合法");return data.wide?read_utf16(memory,GuestAddress{block.address+offset},(block.size-offset)/2-1):utf8_to_utf16_lossy(read_ansi(memory,block.address+offset,block.size-offset-1));};if(name(2)!=u"Win64 PDF" || name(4)!=u"FILE:")return finish(hresult(1801));(void)name(0);}
        if(!mode){data.settings.copies=static_cast<unsigned>(get(layout.copies));data.settings.collate=(data.flags&0x10U)!=0;}if(!data.settings.copies || data.settings.copies>99)return finish(invalid);
        if(!native_gui_ || !runtime_.network.external_io)return finish(hresult(50));
        if(pages){if(!get(layout.handles,true))return finish(pointer_error);memory.check(GuestAddress{get(layout.handles,true)},pages*unit,Access::Read);std::set<std::uint64_t> unique;
            for(std::uint64_t i=0;i<pages;++i){const auto h=memory.load(GuestAddress{get(layout.handles,true)+i*unit},bits);const auto f=gui_.property_pages.find(h);if(f==gui_.property_pages.end() || f->second.used || f->second.phase || !unique.insert(h).second)return finish(handle_error);data.pages.push_back(f->second);}}
        if(data.flags&0x400U){const auto error=commit_print_dialog(state,memory,data);return finish(error);}
        if(gui_.print_dialogs.size()>=64)return finish(0x8007000eU);
        data.target=start==UINT32_MAX?0:static_cast<std::uint32_t>(start+1);data.callback_unknown=get(layout.callback,true);if(data.callback_unknown)(void)method(memory,x86_,data.callback_unknown,0);
        data.result=allocate_gui_scratch(memory,data.scratch,{},64);data.notification=allocate_gui_scratch(memory,data.scratch,{},32);
        memory.write(GuestAddress{data.result+16},callback_iid);memory.write(GuestAddress{data.result+32},site_iid);
        if(!gui_.print_services_vtable){const auto table=runtime_allocate(memory,{},6*unit);unsigned i=0;for(const auto& d:print_dialog_service_descriptors())if(d.family=="internal-print-services")memory.store(GuestAddress{table+unit*i++},bits,resolve(d.family,d.symbol).value);gui_.print_services_vtable=table;}
        data.services=allocate_heap(memory,unit);if(!data.services){release_gui_scratch(memory,state,data.scratch);return finish(0x8007000eU);}memory.store(GuestAddress{data.services},bits,gui_.print_services_vtable);
        const auto root=allocate_gui_window();GuiWindow dialog;dialog.dpi_context=effective_dpi_context();dialog.thread=threads_.current;dialog.class_name=u"#32770";dialog.title=u"列印";dialog.owner=owner;dialog.dialog=dialog.modal=dialog.builtin=true;dialog.width=680;dialog.height=460;dialog.style=0x80c80000U;dialog.procedure=resolve("internal-print-dialog","RootProc").value;dialog.default_id=1;dialog.extra.resize(40);
        for(const auto& page:data.pages){dialog.width=std::max(dialog.width,page.width+32);dialog.height=std::max(dialog.height,page.height+140);gui_.property_pages.at(page.handle).used=true;}
        gui_.windows.emplace(root,dialog);native_gui_->create(root,dialog);gui_.print_services.emplace(data.services,GuiPrintServices{1,root,data.wide});
        const auto add=[&](std::uint64_t parent,unsigned id,std::u16string cls,std::u16string title,int x,int y,unsigned width,unsigned height,unsigned style=0){GuiWindow w;w.dpi_context=effective_dpi_context();w.thread=threads_.current;w.parent=parent;w.id=id;w.class_name=w.control_class=std::move(cls);w.title=std::move(title);w.x=x;w.y=y;w.width=width;w.height=height;w.builtin=w.visible=true;w.style=0x50010000U|style;const auto h=allocate_gui_window();gui_.windows.emplace(h,w);native_gui_->create(h,w);return h;};
        const auto page_y=44+static_cast<unsigned>(data.pages.empty()?0:(data.pages.size()-1)/5)*28;gui_.windows.at(root).height+=page_y-44;dialog.height=gui_.windows.at(root).height;present_gui(root);
        data.general=add(root,0,u"#32770",u"",8,static_cast<int>(page_y),dialog.width-16,dialog.height-page_y-56);gui_.windows.at(data.general).dialog=true;gui_.windows.at(data.general).extra.resize(40);gui_.windows.at(data.general).procedure=resolve("internal-print-dialog","GeneralProc").value;
        const auto g=data.general;add(root,0x5000,u"button",u"一般",12,8,90,26,3);
        for(std::size_t i=0;i<data.pages.size();++i)add(root,static_cast<unsigned>(0x5001+i),u"button",data.pages[i].title,110+static_cast<int>(i%5)*100,8+static_cast<int>(i/5)*28,96,26,3);
        add(g,0,u"static",u"印表機：Win64 PDF（CoreGraphics）",12,8,580,26);
        const std::array<std::u16string,4> labels{u"全部頁面",u"選取範圍",u"指定頁碼",u"目前頁面"};
        for(unsigned i=0;i<4;++i){const auto h=add(g,0x420+i,u"button",labels[i],12,48+static_cast<int>(i)*30,160,26,9);auto& w=gui_.windows.at(h);gui_set_window_enabled(w,i==0 || (i==1?!(data.flags&4U):i==2?!(data.flags&8U):!(data.flags&0x800000U)));w.check=choices==(i==0?0:i==1?1:i==2?2:0x400000U);present_gui(h);}
        std::u16string ranges;for(const auto& r:data.ranges){if(!ranges.empty())ranges+=u", ";ranges+=number(r.first);if(r.second!=r.first)ranges+=u"-"+number(r.second);}if(ranges.empty())ranges=number(data.minimum);
        const auto ranges_edit=add(g,0x480,u"edit",ranges,180,108,280,26);gui_set_window_enabled(gui_.windows.at(ranges_edit),(data.flags&8U)==0);present_gui(ranges_edit);add(g,0,u"static",u"頁碼格式：1-3, 5, 8-10",180,138,350,26);
        add(g,0,u"static",u"份數",12,194,80,26);add(g,0x482,u"edit",number(data.settings.copies),100,194,80,26);
        const auto collate=add(g,0x411,u"button",u"逐份列印",200,194,120,26,3);gui_.windows.at(collate).check=data.settings.collate;present_gui(collate);
        const auto file=add(g,0x410,u"button",u"列印至檔案（PDF）",12,232,230,26,3);gui_.windows.at(file).check=(data.flags&0x20U)!=0;gui_set_window_enabled(gui_.windows.at(file),(data.flags&0x80000U)==0);gui_.windows.at(file).visible=(data.flags&0x100000U)==0;present_gui(file);
        const auto mono=add(g,0x412,u"button",u"灰階",260,232,100,26,3);gui_.windows.at(mono).check=data.settings.monochrome;present_gui(mono);
        const auto paper=add(g,0x471,u"combobox",u"",12,270,160,26,3),orientation=add(g,0x472,u"combobox",u"",200,270,160,26,3);
        gui_.windows.at(paper).items={{gui_.next_item++,0,0,u"Letter",0},{gui_.next_item++,0,0,u"A4",0}};gui_.windows.at(paper).selection=data.settings.width==794 || data.settings.height==794?1:0;
        gui_.windows.at(orientation).items={{gui_.next_item++,0,0,u"直向",0},{gui_.next_item++,0,0,u"橫向",0}};gui_.windows.at(orientation).selection=data.settings.width>data.settings.height?1:0;present_gui(paper);present_gui(orientation);
        add(root,499,u"static",u"",12,static_cast<int>(dialog.height)-90,dialog.width-24,26);
        add(root,1,u"button",u"列印",static_cast<int>(dialog.width)-286,static_cast<int>(dialog.height)-48,80,28,1);add(root,2,u"button",u"取消",static_cast<int>(dialog.width)-196,static_cast<int>(dialog.height)-48,80,28);add(root,0x3021,u"button",u"套用",static_cast<int>(dialog.width)-106,static_cast<int>(dialog.height)-48,80,28);
        memory.store(GuestAddress{p+layout.action},32,0);gui_.print_dialogs.emplace(root,std::move(data));auto& stored=gui_.print_dialogs.at(root);stored.phase=1;
        if(stored.callback_unknown)start_callbacks(state,memory,{method(memory,x86_,stored.callback_unknown,0,{stored.result+16,stored.result})},Continuation::PrintDialogOperation,root);else return resume_print_dialog(state,memory,root,0x80004002U);
        return {};
    }catch(const std::bad_alloc&){return finish(0x8007000eU);}catch(const HostCallFault&){return finish(pointer_error);}catch(const MemoryFault&){return finish(pointer_error);}catch(const std::invalid_argument&){return finish(invalid);}
}
std::optional<std::uint32_t> WindowsServices::resume_print_dialog(GuestState& state,GuestMemory& memory,std::uint64_t root,std::uint64_t value){
    const auto found=gui_.print_dialogs.find(root);if(found==gui_.print_dialogs.end()){complete_call(state,memory,0);return {};}
    auto& data=found->second;const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);const Layout layout(x86_);
    const auto done=[&](std::uint64_t result=0){data.phase=0;complete_call(state,memory,result);};
    const auto call=[&](std::vector<GuestCallback> callbacks,unsigned phase){data.phase=phase;start_callbacks(state,memory,std::move(callbacks),Continuation::PrintDialogOperation,root);};
    const auto notify=[&](std::size_t index,int code,unsigned phase){const auto h=data.pages[index].window;memory.write(GuestAddress{data.notification},std::array<std::uint8_t,32>{});memory.store(GuestAddress{data.notification},bits,root);memory.store(GuestAddress{data.notification+unit*2},32,static_cast<std::uint32_t>(code));const auto parameter=x86_?12U:24U;memory.store(GuestAddress{data.notification+parameter},bits,code==-202 && data.action==1?1:0);call({{gui_.windows.at(h).procedure,{h,0x4e,0,data.notification}}},phase);};
    const auto modal=[&]{data.phase=0;select_print_page(root,data.selected);start_callbacks(state,memory,{},Continuation::GuiModalInit,root);};
    const auto activate=[&](bool initial){
        if(!data.target){select_print_page(root,0);if(initial)modal();else done(1);return;}
        auto& page=data.pages[data.target-1];data.index=data.target-1;
        if(!page.window){if(page.callback)call({{page.callback,{0,2,page.descriptor}}},initial?10:11);else {data.phase=initial?10:11;resume_print_dialog(state,memory,root,1);}}
        else notify(data.index,-200,initial?12:7);
    };
    if(data.phase==1){
        if(!(static_cast<std::uint32_t>(value)&0x80000000U))data.callback=memory.load(GuestAddress{data.result},bits);
        if(data.callback_unknown){memory.store(GuestAddress{data.result+unit},bits,0);call({method(memory,x86_,data.callback_unknown,0,{data.result+32,data.result+unit})},2);return {};}
        data.phase=2;value=0x80004002U;
    }
    if(data.phase==2){
        if(!(static_cast<std::uint32_t>(value)&0x80000000U))data.site=memory.load(GuestAddress{data.result+unit},bits);
        if(data.site){call({method(memory,x86_,data.site,3,{data.services})},3);return {};}data.phase=3;
    }
    if(data.phase==3){if(data.callback){memory.store(GuestAddress{data.result},bits,0);call({method(memory,x86_,data.callback,5,{data.general,0x110,0,data.request,data.result})},4);return {};}data.phase=5;}
    if(data.phase==4){call({method(memory,x86_,data.callback,3)},5);return {};}
    if(data.phase==5){activate(true);return {};}
    if(data.phase==10 || data.phase==11){
        const bool initial=data.phase==10;auto& page=data.pages[data.index];if(!value){if(initial){data.target=0;modal();}else done(0);return {};}
        const auto name=page.wide?(page.flags&1U?"CreateDialogIndirectParamW":"CreateDialogParamW"):(page.flags&1U?"CreateDialogIndirectParamA":"CreateDialogParamA");
        call({{resolve("user32.dll",name).value,{page.instance,page.resource,root,page.procedure,page.descriptor}}},initial?13:6);return {};
    }
    if(data.phase==6 || data.phase==13){
        const bool initial=data.phase==13;auto& page=data.pages[data.index];if(!value || !gui_.windows.contains(value)){if(initial){data.target=0;modal();}else done(0);return {};}
        page.window=value;auto& w=gui_.windows.at(value);w.parent=root;w.owner=0;w.x=8;w.y=gui_.windows.at(data.general).y;w.visible=false;w.style&=~0x08c00000U;w.frame_insets.reset();gui_refresh_frame(w);present_gui(value);notify(data.index,-200,initial?12:7);return {};
    }
    if(data.phase==7 || data.phase==12){const bool initial=data.phase==12;if(!value)select_print_page(root,data.target);if(initial)modal();else done(value?0:1);return {};}
    if(data.phase==9){activate(false);return {};}
    if(data.phase==8){if(value==UINT64_MAX){notify(data.index,-201,8);return {};}if(value){done();return {};}activate(false);return {};}
    if(data.phase==20){
        const auto msg=data.pending;data.phase=0;if(static_cast<std::uint32_t>(value)==0){done(memory.load(GuestAddress{data.result},bits));return {};}
        // 重新進入標準處理，但不重複呼叫攔截器。
        const auto callback=data.callback;data.callback=0;
        if(msg.message==0x111 && (msg.wparam&65535U)>=0x420 && (msg.wparam&65535U)<=0x423){const auto id=static_cast<unsigned>(msg.wparam&65535U);const auto h=gui_child(data.general,id);if(gui_.windows.at(h).enabled){data.flags=(data.flags&~0x400003U)|(id==0x421?1U:id==0x422?2U:id==0x423?0x400000U:0U);for(unsigned n=0x420;n<=0x423;++n){const auto child=gui_child(data.general,n);gui_.windows.at(child).check=n==id;present_gui(child);}}}
        refresh_print_settings(root);data.callback=callback;
        if(msg.message==0x10 || (msg.message==0x111 && (msg.wparam&65535U)==3)){data.action=2;data.phase=30;data.index=0;return resume_print_dialog(state,memory,root,UINT64_MAX);}
        if(msg.message==0x111){call({method(memory,x86_,callback,4)},21);return {};}done();return {};
    }
    if(data.phase==21){done();return {};}
    if(data.phase==25){
        if(value==UINT64_MAX){
            const auto copies=integer(gui_.windows.at(gui_child(data.general,0x482)).title);if(!copies || !*copies || *copies>99){gui_.windows.at(gui_child(root,499)).title=u"份數須介於 1 與 99。";present_gui(gui_child(root,499));done();return {};}
            refresh_print_settings(root);data.ranges.clear();
            if(data.flags&2U){const auto input=utf16_to_utf8(gui_.windows.at(gui_child(data.general,0x480)).title);std::size_t at=0;bool valid=true;
                const auto parse=[&]()->std::optional<unsigned>{while(at<input.size() && input[at]==' ')++at;unsigned n{};const auto r=std::from_chars(input.data()+at,input.data()+input.size(),n);if(r.ec!=std::errc{})return {};at=static_cast<std::size_t>(r.ptr-input.data());while(at<input.size() && input[at]==' ')++at;return n;};
                while(at<input.size()){const auto first=parse();if(!first){valid=false;break;}auto last=first;if(at<input.size() && input[at]=='-'){++at;last=parse();}if(!last || *first>*last || *first<data.minimum || *last>data.maximum || data.ranges.size()>=data.range_capacity){valid=false;break;}data.ranges.emplace_back(*first,*last);if(at==input.size())break;if(input[at++]!=',' || at==input.size()){valid=false;break;}}
                if(!valid || data.ranges.empty()){gui_.windows.at(gui_child(root,499)).title=u"頁碼範圍或範圍數量不合法。";present_gui(gui_child(root,499));done();return {};}
            }
            if(data.selected){data.index=data.selected-1;notify(data.index,-201,25);return {};}
        }else if(value){done();return {};}
        data.index=0;data.phase=26;value=0;
    }
    if(data.phase==26){if(value){if(value==1){data.target=static_cast<std::uint32_t>(data.index);activate(false);}else done();return {};}
        while(data.index<data.pages.size() && !data.pages[data.index].window)++data.index;
        if(data.index<data.pages.size()){const auto i=data.index++;notify(i,-202,26);return {};}
        const auto error=commit_print_dialog(state,memory,data);if(error){gui_.windows.at(gui_child(root,499)).title=u"無法建立列印設定（錯誤 "+number(error)+u"）。";present_gui(gui_child(root,499));done();return {};}
        memory.store(GuestAddress{data.request+layout.action},32,data.action==1?1:2);
        if(data.action==1){gui_.windows.at(root).ended=true;gui_.windows.at(root).dialog_result=0;}done();return {};
    }
    if(data.phase==30){if(value==UINT64_MAX && data.selected){notify(data.selected-1,-209,30);return {};}if(value!=UINT64_MAX && value){done();return {};}data.index=0;data.phase=31;}
    if(data.phase==31){while(data.index<data.pages.size() && !data.pages[data.index].window)++data.index;if(data.index<data.pages.size()){const auto i=data.index++;notify(i,-203,31);return {};}
        memory.store(GuestAddress{data.request+layout.action},32,data.applied?2:0);gui_.windows.at(root).ended=true;gui_.windows.at(root).dialog_result=0;done();return {};
    }
    if(data.phase==40){
        std::vector<GuestCallback> callbacks;for(auto& page:data.pages)if(page.window && gui_.windows.contains(page.window)){const auto proc=gui_.windows.at(page.window).procedure;callbacks.push_back({proc,{page.window,2,0,0}});callbacks.push_back({proc,{page.window,0x82,0,0}});}
        if(data.callback){memory.store(GuestAddress{data.result},bits,0);callbacks.push_back(method(memory,x86_,data.callback,5,{data.general,2,0,0,data.result}));callbacks.push_back(method(memory,x86_,data.callback,5,{data.general,0x82,0,0,data.result}));}
        call(std::move(callbacks),41);return {};
    }
    if(data.phase==41){std::vector<GuestCallback> callbacks;for(const auto& page:data.pages){gui_.property_pages.at(page.handle).used=false;callbacks.push_back({resolve("comctl32.dll","DestroyPropertySheetPage").value,{page.handle}});}
        if(data.site)callbacks.push_back(method(memory,x86_,data.site,3,{0}));call(std::move(callbacks),42);return {};
    }
    if(data.phase==42){std::vector<GuestCallback> callbacks;if(data.site)callbacks.push_back(method(memory,x86_,data.site,2));if(data.callback)callbacks.push_back(method(memory,x86_,data.callback,2));call(std::move(callbacks),43);return {};}
    if(data.phase==43){done();return {};}
    // 尚未建立的頁面之選取入口。
    activate(false);return {};
}
}
