#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "webbrowser_abi.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <map>
#include <memory>

namespace x64arm {
// 舊式 WebBrowser ActiveX 與 WebView2 共用 NativeGui 的 WKWebView；不載入 IE 二進位。
// https://learn.microsoft.com/windows/win32/api/exdisp/nn-exdisp-iwebbrowser2
struct WebBrowserState {
    struct Control {
        unsigned references{1};bool factory{},visible{true},native{},initialized{},ui_active{};
        std::uint64_t key{},window{},site{},inplace_site{},thread{};
        std::array<std::uint64_t,8> interfaces{};
        std::array<std::int32_t,4> bounds{0,0,300,150};
        std::u16string url{u"about:blank"},title,status,host_application,host_object;
        std::map<std::string,std::uint32_t> properties;
        std::map<std::uint32_t,std::uint64_t> sinks;
        std::uint32_t next_cookie{1};
    };
    struct Object {std::shared_ptr<Control> control;unsigned kind;};
    std::map<std::uint64_t,Object> objects;
    std::array<std::uint64_t,8> tables{};
    std::map<std::uint64_t,GuiMessage> deliveries;
    std::uint64_t next_delivery{1};
    unsigned server_locks{};
};
namespace {
constexpr unsigned browser=0,inplace=2,persist=3,connections=4,connection=5,factory=7;
constexpr std::uint32_t invalid=0x80070057U,pointer_error=0x80004003U,no_interface=0x80004002U,not_impl=0x80004001U,no_memory=0x8007000eU;
constexpr webbrowser::Guid unknown{0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr webbrowser::Guid dispatch{0,4,2,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
constexpr webbrowser::Guid in_place_site{0x19,1,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
std::optional<unsigned> interface_kind(const webbrowser::Guid& iid,bool is_factory){
    if(iid==unknown)return is_factory?factory:browser;
    if(is_factory)return iid==webbrowser::IClassFactory_iid?std::optional<unsigned>{factory}:std::nullopt;
    if(iid==dispatch || iid==webbrowser::Guid{0xc1,0x2a,0xb2,0xea,0xc1,0x30,0xcf,0x11,0xa7,0xeb,0,0,0xc0,0x5b,0xae,0x0b} || iid==webbrowser::Guid{0xc2,0x2a,0xb2,0xea,0xc1,0x30,0xcf,0x11,0xa7,0xeb,0,0,0xc0,0x5b,0xae,0x0b})return browser;
    if(iid==webbrowser::Guid{0x14,1,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46})return inplace;
    if(iid==webbrowser::Guid{0x0c,1,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46})return persist;
    for(unsigned i=0;i<factory;++i)if(iid==webbrowser::interfaces[i].iid)return i;
    return std::nullopt;
}
}
std::span<const WindowsServiceDescriptor> webbrowser_service_descriptors(){
    static const auto table=[]{
        static std::vector<std::string> names;std::vector<WindowsServiceDescriptor> out;
        for(const auto& iface:webbrowser::interfaces)for(const auto& method:iface.methods)names.push_back(std::string(iface.name)+"."+std::string(method.name));
        std::size_t i=0;for(const auto& iface:webbrowser::interfaces)for(const auto& method:iface.methods){
            WindowsServiceDescriptor d{"internal-webbrowser",names[i++],method.arguments};
            if(method.name=="PutProperty")d.aggregate_arguments=1U<<2;
            out.push_back(d);
        }
        out.push_back({"internal-webbrowser","Deliver",1});return out;
    }();return table;
}
bool WindowsServices::begin_webbrowser_com(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);if(!arg(0))return false;
    webbrowser::Guid clsid;memory.read(GuestAddress{arg(0)},clsid);if(clsid!=webbrowser::clsid)return false;
    (void)invoke_webbrowser(state,memory,service);return true;
}
void WindowsServices::queue_webbrowser_event(const GuiMessage& event){
    if(!webbrowser_)return;const auto found=webbrowser_->objects.find(event.window);if(found==webbrowser_->objects.end())return;
    auto& control=*found->second.control;if(control.factory)return;
    const auto id=webbrowser_->next_delivery++;webbrowser_->deliveries.emplace(id,event);
    GuiSentMessage request;request.receiver=control.thread;request.abandoned=true;
    request.rpc=GuestCallback{resolve("internal-webbrowser","Deliver").value,{id}};
    gui_.sent_messages.emplace(gui_.next_object++,std::move(request));
}
std::optional<std::uint32_t> WindowsServices::invoke_webbrowser(GuestState& entry,GuestMemory& mem,const WindowsServiceDescriptor& service){
    return service_callback::invoke<std::optional<std::uint32_t>>(this,threads_.current,entry,mem,"WebBrowser",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);},
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
    const WindowsCall arg(state,memory,service);const auto bits=static_cast<std::uint8_t>(x86_?32:64);const unsigned unit=bits/8U;
    const auto finish=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    if(!webbrowser_)webbrowser_=std::make_shared<WebBrowserState>();auto& bridge=*webbrowser_;
    const auto store=[&](std::uint64_t at,std::uint64_t value){memory.store(GuestAddress{at},bits,value);};
    const auto method=[&](std::uint64_t object,unsigned slot){return memory.load(GuestAddress{memory.load(GuestAddress{object},bits)+slot*unit},bits);};
    const auto text=[&](std::uint64_t at){return at?read_utf16(memory,GuestAddress{at},1U<<22):std::u16string{};};
    const auto rect=[&](std::uint64_t at){std::array<std::int32_t,4> value{};for(unsigned i=0;i<4;++i)value[i]=static_cast<std::int32_t>(memory.load(GuestAddress{at+i*4},32));return value;};
    const auto bstr=[&](std::u16string_view value){const auto bytes=utf16_bytes(std::u16string(value));return allocate_bstr(memory,bytes.size()-2,bytes);};
    const auto iface=[&](const std::shared_ptr<WebBrowserState::Control>& c,unsigned kind){
        auto& handle=c->interfaces[kind];if(handle)return handle;
        auto& table=bridge.tables[kind];const auto& abi=webbrowser::interfaces[kind];
        if(!table){table=runtime_allocate(memory,{},abi.methods.size()*unit);for(unsigned i=0;i<abi.methods.size();++i)store(table+i*unit,resolve("internal-webbrowser",std::string(abi.name)+"."+std::string(abi.methods[i].name)).value);}
        handle=allocate_heap(memory,unit);if(handle){store(handle,table);bridge.objects.emplace(handle,WebBrowserState::Object{c,kind});}return handle;
    };
    const auto create=[&](bool is_factory,unsigned kind){auto c=std::make_shared<WebBrowserState::Control>();c->factory=is_factory;c->thread=threads_.current;c->key=iface(c,is_factory?factory:browser);return c->key?iface(c,kind):0;};
    const auto symbol=service.symbol;
    if(symbol=="CoCreateInstance" || symbol=="CoGetClassObject"){
        if(!arg(4) || !arg(3))co_return finish(pointer_error);store(arg(4),0);
        if(symbol=="CoCreateInstance" && arg(1))co_return finish(0x80040110U);
        if(!(arg(symbol=="CoCreateInstance"?2:1)&5U))co_return finish(0x80040154U);
        webbrowser::Guid iid;memory.read(GuestAddress{arg(3)},iid);const auto kind=interface_kind(iid,symbol=="CoGetClassObject");
        if(!kind)co_return finish(no_interface);const auto object=create(symbol=="CoGetClassObject",*kind);store(arg(4),object);co_return finish(object?0:no_memory);
    }
    if(symbol=="Deliver"){
        const auto found=bridge.deliveries.find(arg(0));if(found==bridge.deliveries.end())co_return finish(0);
        auto event=std::move(found->second);bridge.deliveries.erase(found);
        const auto object=bridge.objects.find(event.window);if(object==bridge.objects.end())co_return finish(0);
        auto c=object->second.control;if(event.wparam==1)c->url=event.text;
        // 真正 WKWebView 導覽完成後才通知容器；兩個 VARIANT 依 COM 反向參數順序。
        if(event.wparam==1 && !c->sinks.empty()){
            const auto scratch=allocate_heap(memory,128);if(!scratch)co_return finish(no_memory);
            memory.fill(GuestAddress{scratch},0,128);const auto url=bstr(c->url);store(scratch+96,url);
            memory.store(GuestAddress{scratch+48},16,0x4008);store(scratch+56,scratch+96);
            const auto variant=x86_?16U:24U;memory.store(GuestAddress{scratch+48+variant},16,9);store(scratch+56+variant,iface(c,browser));
            store(scratch+16,scratch+48);store(scratch+16+unit,0);memory.store(GuestAddress{scratch+16+unit*2},32,2);
            const auto sinks=c->sinks;
            for(const auto& [cookie,sink]:sinks){if(!c->sinks.contains(cookie))continue;
                (void)co_await service_callback::call(method(sink,1),{sink});
                for(const auto event_id:{252U,259U})(void)co_await service_callback::call(method(sink,6),{sink,event_id,scratch,0,1,scratch+16,0,0,0});
                (void)co_await service_callback::call(method(sink,2),{sink});
            }
            if(url)free_bstr(state,memory,url);free_heap(memory,scratch,state);
        }co_return finish(0);
    }
    const auto object=bridge.objects.find(arg(0));if(object==bridge.objects.end())co_return finish(pointer_error);
    auto c=object->second.control;const auto kind=object->second.kind;const auto name=symbol.substr(symbol.find('.')+1);
    if(name=="QueryInterface"){
        if(!arg(1) || !arg(2))co_return finish(pointer_error);store(arg(2),0);webbrowser::Guid iid;memory.read(GuestAddress{arg(1)},iid);
        const auto wanted=interface_kind(iid,c->factory);if(!wanted)co_return finish(no_interface);
        const auto handle=iface(c,*wanted);if(!handle)co_return finish(no_memory);++c->references;store(arg(2),handle);co_return finish(0);
    }
    if(name=="AddRef")co_return finish(++c->references);
    if(name=="Release"){
        if(--c->references)co_return finish(c->references);
        if(c->native && native_gui_)native_gui_->webview_destroy(c->key);
        const auto site=std::exchange(c->site,0),in_site=std::exchange(c->inplace_site,0);const auto sinks=std::move(c->sinks);
        for(auto handle:c->interfaces)if(handle){bridge.objects.erase(handle);free_heap(memory,handle,state);}
        if(in_site)(void)co_await service_callback::call(method(in_site,2),{in_site});
        if(site)(void)co_await service_callback::call(method(site,2),{site});
        for(const auto& [cookie,sink]:sinks){(void)cookie;(void)co_await service_callback::call(method(sink,2),{sink});}
        co_return finish(0);
    }
    if(kind==factory){
        if(name=="LockServer"){if(arg(1))++bridge.server_locks;else if(bridge.server_locks)--bridge.server_locks;co_return finish(0);}
        if(name=="CreateInstance"){
            if(!arg(2) || !arg(3))co_return finish(pointer_error);store(arg(3),0);if(arg(1))co_return finish(0x80040110U);
            webbrowser::Guid iid;memory.read(GuestAddress{arg(2)},iid);const auto wanted=interface_kind(iid,false);if(!wanted)co_return finish(no_interface);
            const auto value=create(false,*wanted);store(arg(3),value);co_return finish(value?0:no_memory);
        }
    }
    if(name=="GetClassID"){if(!arg(1))co_return finish(pointer_error);memory.write(GuestAddress{arg(1)},webbrowser::clsid);co_return finish(0);}
    if(name=="InitNew"){if(c->initialized)co_return finish(0x8000ffffU);c->initialized=true;co_return finish(0);}
    if(name=="IsDirty")co_return finish(1);
    if(name=="SetClientSite"){
        const auto next=arg(1),old=c->site;if(next)(void)co_await service_callback::call(method(next,1),{next});c->site=next;
        if(old)(void)co_await service_callback::call(method(old,2),{old});co_return finish(0);
    }
    if(name=="GetClientSite"){
        if(!arg(1))co_return finish(pointer_error);store(arg(1),c->site);if(c->site)(void)co_await service_callback::call(method(c->site,1),{c->site});co_return finish(0);
    }
    if(name=="DoVerb"){
        const auto verb=static_cast<std::int32_t>(arg(1));if(verb==-3){c->visible=false;if(c->native)native_gui_->webview_visible(c->key,false);co_return finish(0);}
        if(verb!=0 && verb!=-1 && verb!=-4 && verb!=-5)co_return finish(0x80040181U);
        auto parent=arg(5);const auto site=arg(3)?arg(3):c->site;
        if(!parent || !gui_.windows.contains(parent) || !arg(6))co_return finish(invalid);
        auto bounds=rect(arg(6));if(bounds[2]<bounds[0] || bounds[3]<bounds[1])co_return finish(invalid);
        if(!native_gui_)co_return finish(not_impl);
        if(site && !c->inplace_site){
            const auto scratch=allocate_heap(memory,32);if(!scratch)co_return finish(no_memory);
            memory.write(GuestAddress{scratch},in_place_site);store(scratch+16,0);
            const auto hr=co_await service_callback::call(method(site,0),{site,scratch,scratch+16});
            const auto in_site=memory.load(GuestAddress{scratch+16},bits);free_heap(memory,scratch,state);
            if((hr&0x80000000U) || !in_site)co_return finish(hr&0x80000000U?hr:no_interface);
            const auto allowed=co_await service_callback::call(method(in_site,5),{in_site});
            if(allowed!=0){(void)co_await service_callback::call(method(in_site,2),{in_site});co_return finish(0x80004005U);}
            const auto activated=co_await service_callback::call(method(in_site,6),{in_site});
            if(activated&0x80000000U){(void)co_await service_callback::call(method(in_site,2),{in_site});co_return finish(activated);}
            c->inplace_site=in_site;
        }
        if(verb==-4 && c->inplace_site && !c->ui_active){
            const auto hr=co_await service_callback::call(method(c->inplace_site,7),{c->inplace_site});
            if(hr&0x80000000U)co_return finish(hr);c->ui_active=true;
        }
        const bool created=c->window!=parent || !c->native;
        if(created){if(!native_gui_->webview_create(c->key,parent))co_return finish(0x80004005U);c->native=true;c->window=parent;}
        c->bounds=bounds;c->visible=true;native_gui_->webview_bounds(c->key,bounds);native_gui_->webview_visible(c->key,true);
        if(site)(void)co_await service_callback::call(method(site,6),{site});
        if(created && c->url!=u"about:blank")native_gui_->webview_navigate(c->key,c->url);co_return finish(0);
    }
    if(name=="SetObjectRects"){
        if(!arg(1) || !arg(2))co_return finish(pointer_error);c->bounds=rect(arg(1));if(c->native)native_gui_->webview_bounds(c->key,c->bounds);co_return finish(0);
    }
    if(name=="GetWindow" || name=="get_HWND"){if(!arg(1))co_return finish(pointer_error);store(arg(1),c->window);co_return finish(c->window?0:0x80004005U);}
    if(name=="Close" || name=="Quit" || name=="InPlaceDeactivate" || name=="UIDeactivate"){
        const auto in_site=c->inplace_site;
        if(c->ui_active){c->ui_active=false;if(in_site)(void)co_await service_callback::call(method(in_site,9),{in_site,0});}
        if(name=="UIDeactivate")co_return finish(0);
        c->inplace_site=0;
        if(in_site){(void)co_await service_callback::call(method(in_site,10),{in_site});(void)co_await service_callback::call(method(in_site,2),{in_site});}
        if(c->native)native_gui_->webview_destroy(c->key);c->native=false;c->window=0;co_return finish(0);
    }
    if(name=="SetHostNames"){c->host_application=text(arg(1));c->host_object=text(arg(2));co_return finish(0);}
    if(name=="OnFrameWindowActivate" || name=="OnDocWindowActivate" || name=="EnableModeless"){c->properties[std::string(name)]=arg(1)!=0;co_return finish(0);}
    if(name=="TranslateAccelerator")co_return finish(1); // 未吃掉訊息，由容器繼續處理。
    if(name=="GetMiscStatus"){if(!arg(2))co_return finish(pointer_error);memory.store(GuestAddress{arg(2)},32,0x20191U);co_return finish(0);}
    if(name=="FindConnectionPoint"){
        if(!arg(1) || !arg(2))co_return finish(pointer_error);store(arg(2),0);webbrowser::Guid iid;memory.read(GuestAddress{arg(1)},iid);
        if(iid!=webbrowser::events_iid)co_return finish(0x80040200U);const auto value=iface(c,connection);if(!value)co_return finish(no_memory);++c->references;store(arg(2),value);co_return finish(0);
    }
    if(name=="GetConnectionInterface"){if(!arg(1))co_return finish(pointer_error);memory.write(GuestAddress{arg(1)},webbrowser::events_iid);co_return finish(0);}
    if(name=="GetConnectionPointContainer"){if(!arg(1))co_return finish(pointer_error);const auto value=iface(c,connections);if(!value)co_return finish(no_memory);++c->references;store(arg(1),value);co_return finish(0);}
    if(name=="Advise" && kind==connection){
        if(!arg(1) || !arg(2))co_return finish(pointer_error);memory.check(GuestAddress{arg(2)},4,Access::Write);
        memory.store(GuestAddress{arg(2)},32,0);const auto scratch=allocate_heap(memory,32);if(!scratch)co_return finish(no_memory);
        memory.write(GuestAddress{scratch},webbrowser::events_iid);store(scratch+16,0);
        const auto hr=co_await service_callback::call(method(arg(1),0),{arg(1),scratch,scratch+16});
        const auto sink=memory.load(GuestAddress{scratch+16},bits);free_heap(memory,scratch,state);
        if((hr&0x80000000U) || !sink)co_return finish(0x80040202U);
        const auto cookie=c->next_cookie++;c->sinks[cookie]=sink;memory.store(GuestAddress{arg(2)},32,cookie);co_return finish(0);
    }
    if(name=="Unadvise" && kind==connection){const auto found=c->sinks.find(static_cast<std::uint32_t>(arg(1)));if(found==c->sinks.end())co_return finish(0x80040200U);const auto sink=found->second;c->sinks.erase(found);(void)co_await service_callback::call(method(sink,2),{sink});co_return finish(0);}
    if(name=="Navigate" || name=="Navigate2"){
        auto source=arg(1);if(name=="Navigate2"){if(!source || memory.load(GuestAddress{source},16)!=8)co_return finish(invalid);source=memory.load(GuestAddress{source+8},bits);}
        if(!source)co_return finish(invalid);
        // 含 POST／自訂 frame 或標頭的導覽不能當成一般 GET 假裝成功。
        for(unsigned i=3;i<=5;++i)if(arg(i) && memory.load(GuestAddress{arg(i)},16)!=0 && memory.load(GuestAddress{arg(i)},16)!=10)co_return finish(not_impl);
        auto uri=text(source);const auto colon=uri.find(u':');if(colon==std::u16string::npos)co_return finish(invalid);
        auto scheme=uri.substr(0,colon);for(auto& letter:scheme)if(letter>=u'A' && letter<=u'Z')letter+=32;
        // 本機 Windows 檔案須經虛擬磁碟轉譯；尚未提供的 scheme 不直接交給宿主。
        if(scheme!=u"http" && scheme!=u"https" && scheme!=u"about" && scheme!=u"data")co_return finish(not_impl);
        c->url=std::move(uri);if(c->native)native_gui_->webview_navigate(c->key,c->url);co_return finish(0);
    }
    if(name=="GoBack" || name=="GoForward" || name=="Refresh" || name=="Refresh2" || name=="Stop"){
        if(!c->native)co_return finish(0x80004005U);native_gui_->webview_command(c->key,name=="GoBack"?2:name=="GoForward"?3:name=="Stop"?1:0);co_return finish(0);
    }
    if(name=="GetTypeInfoCount"){if(!arg(1))co_return finish(pointer_error);memory.store(GuestAddress{arg(1)},32,0);co_return finish(0);}
    if(name=="get_LocationURL" || name=="get_LocationName" || name=="get_StatusText" || name=="get_Type" || name=="get_Name"){
        if(!arg(1))co_return finish(pointer_error);const auto value=bstr(name=="get_LocationURL"?c->url:name=="get_LocationName"?c->title:name=="get_StatusText"?c->status:u"WebBrowser");store(arg(1),value);co_return finish(value?0:no_memory);
    }
    if(name=="put_StatusText"){c->status=text(arg(1));co_return finish(0);}
    if(name=="get_Application" || name=="get_Parent" || name=="get_Container"){if(!arg(1))co_return finish(pointer_error);store(arg(1),iface(c,browser));++c->references;co_return finish(0);}
    if(name=="get_Visible"){if(!arg(1))co_return finish(pointer_error);memory.store(GuestAddress{arg(1)},16,c->visible?65535:0);co_return finish(0);}
    if(name=="put_Visible"){c->visible=arg(1)!=0;if(c->native)native_gui_->webview_visible(c->key,c->visible);co_return finish(0);}
    if(name=="get_Busy" || name=="get_ReadyState"){
        if(!arg(1))co_return finish(pointer_error);const auto loading=c->native && native_gui_->webview_loading(c->key);
        memory.store(GuestAddress{arg(1)},name=="get_Busy"?16:32,name=="get_Busy"?(loading?65535:0):loading?1:4);co_return finish(0);
    }
    if(name.starts_with("get_") || name.starts_with("put_")){
        const auto property=std::string(name.substr(4));const bool get=name.starts_with("get_");
        if(property=="Left" || property=="Top" || property=="Width" || property=="Height"){
            const auto axis=(property=="Top" || property=="Height")?1U:0U;const bool size=property=="Width" || property=="Height";
            if(get){if(!arg(1))co_return finish(pointer_error);memory.store(GuestAddress{arg(1)},32,static_cast<std::uint32_t>(size?c->bounds[axis+2]-c->bounds[axis]:c->bounds[axis]));}
            else {const auto value=static_cast<std::int32_t>(arg(1));if(size && value<0)co_return finish(invalid);if(size)c->bounds[axis+2]=c->bounds[axis]+value;else {c->bounds[axis+2]+=value-c->bounds[axis];c->bounds[axis]=value;}if(c->native)native_gui_->webview_bounds(c->key,c->bounds);}co_return finish(0);
        }
        if(property=="Silent" || property=="RegisterAsBrowser" || property=="RegisterAsDropTarget" || property=="Resizable"){
            if(get){if(!arg(1))co_return finish(pointer_error);memory.store(GuestAddress{arg(1)},16,c->properties[property]?65535:0);}else c->properties[property]=arg(1)!=0;co_return finish(0);
        }
    }
    runtime_.module_events.push_back("webbrowser_unsupported="+std::string(name));co_return finish(not_impl);
    });
}
}
