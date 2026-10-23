#include "x64arm/windows_services.hpp"
#include <chrono>
#include <cstdlib>
#include "x64arm/windows_window_state.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <bit>

namespace x64arm {
namespace {
using Rect=std::array<std::int32_t,4>;
std::uint32_t hr(std::uint32_t error){return error?0x80070000U|(error&0xffffU):0;}
bool nonempty(const Rect& r){return r[0]<r[2] && r[1]<r[3];}
std::u16string folder(std::u16string path){const auto cut=path.find_last_of(u'\\');return path.substr(0,cut<3?3:cut);}
bool same_path(const std::u16string& a,const std::u16string& b){if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i)if(unicode_case_unit(a[i],false)!=unicode_case_unit(b[i],false))return false;return true;}
std::string file_uri(std::string value){
    if(!value.starts_with("file:///"))throw std::invalid_argument("file URI 必須位於客體磁碟");
    value.erase(0,8);if(value.find_first_of("?#")!=value.npos)throw std::invalid_argument("file URI 含有未支援的查詢／片段");std::string decoded;
    const auto hex=[](char c)->int{if(c>='0' && c<='9')return c-'0';if(c>='a' && c<='f')return c-'a'+10;if(c>='A' && c<='F')return c-'A'+10;return -1;};
    for(std::size_t i=0;i<value.size();++i){if(value[i]=='%'){if(i+2>=value.size() || hex(value[i+1])<0 || hex(value[i+2])<0)throw std::invalid_argument("URI 編碼錯誤");decoded+=static_cast<char>(hex(value[i+1])*16+hex(value[i+2]));i+=2;}else decoded+=value[i];}
    if(decoded.size()<3 || decoded[1]!=':' || decoded[2]!='/')throw std::invalid_argument("file URI 沒有客體磁碟");
    return decoded;
}
}
namespace {
std::chrono::milliseconds gui_area_cache_window(){
    static const auto window=[] {const auto* value=std::getenv("X64ARM_GUI_AREA_CACHE_MS");return std::chrono::milliseconds(value && *value?std::atoll(value):100LL);}();
    return window;
}
}
// 螢幕與工作區一起查、一起快取：GetMonitorInfo 每次都要兩者，Chromium 的 GPU 程序每個 Present 呼叫一次。
// 顯示器設定變更後最多沿用快取 100 ms；X64ARM_GUI_AREA_CACHE_MS=0 關閉。
void WindowsServices::refresh_gui_areas(){
    const auto now=std::chrono::steady_clock::now();
    if(gui_area_cached_!=std::chrono::steady_clock::time_point{} && gui_area_cache_window().count()>0 && now-gui_area_cached_<gui_area_cache_window())return;
    gui_screen_cache_=native_gui_?native_gui_->screen_area():Rect{0,0,1440,900};
    gui_work_cache_=native_gui_?native_gui_->work_area():Rect{0,0,1440,900};
    gui_area_cached_=now;
}
std::array<std::int32_t,4> WindowsServices::gui_screen_area(){refresh_gui_areas();return gui_screen_cache_;}
std::array<std::int32_t,4> WindowsServices::gui_work_area(std::uint64_t excluded){
    refresh_gui_areas();auto area=gui_work_cache_;
    area[2]+=area[0];area[3]+=area[1];
    for(const auto& [handle,bar]:gui_.appbars){
        if(handle==excluded || !bar.positioned || bar.autohide)continue;
        const auto& r=bar.rectangle;
        if(bar.edge==0)area[0]=std::clamp(r[2],area[0],area[2]);
        else if(bar.edge==1)area[1]=std::clamp(r[3],area[1],area[3]);
        else if(bar.edge==2)area[2]=std::clamp(r[0],area[0],area[2]);
        else area[3]=std::clamp(r[1],area[1],area[3]);
    }
    return area;
}
void WindowsServices::notify_appbars(std::uint32_t event,std::uint64_t parameter){
    for(const auto& [handle,bar]:gui_.appbars)if(gui_.windows.contains(handle) && bar.callback){
        if(std::none_of(gui_.queue.begin(),gui_.queue.end(),[&](const auto& msg){return msg.window==handle && msg.message==bar.callback && msg.wparam==event && msg.lparam==parameter;}))
            enqueue_gui_message({handle,event,parameter,bar.callback});
    }
}
void WindowsServices::poll_appbars(){
    if(gui_.appbars.empty())return;
    const auto area=native_gui_?native_gui_->work_area():Rect{0,0,1440,900};
    if(gui_.appbar_native_area && *gui_.appbar_native_area!=area)notify_appbars(1);
    gui_.appbar_native_area=area;
    const auto dock=native_gui_?native_gui_->shell_dock():std::nullopt;
    if(gui_.appbar_native_dock && dock && gui_.appbar_native_dock->state!=dock->state)notify_appbars(0);
    gui_.appbar_native_dock=dock;
    const auto screen=gui_screen_area();bool fullscreen=false;
    for(const auto& [handle,w]:gui_.windows)if(!gui_.appbars.contains(handle) && !w.parent && w.visible && w.show_command!=2 && !(w.style&0x00c00000U) && w.x<=screen[0] && w.y<=screen[1] && static_cast<std::int64_t>(w.x)+w.width>=screen[2] && static_cast<std::int64_t>(w.y)+w.height>=screen[3]){fullscreen=true;break;}
    if(fullscreen!=gui_.appbar_fullscreen){gui_.appbar_fullscreen=fullscreen;notify_appbars(2,fullscreen);}
}
bool WindowsServices::shell_open_with_message(const GuiMessage& message){
    const auto found=gui_.shell_open_with.find(message.window);if(found==gui_.shell_open_with.end())return false;
    auto& data=found->second;auto& dialog=gui_.windows.at(message.window);
    if(message.message==0x10 || (message.message==0x111 && (message.wparam&65535U)==2)){dialog.ended=true;dialog.dialog_result=hr(1223);return true;}
    if(message.message!=0x111)return false;
    if((message.wparam&65535U)!=1)return true;
    if(!data.execute){dialog.ended=true;dialog.dialog_result=0;return true;}
    const auto list=gui_child(message.window,412);const auto index=gui_.windows.at(list).selection;
    if(index<0 || static_cast<std::size_t>(index)>=data.applications.size())return true;
    const auto error=native_gui_->shell_open(data.file,data.uri,data.applications[static_cast<std::size_t>(index)]);
    if(!error){dialog.ended=true;dialog.dialog_result=0;}
    else {const auto status=gui_child(message.window,499);gui_.windows.at(status).title=u"無法開啟檔案，請選擇其他應用程式或取消。";present_gui(status);}
    return true;
}
std::optional<std::uint32_t> WindowsServices::invoke_shell_desktop(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8U);
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    try {
        if(service.symbol=="SHAppBarMessage"){
            const auto message=static_cast<std::uint32_t>(arg(0));const auto p=arg(1);const auto fail=[&](std::uint32_t e){last_error_=e;return finish(0);};
            const unsigned size=x86_?36:48,handle_at=x86_?4:8,callback_at=x86_?8:16,edge_at=x86_?12:20,rect_at=x86_?16:24,param_at=x86_?32:40;
            if(!p || memory.load(GuestAddress{p},32)!=size)return fail(87);
            memory.check(GuestAddress{p},size,Access::Read);
            const auto handle=memory.load(GuestAddress{p+handle_at},bits),parameter=memory.load(GuestAddress{p+param_at},bits);
            const auto edge=static_cast<std::uint32_t>(memory.load(GuestAddress{p+edge_at},32));
            const auto read_rect=[&]{Rect r;for(unsigned i=0;i<4;++i)r[i]=std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(memory.load(GuestAddress{p+rect_at+i*4},32)));return r;};
            const auto write_rect=[&](const Rect& r){memory.check(GuestAddress{p+rect_at},16,Access::Write);for(unsigned i=0;i<4;++i)memory.store(GuestAddress{p+rect_at+i*4},32,static_cast<std::uint32_t>(r[i]));};
            if(message==4){const auto dock=native_gui_?native_gui_->shell_dock():std::nullopt;return finish(dock?dock->state:0);}
            if(message==5){const auto dock=native_gui_?native_gui_->shell_dock():std::nullopt;if(!dock || !nonempty(dock->rectangle))return fail(50);memory.check(GuestAddress{p},size,Access::Write);write_rect(dock->rectangle);memory.store(GuestAddress{p+edge_at},32,dock->edge);return finish(1);}
            if(message==10)return fail(50); // 公開 AppKit API 不允許修改使用者的系統 Dock 狀態。
            if(message==7 || message==11){
                if(edge>3)return fail(87);if(message==11 && read_rect()!=gui_screen_area())return fail(1461);
                for(const auto& [h,bar]:gui_.appbars)if(bar.autohide && bar.edge==edge)return finish(h);
                return finish(0);
            }
            if(!gui_.windows.contains(handle) || gui_.windows.at(handle).parent)return fail(1400);
            if(message==0){if(gui_.appbars.contains(handle))return finish(0);const auto callback=memory.load(GuestAddress{p+callback_at},32);if(callback<0x400 || callback>65535 || gui_.appbars.size()>=256)return fail(87);GuiAppBar bar;bar.callback=static_cast<std::uint32_t>(callback);gui_.appbars.emplace(handle,bar);return finish(1);}
            const auto found=gui_.appbars.find(handle);if(found==gui_.appbars.end())return fail(87);auto& bar=found->second;
            if(message==1){gui_.appbars.erase(found);notify_appbars(1);return finish(1);}
            if(message==6){bar.active=parameter!=0;return finish(1);}
            if(message==9){notify_appbars(1);return finish(1);}
            if(message==8 || message==12){
                if(edge>3)return fail(87);if(message==12 && read_rect()!=gui_screen_area())return fail(1461);
                if(parameter){for(const auto& [h,other]:gui_.appbars)if(h!=handle && other.autohide && other.edge==edge)return finish(0);const auto dock=native_gui_?native_gui_->shell_dock():std::nullopt;if(dock && (dock->state&1U) && dock->edge==edge)return finish(0);bar.edge=edge;bar.autohide=true;}
                else {if(!bar.autohide || bar.edge!=edge)return finish(0);bar.autohide=false;}
                notify_appbars(1);return finish(1);
            }
            if(message==2 || message==3){
                if(edge>3)return fail(87);auto proposed=read_rect();if(!nonempty(proposed))return fail(87);
                memory.check(GuestAddress{p+rect_at},16,Access::Write);
                const auto area=gui_work_area(handle);const auto extent=std::min<std::int64_t>(edge==0 || edge==2?static_cast<std::int64_t>(proposed[2])-proposed[0]:static_cast<std::int64_t>(proposed[3])-proposed[1],edge==0 || edge==2?area[2]-area[0]:area[3]-area[1]);
                proposed=area;if(edge==0)proposed[2]=area[0]+static_cast<std::int32_t>(extent);else if(edge==1)proposed[3]=area[1]+static_cast<std::int32_t>(extent);else if(edge==2)proposed[0]=area[2]-static_cast<std::int32_t>(extent);else proposed[1]=area[3]-static_cast<std::int32_t>(extent);
                write_rect(proposed);if(message==3){const bool changed=!bar.positioned || bar.rectangle!=proposed || bar.edge!=edge;bar.rectangle=proposed;bar.edge=edge;bar.positioned=true;if(changed)notify_appbars(1);}return finish(1);
            }
            return fail(87);
        }
        if(service.symbol=="SHOpenFolderAndSelectItems"){
            if(!gui_.com_initializations.contains(threads_.current))return finish(0x800401f0U);
            const auto count=arg(1),flags=arg(3);if(count>4096 || (flags&~3ULL) || (count && !arg(2)))return finish(0x80070057U);
            const auto pidl_path=[&](std::uint64_t at){const auto found=gui_.shell_pidls.find(at);if(found==gui_.shell_pidls.end() || !allocations_.contains(at))throw std::invalid_argument("PIDL 不屬於此 Shell");return found->second;};
            const auto parent=pidl_path(arg(0));std::vector<std::shared_ptr<HostFile>> files;files.reserve(count?count:1);
            if(count){const auto info=filesystem_.target_information(utf16_to_utf8(parent));if(info.error)return finish(hr(info.error));if(!(info.metadata.attributes&16U))return finish(hr(267));memory.check(GuestAddress{arg(2)},count*unit,Access::Read);}
            for(std::uint64_t i=0;i<(count?count:1);++i){const auto path=count?pidl_path(memory.load(GuestAddress{arg(2)+i*unit},bits)):parent;if(count && !same_path(folder(path),parent))return finish(0x80070057U);const auto file=filesystem_.pin(utf16_to_utf8(path));if(file.error)return finish(hr(file.error));files.push_back(file.file);}
            if(!native_gui_)return finish(hr(50));if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許啟動原生 Finder");
            return finish(hr(native_gui_->shell_reveal(files,static_cast<std::uint32_t>(flags))));
        }
        if(service.symbol!="SHOpenWithDialog")throw HostCallFault("未知的 Shell desktop API");
        const auto owner=arg(0),p=arg(1);if(!p)return finish(0x80004003U);if(owner && !gui_.windows.contains(owner))return finish(hr(1400));
        memory.check(GuestAddress{p},unit*2+4,Access::Read);const auto file_at=memory.load(GuestAddress{p},bits),class_at=memory.load(GuestAddress{p+unit},bits),flags=memory.load(GuestAddress{p+unit*2},32);
        if((flags&~0xefULL) || !file_at)return finish(0x80070057U);
        auto name=read_utf16(memory,GuestAddress{file_at});const auto description=class_at && !(flags&64U)?read_utf16(memory,GuestAddress{class_at}):std::u16string{};
        if(name.empty())return finish(0x80070057U);GuiShellOpenWith data;data.execute=(flags&4U)!=0;data.owner_enabled=owner?gui_.windows.at(owner).enabled:false;
        if(data.execute){
            auto encoded=utf16_to_utf8(name);
            if(flags&192U){
                if((flags&64U) && encoded.find(':')==encoded.npos)encoded+=':';
                const auto colon=encoded.find(':');
                if(colon==encoded.npos || !colon || colon>63 || encoded.find_first_of("\r\n\0",0,3)!=encoded.npos)return finish(0x80070057U);
                for(std::size_t i=0;i<colon;++i){const auto c=encoded[i];if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (i && ((c>='0' && c<='9') || c=='+' || c=='-' || c=='.'))))return finish(0x80070057U);if(c>='A' && c<='Z')encoded[i]=static_cast<char>(c+32);}
                if(encoded.substr(0,colon)!="file")data.uri=std::move(encoded);
                else name=utf8_to_utf16(file_uri(std::move(encoded)));
            }
            if(data.uri.empty()){const auto pinned=filesystem_.pin_security(utf16_to_utf8(name));if(pinned.error)return finish(hr(pinned.error));data.file=pinned.file;}
        }
        if(!native_gui_)return finish(hr(50));if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許啟動原生應用程式");
        if(data.execute)data.applications=native_gui_->shell_applications(data.file,data.uri);
        if(data.applications.size()>256)data.applications.resize(256);
        GuiWindow dialog;dialog.dpi_context=effective_dpi_context();dialog.thread=threads_.current;dialog.class_name=u"#32770";dialog.dialog=true;dialog.modal=true;dialog.builtin=true;dialog.owner=owner;dialog.width=600;dialog.height=data.execute?350:200;dialog.title=u"選擇開啟方式";
        const auto handle=allocate_gui_window();gui_.windows.emplace(handle,dialog);
        try {
            native_gui_->create(handle,dialog);
            const auto add=[&](unsigned id,std::u16string cls,std::u16string text,int x,int y,unsigned width,unsigned height,std::uint32_t style=0){GuiWindow w;w.dpi_context=dialog.dpi_context;w.thread=threads_.current;w.parent=handle;w.id=id;w.class_name=std::move(cls);w.title=std::move(text);w.x=x;w.y=y;w.width=width;w.height=height;w.style=style;w.visible=true;w.builtin=true;const auto child=allocate_gui_window();gui_.windows.emplace(child,w);native_gui_->create(child,w);return child;};
            add(0,u"static",description.empty()?name:description,20,20,550,45);
            if(data.execute){const auto list=add(412,u"listbox",{},20,70,550,160,1);auto& w=gui_.windows.at(list);for(const auto& app:data.applications)w.items.push_back({gui_.next_item++,0,0,app.name,0});w.selection=w.items.empty()?-1:0;present_gui(list);}
            add(499,u"static",data.execute?(data.applications.empty()?u"沒有可開啟此檔案的 macOS 應用程式。":u""):u"請在 macOS 系統設定或 Finder 的「取得資訊」中變更預設應用程式。",20,data.execute?240:70,550,45);
            const auto ok=add(1,u"button",data.execute?u"開啟":u"確定",370,static_cast<int>(dialog.height)-45,90,28,1);add(2,u"button",u"取消",480,static_cast<int>(dialog.height)-45,90,28);
            gui_set_window_enabled(gui_.windows.at(ok),!data.execute || !data.applications.empty());present_gui(ok);gui_.shell_open_with.emplace(handle,std::move(data));
        }catch(...){erase_window(handle);throw;}
        start_callbacks(state,memory,{},Continuation::GuiModalInit,handle);return std::nullopt;
    }catch(const std::bad_alloc&){if(service.symbol=="SHAppBarMessage"){last_error_=8;return finish(0);}return finish(0x8007000eU);}catch(const std::invalid_argument&){if(service.symbol=="SHAppBarMessage"){last_error_=87;return finish(0);}return finish(0x80070057U);}
}
}
