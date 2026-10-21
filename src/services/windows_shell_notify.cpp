#include "x64arm/windows_services.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/unicode.hpp"
#include "directory_watch.hpp"
#include <algorithm>
#include <functional>

namespace x64arm {
std::span<const WindowsServiceDescriptor> shell_notification_service_descriptors() {
    static constexpr WindowsServiceDescriptor table[]{
        {"shell32.dll","SHChangeNotifyRegister",6},{"shell32.dll","SHChangeNotifyDeregister",1},
        {"shell32.dll","SHChangeNotification_Lock",4},{"shell32.dll","SHChangeNotification_Unlock",1},
        {"shell32.dll","SHCreateDirectoryExA",3},{"shell32.dll","SHCreateDirectoryExW",3},{"shell32.dll","SHCreateDirectory",2}
    };return table;
}
namespace {
constexpr std::uint32_t all_events=0x7fffffffU, interrupt_event=0x80000000U;
std::u16string canonical(std::u16string path) {
    std::replace(path.begin(),path.end(),u'/',u'\\');
    if(path.size()<3 || path[1]!=u':' || path[2]!=u'\\')return {};
    if(path[0]>=u'a' && path[0]<=u'z')path[0]-=32;
    if(path[0]<u'A' || path[0]>u'Z')return {};
    std::u16string result=path.substr(0,3);
    for(std::size_t at=3;at<path.size();){
        auto end=path.find(u'\\',at);if(end==path.npos)end=path.size();const auto part=path.substr(at,end-at);at=end+1;
        if(part==u".."){if(result.size()>3){const auto slash=result.find_last_of(u'\\');result.resize(slash<3?3:slash);}}
        else if(!part.empty() && part!=u"."){
            if(part.find_first_of(u"\"<>|:*?")!=part.npos)return {};
            if(result.size()>3)result+=u'\\';result+=part;
        }
    }return result;
}
std::u16string fold(std::u16string value){for(auto& c:value)if(c>=u'A' && c<=u'Z')c+=32;return value;}
std::u16string parent(const std::u16string& path){const auto at=path.find_last_of(u'\\');return path.substr(0,at<3?3:at);}
bool matches(const GuiState::ShellRegistration& registration,const std::array<std::u16string,2>& paths,std::uint32_t event,bool notify_children=false) {
    if(registration.root.empty() || (event&0x0c0581e0U))return true;
    const auto root=fold(registration.root);
    return std::any_of(paths.begin(),paths.end(),[&](const auto& value){
        if(value.empty())return false;const auto path=fold(value);
        if(path==root || fold(parent(value))==root)return true;
        if(notify_children && root.starts_with(path.ends_with(u'\\')?path:path+u'\\'))return true;
        const auto prefix=root.ends_with(u'\\')?root:root+u'\\';
        return registration.recursive && path.starts_with(prefix);
    });
}
std::map<std::u16string,FileMetadata> snapshot(ReadOnlyFiles& filesystem,const std::u16string& root,bool subtree) {
    std::map<std::u16string,FileMetadata> items;
    std::function<void(const std::u16string&,unsigned)> visit=[&](const auto& path,unsigned depth){
        if(depth>128 || items.size()>=4096)throw std::length_error("Shell 監看目錄超過上限");
        const auto listing=filesystem.list(utf16_to_utf8(path));if(listing.error)throw std::invalid_argument("Shell 監看目錄無法讀取");
        for(const auto& item:listing.entries){if(items.size()>=4096)throw std::length_error("Shell 監看項目超過上限");
            auto child=path+(path.ends_with(u'\\')?u"":u"\\")+utf8_to_utf16(item.name);items.emplace(child,item.metadata);
            if(subtree && (item.metadata.attributes&16) && !(item.metadata.attributes&0x400))visit(child,depth+1);
        }
    };visit(root,0);return items;
}
}
void WindowsServices::release_shell_delivery(GuestMemory& memory,std::uint64_t token) {
    const auto found=gui_.shell_deliveries.find(token);if(found==gui_.shell_deliveries.end())return;
    const auto& delivery=found->second;if(!delivery.consumed || delivery.active || delivery.locks)return;
    // 配置只由通知服務持有；接收者可讀 PIDL，不能以 ILFree 取得所有權。
    for(const auto address:delivery.allocations){gui_.shell_pidls.erase(address);if(allocations_.contains(address))(void)free_heap(memory,address,GuestState{});}
    gui_.shell_deliveries.erase(found);
}
void WindowsServices::consume_shell_message(const GuiMessage& message) {
    const auto found=gui_.shell_deliveries.find(message.wparam);
    if(found!=gui_.shell_deliveries.end() && found->second.window==message.window && found->second.message==message.message &&
       message.lparam==(found->second.shared?process_id():found->second.event))found->second.consumed=true;
}
void WindowsServices::retire_shell_messages(GuestMemory& memory) {
    std::vector<std::uint64_t> retired;
    for(const auto& [token,delivery]:gui_.shell_deliveries)if(delivery.thread==threads_.current && delivery.consumed && !delivery.active && !delivery.locks)retired.push_back(token);
    for(const auto token:retired)release_shell_delivery(memory,token);
}
void WindowsServices::retain_shell_callback(CallbackFrame& frame,const GuestCallback& call) {
    if(call.arguments.size()!=4)return;
    const auto found=gui_.shell_deliveries.find(call.arguments[2]);if(found==gui_.shell_deliveries.end())return;
    auto& delivery=found->second;
    if(call.arguments[0]!=delivery.window || static_cast<std::uint32_t>(call.arguments[1])!=delivery.message ||
       call.arguments[3]!=(delivery.shared?process_id():delivery.event))return;
    if(std::find(frame.shell_deliveries.begin(),frame.shell_deliveries.end(),found->first)!=frame.shell_deliveries.end())return;
    frame.shell_deliveries.push_back(found->first);++delivery.active;
}
void WindowsServices::finish_shell_callbacks(GuestMemory& memory,const CallbackFrame& frame) {
    for(const auto token:frame.shell_deliveries){const auto found=gui_.shell_deliveries.find(token);if(found!=gui_.shell_deliveries.end() && found->second.active)--found->second.active;release_shell_delivery(memory,token);}
}
void WindowsServices::shell_notify(GuestMemory& memory,std::uint32_t event,const std::array<std::u16string,2>& paths,bool interrupt,std::vector<GuestCallback>* flush,std::uint32_t target,bool notify_children) {
    for(const auto& [id,registration]:gui_.shell_registrations){
        if(target && target!=id)continue;
        if(!(registration.sources&(interrupt?1U:2U)) || !(registration.events&(event&all_events)) || !matches(registration,paths,event,notify_children) || !gui_.windows.contains(registration.window))continue;
        if(gui_.shell_deliveries.size()>=1024 || (!flush && gui_.queue.size()>=10000)){last_error_=1816;continue;}
        GuiState::ShellDelivery delivery;delivery.allocations.reserve(3);delivery.registration=id;delivery.window=registration.window;delivery.message=registration.message;
        delivery.thread=gui_.windows.at(registration.window).thread;delivery.event=event&all_events;delivery.shared=(registration.sources&0x8000)!=0;
        const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
        try{
            delivery.array=allocate_heap(memory,unit*2);if(!delivery.array){last_error_=8;continue;}delivery.allocations.push_back(delivery.array);
            for(unsigned n=0;n<2;++n){std::uint64_t pidl=0;if(!paths[n].empty()){
                pidl=shell_make_pidl(memory,paths[n]);if(!pidl)throw std::bad_alloc();delivery.allocations.push_back(pidl);
            }memory.store(GuestAddress{delivery.array+n*unit},bits,pidl);}
            const auto token=delivery.shared?runtime_.allocate_wait_handle():delivery.array;
            const auto wp=token;const auto lp=delivery.shared?process_id():delivery.event;
            delivery.consumed=flush!=nullptr;gui_.shell_deliveries.emplace(token,std::move(delivery));
            try{if(flush)flush->push_back({resolve("user32.dll","SendMessageW").value,{registration.window,registration.message,wp,lp}});
                else enqueue_gui_message({registration.window,wp,lp,registration.message});}
            catch(...){gui_.shell_deliveries.at(token).consumed=true;release_shell_delivery(memory,token);throw;}
        }catch(const std::bad_alloc&){for(const auto pointer:delivery.allocations)if(allocations_.contains(pointer)){gui_.shell_pidls.erase(pointer);(void)free_heap(memory,pointer,GuestState{});}last_error_=8;}
    }
    for(const auto& [window,browser]:gui_.shell_browsers)if(gui_.windows.contains(window)){
        GuiState::ShellRegistration selection;selection.root=browser.directory;selection.recursive=true;
        if(matches(selection,paths,event,true))shell_browse_refresh(window);
    }
}
void WindowsServices::poll_shell_notifications(GuestMemory& memory) {
    std::erase_if(gui_.shell_registrations,[&](const auto& item){return !gui_.windows.contains(item.second.window);});
    std::erase_if(shell_watches_,[&](const auto& item){return !gui_.shell_registrations.contains(item.first);});
    std::vector<std::uint64_t> stale;
    for(auto& [token,delivery]:gui_.shell_deliveries)if(!gui_.windows.contains(delivery.window) || !gui_.shell_registrations.contains(delivery.registration)){
        delivery.consumed=true;std::erase_if(gui_.queue,[&](const auto& message){return message.window==delivery.window && message.message==delivery.message && message.wparam==token;});stale.push_back(token);
    }
    for(const auto token:stale)release_shell_delivery(memory,token);
    for(auto& [id,watches]:shell_watches_){const auto registration=gui_.shell_registrations.find(id);if(registration==gui_.shell_registrations.end())continue;
        const auto emit=[&](std::uint32_t event,const std::array<std::u16string,2>& paths){shell_notify(memory,event,paths,true,nullptr,id);};
        for(auto& watch:watches){
            watch.native->poll();std::vector<std::uint8_t> bytes(65536);const auto result=watch.native->take(bytes);if(!result)continue;
            if(result->error || !result->bytes){emit(0x1000,{watch.root,{}});continue;}
            std::vector<std::pair<std::uint32_t,std::u16string>> records;
            for(std::size_t at=0;at<result->bytes;){
                const auto u32=[&](std::size_t offset){std::uint32_t value=0;for(unsigned j=0;j<4;++j)value|=static_cast<std::uint32_t>(bytes[at+offset+j])<<(8*j);return value;};
                if(result->bytes-at<12){records.clear();break;}const auto next=u32(0),length=u32(8);
                if(length&1U || length>result->bytes-at-12 || (next && (next<12+length || (next&3U) || next>result->bytes-at))){records.clear();break;}
                std::u16string suffix;for(unsigned j=0;j<length;j+=2)suffix.push_back(static_cast<char16_t>(bytes[at+12+j]|(static_cast<unsigned>(bytes[at+13+j])<<8)));
                const auto path=canonical(watch.root+(watch.root.ends_with(u'\\')?u"":u"\\")+suffix);
                if(path.empty()){records.clear();break;}records.emplace_back(u32(4),path);if(!next)break;at+=next;
            }
            if(records.empty()){emit(0x1000,{watch.root,{}});continue;}
            for(std::size_t n=0;n<records.size();++n){const auto& [action,path]=records[n];
                const auto old=watch.known.find(path);const auto current=filesystem_.information(utf16_to_utf8(path));
                const auto attributes=old!=watch.known.end()?old->second.attributes:current.error?0U:current.metadata.attributes;
                const bool directory=(attributes&16)!=0;
                if(action==4 && n+1<records.size() && records[n+1].first==5){
                    const auto destination=records[++n].second;emit(directory?0x20000:1,{path,destination});
                    if(old!=watch.known.end()){const auto info=old->second;watch.known.erase(old);watch.known[destination]=info;}
                }else if(action==1)emit(directory?8:2,{path,{}});
                else if(action==2)emit(directory?16:4,{path,{}});
                else if(action==3){
                    if(old!=watch.known.end() && !current.error && old->second.attributes!=current.metadata.attributes)emit(0x800,{path,{}});
                    emit(directory?0x1000:0x2000,{path,{}});
                }else emit(0x1000,{watch.root,{}});
            }
            try{watch.known=snapshot(filesystem_,watch.root,registration->second.recursive && (registration->second.sources&0x1000));}
            catch(const std::exception&){watch.known.clear();}
        }
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_shell_notification(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service) {
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;const auto u32=[&](unsigned n){return static_cast<std::uint32_t>(arg(n));};
    const auto finish=[&](std::uint64_t result){arg.return_to(state,result);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(symbol.starts_with("SHCreateDirectory")?error:0);};
    const std::uint8_t bits=x86_?32:64;const unsigned unit=x86_?4:8;
    try{
        if(symbol=="SHChangeNotifyRegister"){
            const auto source=u32(1),events=u32(2),message=u32(3);
            if(!gui_.windows.contains(arg(0)) || gui_.windows.at(arg(0)).ended)return fail(1400);
            if(!(source&3) || (source&~0x9003U) || !events || (events&interrupt_event) || !message || message>65535 || u32(4)!=1 || !arg(5))return fail(87);
            memory.check(GuestAddress{arg(5)},x86_?8:16,Access::Read);const bool recursive=memory.load(GuestAddress{arg(5)+unit},32)!=0;
            if((source&0x1000) && (!(source&1) || !recursive))return fail(87);
            if(gui_.shell_registrations.size()>=64 || gui_.next_shell_registration==UINT32_MAX)return fail(8);
            std::u16string root;const auto pidl=memory.load(GuestAddress{arg(5)},bits);
            if(pidl){const auto item=gui_.shell_pidls.find(pidl);if(item==gui_.shell_pidls.end() || !allocations_.contains(pidl))return fail(87);root=item->second;}
            std::vector<ShellWatch> watches;
            if(source&1){
                if(!runtime_.network.external_io)throw HostCallFault("比較模式不允許監看宿主 Shell 目錄");
                std::vector<std::u16string> roots;if(root.empty()){for(char16_t letter=u'A';letter<=u'Z';++letter){std::u16string candidate{letter,u':',u'\\'};if(!filesystem_.information(utf16_to_utf8(candidate)).error)roots.push_back(candidate);}}
                else{const auto info=filesystem_.information(utf16_to_utf8(root));if(info.error)return fail(info.error);roots.push_back(info.metadata.attributes&16?root:parent(root));}
                for(const auto& candidate:roots){const auto opened=filesystem_.open_directory_observer(utf16_to_utf8(candidate));if(opened.error)return fail(opened.error);
                    std::uint32_t error=0;ShellWatch watch;watch.root=candidate;watch.native=DirectoryWatch::create(*opened.file,recursive && (source&0x1000),0x11f,65536,error);if(!watch.native)return fail(error);
                    watch.known=snapshot(filesystem_,candidate,recursive && (source&0x1000));watches.push_back(std::move(watch));}
                if(watches.empty())return fail(3);
            }
            const auto id=gui_.next_shell_registration++;GuiState::ShellRegistration registration{arg(0),source,events,message,std::move(root),recursive};
            shell_watches_.emplace(id,std::move(watches));try{gui_.shell_registrations.emplace(id,std::move(registration));}catch(...){shell_watches_.erase(id);throw;}return finish(id);
        }
        if(symbol=="SHChangeNotifyDeregister"){
            if(!gui_.shell_registrations.erase(u32(0)))return finish(0);shell_watches_.erase(u32(0));poll_shell_notifications(memory);return finish(1);
        }
        if(symbol=="SHChangeNotification_Lock"){
            if(arg(2)){memory.check(GuestAddress{arg(2)},unit,Access::Write);memory.store(GuestAddress{arg(2)},bits,0);}
            if(arg(3)){memory.check(GuestAddress{arg(3)},4,Access::Write);memory.store(GuestAddress{arg(3)},32,0);}
            const auto found=gui_.shell_deliveries.find(arg(0));
            if(found==gui_.shell_deliveries.end() || !found->second.shared || u32(1)!=process_id())return fail(6);
            if(gui_.shell_locks.size()>=1024)return fail(8);const auto lock=runtime_.allocate_wait_handle();gui_.shell_locks.emplace(lock,found->first);++found->second.locks;
            if(arg(2))memory.store(GuestAddress{arg(2)},bits,found->second.array);if(arg(3))memory.store(GuestAddress{arg(3)},32,found->second.event);return finish(lock);
        }
        if(symbol=="SHChangeNotification_Unlock"){
            const auto found=gui_.shell_locks.find(arg(0));if(found==gui_.shell_locks.end())return fail(6);const auto token=found->second;gui_.shell_locks.erase(found);
            const auto delivery=gui_.shell_deliveries.find(token);if(delivery!=gui_.shell_deliveries.end() && delivery->second.locks)--delivery->second.locks;
            release_shell_delivery(memory,token);return finish(1);
        }
        if(symbol.starts_with("SHCreateDirectoryEx") || symbol=="SHCreateDirectory"){
            // SHCreateDirectory（序號 165）為 Unicode、沒有安全屬性的 SHCreateDirectoryExW。
            const bool wide=symbol.ends_with('W') || symbol=="SHCreateDirectory";
            if(!arg(1))return finish(87);if(arg(0) && !gui_.windows.contains(arg(0)))return finish(1400);
            const auto ansi=wide?std::string{}:read_ansi(memory,arg(1));
            if(!wide && ansi.size()>=248)return finish(206);
            const auto path=wide?read_utf16(memory,GuestAddress{arg(1)}):utf8_to_utf16(ansi);
            if(path.size()>=248)return finish(206);auto destination=canonical(path);if(destination.empty())return finish(161);
            const auto security=synchronization_security(memory,symbol=="SHCreateDirectory"?0:arg(2),x86_);if(security.error)return finish(security.error);
            if(filesystem_.writable() && !runtime_.network.external_io)throw HostCallFault("比較模式不允許宿主 Shell 目錄建立");
            const auto existing=filesystem_.information(utf16_to_utf8(destination));if(!existing.error)return finish(183);
            for(std::size_t at=3;at<=destination.size();++at)if(at==destination.size() || destination[at]==u'\\'){
                const auto prefix=destination.substr(0,at);const auto info=filesystem_.information(utf16_to_utf8(prefix));
                if(info.error){const auto error=filesystem_.create_directory(utf16_to_utf8(prefix));if(error && error!=183)return finish(error);const auto verify=filesystem_.information(utf16_to_utf8(prefix));if(verify.error)return finish(verify.error);if(!(verify.metadata.attributes&16))return finish(3);if(!error)shell_notify(memory,8,{prefix,{}},false);}
                else if(!(info.metadata.attributes&16))return finish(3);
                const auto visible=filesystem_.list(utf16_to_utf8(prefix));if(visible.error)return finish(visible.error==5?1223:visible.error);
            }
            return finish(0);
        }
        if(symbol=="SHChangeNotify"){
            const auto event=u32(0),flags=u32(1),type=flags&0xffU;
            if(!event || (flags&~0x130ffU) || (flags&0x3000U)==0x2000)return fail(87);if(type!=0 && type!=1 && type!=5)return fail(50);
            std::array<std::u16string,2> paths;
            for(unsigned n=0;n<2;++n)if(arg(n+2)){
                if(type==0){const auto item=gui_.shell_pidls.find(arg(n+2));if(item==gui_.shell_pidls.end() || !allocations_.contains(item->first))return fail(87);paths[n]=item->second;}
                else paths[n]=canonical(type==5?read_utf16(memory,GuestAddress{arg(n+2)}):utf8_to_utf16(read_ansi(memory,arg(n+2))));
                if(paths[n].empty() || paths[n].size()>=260)return fail(87);
            }
            std::vector<GuestCallback> callbacks;shell_notify(memory,event,paths,(event&interrupt_event)!=0,(flags&0x3000U)==0x1000?&callbacks:nullptr,0,(flags&0x10000U)!=0);
            if(!callbacks.empty()){start_callbacks(state,memory,std::move(callbacks),Continuation::Return,0);return std::nullopt;}return finish(0);
        }
        throw HostCallFault("未知 Shell 通知入口");
    }catch(const std::bad_alloc&){return fail(8);}catch(const std::length_error&){return fail(8);}catch(const std::invalid_argument&){return fail(87);}
}
}
