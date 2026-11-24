    if(service.family=="msvfw32.dll" && name.starts_with("MCIWnd")){
        return nested_continuation(state,memory,"MCIWnd 建立",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                using service_callback::Task;
                const WindowsCall args(s,m,service);
                const auto finish=[&](std::uint64_t value){args.return_to(s,value);return std::optional<std::uint32_t>{};};
                const auto api=[&](std::string_view family,std::string_view symbol,std::vector<std::uint64_t> values)
                    ->Task<std::uint64_t>{co_return co_await service_callback::call_value(resolve(family,symbol).value,std::move(values));};
                GuestScratch scratch(*this,m,s);
                try{
                    const auto class_name=scratch.text(vfw::mci_window_class);
                    if(!class_name)co_return finish(0);
                    if(!gui_class_record(vfw::mci_window_class,false)){
                        const GuiClassAbi abi(x86_,false);
                        const auto record=scratch.allocate(abi.size);
                        if(!record)co_return finish(0);
                        const auto module=scratch.text(u"msvfw32.dll");
                        const auto library=co_await api("kernel32.dll","GetModuleHandleW",{module});
                        const auto cursor=co_await api("user32.dll","LoadCursorW",{0,32512});
                        m.store(GuestAddress{record+abi.style},32,0x402b);
                        m.store(GuestAddress{record+abi.procedure},abi.pointer_bits,resolve("internal-avi-mciwnd","WindowProc").value);
                        m.store(GuestAddress{record+abi.instance},abi.pointer_bits,library);
                        m.store(GuestAddress{record+abi.cursor},abi.pointer_bits,cursor);
                        m.store(GuestAddress{record+abi.background},abi.pointer_bits,6);
                        m.store(GuestAddress{record+abi.name},abi.pointer_bits,class_name);
                        if(!(co_await api("user32.dll","RegisterClassW",{record})) &&
                           !gui_class_record(vfw::mci_window_class,false))co_return finish(0);
                    }
                    if(service.symbol=="MCIWndRegisterClass")co_return finish(1);
                    std::uint64_t file=0;
                    if(args(3)){
                        if(service.symbol.ends_with('W'))file=args(3);
                        else file=scratch.text(ansi_to_utf16_lossy(read_ansi(m,args(3),32767)));
                        if(!file)co_return finish(0);
                    }
                    const auto parent=args(0),instance=args(1)?args(1):runtime_.image.base;
                    auto style=static_cast<std::uint32_t>(args(2));
                    if(!(style&0xc0000000U))style|=parent?0x50800000U:0x10cf0000U;
                    style|=0x06000000U;
                    const auto window=co_await api("user32.dll","CreateWindowExW",
                        {0,class_name,0,style,0,0,300,28,parent,style&0x40000000U?66U:0U,instance,file});
                    co_return finish(window);
                }catch(const MemoryFault&){co_return finish(0);}
                catch(const std::bad_alloc&){co_return finish(0);}
            });
    }
    if(service.family=="internal-avi-mciwnd"){
        return nested_continuation(state,memory,"MCIWnd 視窗",
            [this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                using service_callback::Task;
                const WindowsCall args(s,m,service);
                using namespace vfw;
                const auto hwnd=args(0),wp=args(2),lp=args(3);
                auto message=static_cast<unsigned>(args(1));
                const unsigned unit=x86_?4U:8U;
                const auto ptr_bits=static_cast<std::uint8_t>(unit*8);
                const auto finish=[&](std::uint64_t result){args.return_to(s,result);return std::optional<std::uint32_t>{};};
                const auto api=[&](std::string_view family,std::string_view symbol,std::vector<std::uint64_t> values)
                    ->Task<std::uint64_t>{co_return co_await service_callback::call_value(resolve(family,symbol).value,std::move(values));};
                const auto def=[&]()->Task<std::uint64_t>{
                    co_return co_await api("user32.dll","DefWindowProcW",{hwnd,message,wp,lp});
                };
                auto& bridge=*avi_disk_;
                const auto window=gui_.windows.find(hwnd);
                if(window==gui_.windows.end())co_return finish(0);
                if(message==1){
                    auto item=std::make_shared<MciWindow>();
                    item->owner=window->second.parent?window->second.parent:window->second.owner;
                    item->styles=window->second.style&0xffffU;
                    bridge.mci_windows.emplace(hwnd,std::move(item));
                }
                const auto found=bridge.mci_windows.find(hwnd);
                if(found==bridge.mci_windows.end())co_return finish(co_await def());
                const auto self=found->second;
                auto& player=*self;
                GuestScratch scratch(*this,m,s);
                const auto block=scratch.allocate(144);
                if(!block)co_return finish(message==1?UINT64_MAX:mci::memory);
                const auto field=[&](unsigned at,unsigned width,std::uint64_t value){
                    m.store(GuestAddress{block+at},static_cast<std::uint8_t>(width),value);
                };
                const auto query=[&](unsigned at,unsigned width=32){
                    return m.load(GuestAddress{block+at},static_cast<std::uint8_t>(width));
                };
                const auto clean=[&]{m.fill(GuestAddress{block},0,144);};
                const auto send=[&](unsigned command,unsigned flags=0)->Task<std::uint32_t>{
                    co_return static_cast<unsigned>(co_await api("winmm.dll","mciSendCommandW",{player.device,command,flags,block}));
                };
                const auto numeric=[](std::uint64_t value){return utf8_to_utf16(std::to_string(value));};
                const auto avi_device=[&]{
                    const auto item=mci_state().videos.find(player.device);
                    return item==mci_state().videos.end()?std::shared_ptr<mci::AviDevice>{}:item->second;
                };
                const auto text=[&](std::uint64_t pointer,bool wide){
                    if(!pointer)return std::u16string{};
                    return wide?read_utf16(m,GuestAddress{pointer},32767):ansi_to_utf16_lossy(read_ansi(m,pointer,32767));
                };
                const auto copy=[&](std::u16string_view data,bool wide){
                    const auto maximum=std::min<std::uint64_t>(wp,32768);
                    if(!lp || !maximum)return;
                    if(wide)m.write(GuestAddress{lp},utf16_bytes(data.substr(0,maximum-1)));
                    else{
                        auto value=utf16_to_utf8(data);
                        value.resize(std::min<std::size_t>(value.size(),maximum-1));
                        value.push_back(0);
                        m.write(GuestAddress{lp},std::span(reinterpret_cast<const std::uint8_t*>(value.data()),value.size()));
                    }
                };
                const auto notify=[&](unsigned flag,unsigned signal,std::uint64_t data)->Task<bool>{
                    if((player.styles&flag) && player.owner){
                        (void)co_await api("user32.dll","SendMessageW",{player.owner,signal,hwnd,data});
                    }
                    co_return true;
                };
                const auto error=[&](unsigned status)->Task<std::uint64_t>{
                    player.error=status;
                    if(status)(void)co_await notify(0x1000,1229,status);
                    co_return status;
                };
                const auto status=[&](unsigned item,unsigned flags=0)->Task<std::uint64_t>{
                    if(!player.device)co_return item==4?524U:0U;
                    clean();
                    field(2*unit,32,item);
                    if(const auto failure=co_await send(mci::status,0x100U|flags)){
                        player.error=failure;
                        co_return 0;
                    }
                    co_return query(unit,ptr_bits);
                };
                const auto cap=[&](unsigned item)->Task<std::uint64_t>{
                    if(!player.device)co_return 0;
                    clean();
                    field(unit+4,32,item);
                    if(co_await send(mci::getcaps,0x100))co_return 0;
                    co_return query(unit);
                };
                const auto get_mode_text=[](unsigned mode)->std::u16string_view {
                    constexpr std::u16string_view modes[]{u"not ready",u"stopped",u"playing",u"recording",u"seeking",u"paused",u"open"};
                    return mode>=524 && mode<=530?modes[mode-524]:u"not ready";
                };
                const auto timer=[&]()->Task<bool>{
                    (void)co_await api("user32.dll","KillTimer",{hwnd,clip_timer});
                    if(player.device){
                        const auto current=gui_.windows.find(hwnd);
                        if(current==gui_.windows.end())co_return false;
                        const auto shown=player.active && current->second.visible;
                        auto millis=shown?player.active_timer:player.inactive_timer;
                        if(const auto video=avi_device();video && shown)
                            millis=std::min(millis,static_cast<unsigned>(std::max<std::uint64_t>(10,video->clip->interval()/10000)));
                        co_return 0!=co_await api("user32.dll","SetTimer",{hwnd,clip_timer,millis,0});
                    }
                    co_return true;
                };
                const auto paint=[&](std::uint64_t dc=0)->Task<bool>{
                    if(!avi_device())co_return false;
                    clean();
                    if(dc)field(unit+16,ptr_bits,dc);
                    co_return 0==co_await send(mci::video_update,dc?0x20000U:0U);
                };
                const auto visibility=[&](unsigned id,bool visible)->Task<bool>{
                    if(const auto child=gui_child(hwnd,id))(void)co_await api("user32.dll","ShowWindow",{child,visible?5U:0U});
                    co_return true;
                };
#include "windows_vfw_mciwnd_player.inc"
            });
    }
