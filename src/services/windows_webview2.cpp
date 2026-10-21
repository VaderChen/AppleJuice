#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/webview2_abi.hpp"
#include <algorithm>
#include <bit>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace x64arm {
// WebView2（WebView2Loader.dll 與 ICoreWebView2* COM 物件）對應宿主 WKWebView。
// 介面順序與 IID 取自 WebView2 SDK 產生的 webview2_abi.hpp；每種物件使用最高版本 vtable，
// QueryInterface 接受同一繼承鏈的全部版本。完成回呼經訊息迴圈在建立者的 UI 執行緒非同步送回。
// 參考：https://learn.microsoft.com/microsoft-edge/webview2/reference/win32/
struct WebView2State {
    enum class Kind {Environment,Controller,WebView,Settings};
    struct Object {Kind kind{};unsigned references{1};std::uint64_t controller{};};
    struct Controller {
        std::uint64_t window{},webview{},settings{},environment{};
        std::array<std::int32_t,4> bounds{};bool visible{true},closed{},native{};double zoom{1.0};
        std::u16string source{u"about:blank"};
    };
    struct Delivery {std::uint64_t handler{},object{};std::uint32_t result{};std::u16string text;bool has_text{};};
    std::map<std::uint64_t,Object> objects;
    std::map<std::uint64_t,Controller> controllers;
    std::map<std::uint64_t,std::map<std::string,std::uint32_t>> settings;
    std::map<std::uint64_t,Delivery> deliveries;
    std::map<Kind,std::uint64_t> tables;
    std::uint64_t next_delivery{1},next_token{1},next_script{1};
    std::set<std::string> reported;
};
namespace {
constexpr std::uint32_t not_implemented=0x80004001U,no_interface=0x80004002U,pointer_error=0x80004003U,invalid=0x80070057U,
    no_memory=0x8007000eU,invalid_state=0x8007139fU,invalid_window=0x80070578U,not_supported=0x80070032U;
constexpr std::string_view interface_names[]{"ICoreWebView2Environment8","ICoreWebView2Controller3","ICoreWebView2_10","ICoreWebView2Settings6"};
constexpr std::string_view kind_prefix[]{"Environment.","Controller.","WebView.","Settings."};
// 各物件可回應的 IID：IUnknown 與同一繼承鏈的全部版本。
const std::vector<std::array<std::uint8_t,16>>& interface_chain(WebView2State::Kind kind){
    static const auto chains=[]{
        std::array<std::vector<std::array<std::uint8_t,16>>,4> out;
        const std::array<std::pair<std::string_view,int>,4> bases{{{"ICoreWebView2Environment",8},{"ICoreWebView2Controller",3},{"ICoreWebView2",10},{"ICoreWebView2Settings",6}}};
        for(std::size_t k=0;k<4;++k){
            out[k].push_back({0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46});
            for(int version=1;version<=bases[k].second;++version){
                std::string name(bases[k].first);if(version>1)name+=k==2?"_"+std::to_string(version):std::to_string(version);
                if(const auto* item=webview2::find_interface(name))out[k].push_back(item->iid);
            }
        }
        return out;
    }();
    return chains[static_cast<std::size_t>(kind)];
}
// 預設設定值依 ICoreWebView2Settings 文件。
std::uint32_t default_setting(std::string_view property){
    return property=="IsPasswordAutosaveEnabled"?0U:1U;
}
}
std::span<const WindowsServiceDescriptor> webview2_loader_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"webview2loader.dll","CreateCoreWebView2EnvironmentWithOptions",4},{"webview2loader.dll","CreateCoreWebView2Environment",1},
        {"webview2loader.dll","GetAvailableCoreWebView2BrowserVersionString",2},{"webview2loader.dll","CompareBrowserVersions",3},
    };
    return table;
}
std::span<const WindowsServiceDescriptor> webview2_object_service_descriptors(){
    static const auto descriptors=[]{
        static std::vector<std::string> names;std::vector<WindowsServiceDescriptor> out;
        for(std::size_t k=0;k<4;++k){const auto* item=webview2::find_interface(interface_names[k]);for(const auto& method:item->methods)names.push_back(std::string(kind_prefix[k])+std::string(method.name));}
        names.push_back("Deliver");
        std::size_t at=0;
        for(std::size_t k=0;k<4;++k){const auto* item=webview2::find_interface(interface_names[k]);
            for(const auto& method:item->methods){WindowsServiceDescriptor descriptor{"internal-webview2",names[at++],method.arguments+1};out.push_back(descriptor);}}
        out.push_back(WindowsServiceDescriptor{"internal-webview2",names[at],1});
        return out;
    }();
    return descriptors;
}
std::optional<std::uint32_t> WindowsServices::invoke_webview2(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    using Kind=WebView2State::Kind;
    const WindowsCall arg(state,memory,service);const std::string_view symbol=service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    // WebView2 只有 64-bit 宿主路徑；Win32 的 RECT／double 傳值另需轉換。
    if(x86_)return finish(service.family=="webview2loader.dll"?not_supported:not_implemented);
    if(!webview2_)webview2_=std::make_shared<WebView2State>();
    auto& bridge=*webview2_;
    const auto store=[&](std::uint64_t at,unsigned bits,std::uint64_t value){memory.store(GuestAddress{at},static_cast<std::uint8_t>(bits),value);};
    const auto text=[&](std::uint64_t at){return at?read_utf16(memory,GuestAddress{at},UINT32_C(1)<<25):std::u16string{};};
    const auto task_string=[&](std::uint64_t out,std::u16string_view value)->std::uint32_t{
        if(!out)return pointer_error;
        const auto bytes=utf16_bytes(std::u16string(value));const auto copy=allocate_heap(memory,bytes.size());
        if(!copy){store(out,64,0);return no_memory;}
        memory.write(GuestAddress{copy},bytes);gui_.task_allocations.insert(copy);store(out,64,copy);return 0;
    };
    const auto method=[&](std::uint64_t object,unsigned slot){
        const auto target=memory.load(GuestAddress{memory.load(GuestAddress{object},64)+slot*8},64);
        if(!handles(GuestAddress{target}))memory.check(GuestAddress{target},1,Access::Execute);return target;
    };
    const auto create=[&](Kind kind,std::uint64_t controller)->std::uint64_t{
        auto& table=bridge.tables[kind];
        if(!table){
            const auto* item=webview2::find_interface(interface_names[static_cast<std::size_t>(kind)]);
            table=runtime_allocate(memory,{},item->methods.size()*8);
            for(std::size_t i=0;i<item->methods.size();++i)
                store(table+i*8,64,resolve("internal-webview2",std::string(kind_prefix[static_cast<std::size_t>(kind)])+std::string(item->methods[i].name)).value);
        }
        const auto pointer=allocate_heap(memory,16);if(!pointer)return 0;
        store(pointer,64,table);store(pointer+8,64,0);bridge.objects.emplace(pointer,WebView2State::Object{kind,1,controller});return pointer;
    };
    // 完成處理常式：建立時 AddRef，之後於訊息迴圈呼叫 Invoke，再 Release 處理常式與傳遞用的物件參照。
    const auto queue=[&](std::uint64_t handler,std::uint32_t result,std::uint64_t object,std::optional<std::u16string> value={}){
        const auto id=bridge.next_delivery++;WebView2State::Delivery delivery;delivery.handler=handler;delivery.object=object;delivery.result=result;
        if(value){delivery.text=*value;delivery.has_text=true;}
        bridge.deliveries.emplace(id,std::move(delivery));
        GuiSentMessage request;request.receiver=threads_.current;request.abandoned=true;
        request.rpc=GuestCallback{resolve("internal-webview2","Deliver").value,{id}};
        gui_.sent_messages.emplace(gui_.next_object++,std::move(request));
        start_callbacks(state,memory,{{method(handler,1),{handler}}},Continuation::Return,0);return std::optional<std::uint32_t>{};
    };
    const auto report=[&](std::string_view name){
        if(bridge.reported.insert(std::string(name)).second)runtime_.module_events.push_back("webview2_note=尚未支援 "+std::string(name));
    };
    if(symbol=="CreateCoreWebView2EnvironmentWithOptions" || symbol=="CreateCoreWebView2Environment"){
        const auto handler=arg(symbol=="CreateCoreWebView2Environment"?0:3);if(!handler)return finish(pointer_error);
        const auto environment=create(Kind::Environment,0);if(!environment)return finish(no_memory);
        return queue(handler,0,environment);
    }
    if(symbol=="GetAvailableCoreWebView2BrowserVersionString"){
        if(!arg(1))return finish(pointer_error);
        return finish(task_string(arg(1),u"120.0.2210.91"));
    }
    if(symbol=="CompareBrowserVersions"){
        if(!arg(0) || !arg(1) || !arg(2))return finish(pointer_error);
        const auto parts=[](const std::u16string& value){std::vector<std::uint64_t> out;std::uint64_t n=0;bool digit=false;for(const auto c:value){if(c>=u'0' && c<=u'9'){n=n*10+(c-u'0');digit=true;}else if(c==u'.'){out.push_back(n);n=0;digit=false;}else break;}if(digit || !out.empty())out.push_back(n);return out;};
        auto a=parts(text(arg(0))),b=parts(text(arg(1)));a.resize(4);b.resize(4);
        store(arg(2),32,a<b?static_cast<std::uint32_t>(-1):a>b?1U:0U);return finish(0);
    }
    if(symbol=="Deliver"){
        const auto found=bridge.deliveries.find(arg(0));if(found==bridge.deliveries.end())return finish(0);
        auto delivery=std::move(found->second);bridge.deliveries.erase(found);
        std::vector<GuestCallback> callbacks;std::uint64_t scratch=0;
        if(delivery.has_text){
            const auto bytes=utf16_bytes(delivery.text);scratch=allocate_heap(memory,bytes.size());if(scratch)memory.write(GuestAddress{scratch},bytes);
            callbacks.push_back({method(delivery.handler,3),{delivery.handler,delivery.result,scratch}});
        }else callbacks.push_back({method(delivery.handler,3),{delivery.handler,delivery.result,delivery.object}});
        callbacks.push_back({method(delivery.handler,2),{delivery.handler}});
        if(delivery.object && !delivery.has_text)callbacks.push_back({method(delivery.object,2),{delivery.object}});
        start_callbacks(state,memory,std::move(callbacks),scratch?Continuation::ReturnZeroFreeHeap:Continuation::Return,scratch);return std::nullopt;
    }
    // 物件方法：symbol 為「種類.方法」。
    const auto dot=symbol.find('.');if(dot==std::string_view::npos)return finish(not_implemented);
    const auto member=symbol.substr(dot+1);
    const auto self=arg(0);const auto found=bridge.objects.find(self);if(found==bridge.objects.end())return finish(invalid);
    auto& object=found->second;const auto kind=object.kind;
    if(member=="QueryInterface"){
        if(!arg(2))return finish(pointer_error);
        store(arg(2),64,0);if(!arg(1))return finish(invalid);
        std::array<std::uint8_t,16> iid{};memory.read(GuestAddress{arg(1)},iid);
        const auto& chain=interface_chain(kind);
        if(std::find(chain.begin(),chain.end(),iid)==chain.end())return finish(no_interface);
        ++object.references;store(arg(2),64,self);return finish(0);
    }
    if(member=="AddRef"){++object.references;return finish(object.references);}
    if(member=="Release"){
        if(object.references)--object.references;
        // Controller 最後一個參照釋放即關閉原生檢視；子物件記憶體隨 Controller 保留，避免懸置指標。
        if(!object.references && kind==Kind::Controller)if(auto controller=bridge.controllers.find(self);controller!=bridge.controllers.end() && !controller->second.closed){
            controller->second.closed=true;if(native_gui_ && controller->second.native)native_gui_->webview_destroy(self);
        }
        return finish(object.references);
    }
    // 事件登錄：記錄 token，目前不產生任何事件。
    if(member.starts_with("add_")){
        if(!arg(1) || !arg(2))return finish(pointer_error);
        store(arg(2),64,bridge.next_token++);return finish(0);
    }
    if(member.starts_with("remove_"))return finish(0);
    if(kind==Kind::Environment){
        if(member=="CreateCoreWebView2Controller"){
            const auto window=arg(1),handler=arg(2);if(!handler)return finish(pointer_error);
            if(!gui_.windows.contains(window))return finish(invalid_window);
            const auto controller=create(Kind::Controller,0);if(!controller)return finish(no_memory);
            const auto webview=create(Kind::WebView,controller),settings=create(Kind::Settings,controller);
            if(!webview || !settings)return finish(no_memory);
            WebView2State::Controller record;record.window=window;record.webview=webview;record.settings=settings;record.environment=self;
            record.native=native_gui_ && native_gui_->webview_create(controller,window);
            bridge.controllers.emplace(controller,std::move(record));
            return queue(handler,0,controller);
        }
        if(member=="get_BrowserVersionString")return finish(task_string(arg(1),u"120.0.2210.91"));
        report(symbol);return finish(not_implemented);
    }
    const auto controller_key=kind==Kind::Controller?self:object.controller;
    const auto controller=bridge.controllers.find(controller_key);if(controller==bridge.controllers.end())return finish(invalid_state);
    auto& view=controller->second;
    if(view.closed)return finish(invalid_state);
    const bool native=native_gui_ && view.native;
    if(kind==Kind::Settings){
        auto& values=bridge.settings[self];
        if(member=="get_UserAgent")return finish(task_string(arg(1),u"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 Edg/120.0.2210.91"));
        if(member=="put_UserAgent")return finish(0);
        if(member.starts_with("get_")){
            if(!arg(1))return finish(pointer_error);
            const auto property=std::string(member.substr(4));const auto value=values.find(property);
            store(arg(1),32,value==values.end()?default_setting(property):value->second);return finish(0);
        }
        if(member.starts_with("put_")){values[std::string(member.substr(4))]=static_cast<std::uint32_t>(arg(1))?1U:0U;return finish(0);}
        report(symbol);return finish(not_implemented);
    }
    if(kind==Kind::Controller){
        if(member=="get_IsVisible"){if(!arg(1))return finish(pointer_error);store(arg(1),32,view.visible?1:0);return finish(0);}
        if(member=="put_IsVisible"){view.visible=static_cast<std::uint32_t>(arg(1))!=0;if(native)native_gui_->webview_visible(self,view.visible);return finish(0);}
        if(member=="get_Bounds"){if(!arg(1))return finish(pointer_error);for(unsigned i=0;i<4;++i)store(arg(1)+i*4,32,static_cast<std::uint32_t>(view.bounds[i]));return finish(0);}
        // x64 呼叫慣例：16-byte RECT 傳值時由呼叫端複製並傳入其位址。
        const auto read_rect=[&](std::uint64_t at){std::array<std::int32_t,4> rect{};for(unsigned i=0;i<4;++i)rect[i]=static_cast<std::int32_t>(memory.load(GuestAddress{at+i*4},32));return rect;};
        const auto xmm_double=[&](unsigned index){std::uint64_t bits=0;for(unsigned i=0;i<8;++i)bits|=static_cast<std::uint64_t>(state.xmm[index][i])<<(i*8);return std::bit_cast<double>(bits);};
        if(member=="put_Bounds"){if(!arg(1))return finish(pointer_error);view.bounds=read_rect(arg(1));if(native)native_gui_->webview_bounds(self,view.bounds);return finish(0);}
        if(member=="SetBoundsAndZoomFactor"){
            if(!arg(1))return finish(pointer_error);view.bounds=read_rect(arg(1));view.zoom=xmm_double(2);
            if(native){native_gui_->webview_bounds(self,view.bounds);native_gui_->webview_zoom(self,view.zoom);}return finish(0);
        }
        if(member=="get_ZoomFactor"){if(!arg(1))return finish(pointer_error);store(arg(1),64,std::bit_cast<std::uint64_t>(view.zoom));return finish(0);}
        if(member=="put_ZoomFactor"){const auto zoom=xmm_double(1);if(!(zoom>0))return finish(invalid);view.zoom=zoom;if(native)native_gui_->webview_zoom(self,zoom);return finish(0);}
        if(member=="get_RasterizationScale"){if(!arg(1))return finish(pointer_error);store(arg(1),64,std::bit_cast<std::uint64_t>(1.0));return finish(0);}
        if(member=="put_RasterizationScale" || member=="put_ShouldDetectMonitorScaleChanges" || member=="put_BoundsMode" || member=="put_DefaultBackgroundColor")return finish(0);
        if(member=="get_ShouldDetectMonitorScaleChanges"){if(!arg(1))return finish(pointer_error);store(arg(1),32,1);return finish(0);}
        if(member=="get_BoundsMode"){if(!arg(1))return finish(pointer_error);store(arg(1),32,0);return finish(0);}
        if(member=="get_DefaultBackgroundColor"){if(!arg(1))return finish(pointer_error);store(arg(1),32,0xffffffffU);return finish(0);}
        if(member=="MoveFocus"){if(native)native_gui_->webview_focus(self);return finish(0);}
        if(member=="get_ParentWindow"){if(!arg(1))return finish(pointer_error);store(arg(1),64,view.window);return finish(0);}
        if(member=="put_ParentWindow"){
            if(!gui_.windows.contains(arg(1)))return finish(invalid_window);
            if(arg(1)!=view.window){
                if(native)native_gui_->webview_destroy(self);
                view.window=arg(1);view.native=native_gui_ && native_gui_->webview_create(self,view.window);
                if(view.native){native_gui_->webview_bounds(self,view.bounds);native_gui_->webview_visible(self,view.visible);native_gui_->webview_navigate(self,view.source);}
            }
            return finish(0);
        }
        if(member=="NotifyParentWindowPositionChanged")return finish(0);
        if(member=="Close"){view.closed=true;if(native)native_gui_->webview_destroy(self);return finish(0);}
        if(member=="get_CoreWebView2"){
            if(!arg(1))return finish(pointer_error);++bridge.objects.at(view.webview).references;store(arg(1),64,view.webview);return finish(0);
        }
        report(symbol);return finish(not_implemented);
    }
    // ICoreWebView2_10。
    if(member=="get_Settings"){if(!arg(1))return finish(pointer_error);++bridge.objects.at(view.settings).references;store(arg(1),64,view.settings);return finish(0);}
    if(member=="get_Environment"){if(!arg(1))return finish(pointer_error);++bridge.objects.at(view.environment).references;store(arg(1),64,view.environment);return finish(0);}
    if(member=="get_Source")return finish(task_string(arg(1),view.source));
    if(member=="Navigate"){
        if(!arg(1))return finish(pointer_error);const auto uri=text(arg(1));
        if(uri.find(u':')==std::u16string::npos)return finish(invalid);
        view.source=uri;if(native)native_gui_->webview_navigate(controller_key,uri);return finish(0);
    }
    if(member=="NavigateToString"){
        if(!arg(1))return finish(pointer_error);
        // WebView2 對字串內容回報 about:blank 為來源。
        view.source=u"about:blank";if(native)native_gui_->webview_load_html(controller_key,text(arg(1)));return finish(0);
    }
    if(member=="Reload"){if(native)native_gui_->webview_command(controller_key,0);return finish(0);}
    if(member=="Stop"){if(native)native_gui_->webview_command(controller_key,1);return finish(0);}
    if(member=="GoBack"){if(native)native_gui_->webview_command(controller_key,2);return finish(0);}
    if(member=="GoForward"){if(native)native_gui_->webview_command(controller_key,3);return finish(0);}
    if(member=="get_CanGoBack" || member=="get_CanGoForward" || member=="get_ContainsFullScreenElement" || member=="get_IsSuspended" || member=="get_IsMuted" ||
       member=="get_IsDocumentPlayingAudio" || member=="get_IsDefaultDownloadDialogOpen"){
        if(!arg(1))return finish(pointer_error);store(arg(1),32,0);return finish(0);
    }
    if(member=="put_IsMuted")return finish(0);
    if(member=="get_BrowserProcessId"){if(!arg(1))return finish(pointer_error);store(arg(1),32,static_cast<std::uint32_t>(process_id()));return finish(0);}
    if(member=="get_DocumentTitle")return finish(task_string(arg(1),{}));
    if(member=="ExecuteScript"){
        if(!arg(1))return finish(pointer_error);
        if(native)native_gui_->webview_script(controller_key,text(arg(1)));
        // 宿主腳本非同步執行；結果 JSON 目前一律回報 null。
        if(!arg(2))return finish(0);
        return queue(arg(2),0,0,std::u16string(u"null"));
    }
    if(member=="AddScriptToExecuteOnDocumentCreated"){
        if(!arg(1))return finish(pointer_error);
        if(native)native_gui_->webview_user_script(controller_key,text(arg(1)));
        const auto id=bridge.next_script++;
        if(!arg(2))return finish(0);
        return queue(arg(2),0,0,utf8_to_utf16(std::to_string(id)));
    }
    if(member=="RemoveScriptToExecuteOnDocumentCreated")return finish(0);
    if(member=="PostWebMessageAsJson" || member=="PostWebMessageAsString"){
        if(!arg(1))return finish(pointer_error);
        if(native)native_gui_->webview_post_message(controller_key,text(arg(1)),member=="PostWebMessageAsJson");return finish(0);
    }
    if(member=="OpenDevToolsWindow" || member=="OpenTaskManagerWindow" || member=="OpenDefaultDownloadDialog" || member=="CloseDefaultDownloadDialog" || member=="Resume")return finish(0);
    report(symbol);return finish(not_implemented);
}
}
