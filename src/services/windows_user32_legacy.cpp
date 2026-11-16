#include "x64arm/windows_services.hpp"
#include "x64arm/windows_userdraw.hpp"
#include "service_callback_task.hpp"
#include "x64arm/unicode.hpp"

namespace x64arm {
// user32 舊版介面家族：BroadcastSystemMessage(Ex)A/W、SetSysColors、DlgDirList(ComboBox)A/W 與 DlgDirSelect(ComboBox)ExA/W。
// 廣播範圍與 HWND_BROADCAST 相同（本程序的頂層視窗）；跨程序視窗尚無共用 broker。
std::span<const WindowsServiceDescriptor> user32_legacy_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"user32.dll","BroadcastSystemMessage",5,0,0,5},{"user32.dll","BroadcastSystemMessageA",5,0,0,5},{"user32.dll","BroadcastSystemMessageW",5,0,0,5},
        {"user32.dll","BroadcastSystemMessageExA",6,0,0,5},{"user32.dll","BroadcastSystemMessageExW",6,0,0,5},
        {"user32.dll","SetSysColors",3,0,0,1},
        {"user32.dll","DlgDirListA",5,0,0,0x1c},{"user32.dll","DlgDirListW",5,0,0,0x1c},{"user32.dll","DlgDirListComboBoxA",5,0,0,0x1c},{"user32.dll","DlgDirListComboBoxW",5,0,0,0x1c},
        {"user32.dll","DlgDirSelectExA",4,0,0,0xc},{"user32.dll","DlgDirSelectExW",4,0,0,0xc},{"user32.dll","DlgDirSelectComboBoxExA",4,0,0,0xc},{"user32.dll","DlgDirSelectComboBoxExW",4,0,0,0xc},
    };
    return table;
}
std::vector<std::uint64_t> WindowsServices::gui_broadcast_targets() const {
    std::vector<std::uint64_t> targets;
    for(const auto& [handle,window]:gui_.windows)if(!window.parent || window.parent==gui_.desktop)targets.push_back(handle);
    return targets;
}
std::optional<std::uint32_t> WindowsServices::invoke_user32_legacy(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto symbol=service.symbol;
    const auto d=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    if(symbol.starts_with("DlgDir")){
        // 清單方塊（LB_*）與下拉方塊（CB_*）兩組訊息；控制項經客體 SendMessage，保留子類別化的視窗程序。
        const bool combo=symbol.find("ComboBox")!=symbol.npos,wide=symbol.ends_with('W'),select=symbol.starts_with("DlgDirSelect");
        std::u16string spec;
        if(!select && arg(1))spec=wide?read_utf16(memory,GuestAddress{arg(1)},260):utf8_to_utf16_lossy(read_ansi(memory,arg(1),260));
        return nested_continuation(state,memory,"DlgDir",[this,service,combo,wide,select,spec](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);
            const auto entry=[&](std::string_view dll,std::string_view target){const auto stub=host_service_stub(dll,target);return stub.value?stub.value:resolve(dll,target).value;};
            const auto handle=[&](std::uint64_t value){return x86_?value&UINT32_MAX:value;};
            const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            const auto send=[&](std::uint64_t control,std::uint32_t message,std::uint64_t wparam,std::uint64_t lparam){return service_callback::call_value(entry("user32.dll","SendMessageW"),{control,message,wparam,lparam});};
            GuestScratch scratch(*this,m,s);
            if(select){
                // 目前選取：「[-x-]」轉成「x:」、「[目錄]」轉成「目錄\」並回傳 TRUE；一般檔名回傳 FALSE；沒有選取時不改動緩衝區。
                const auto control=handle(co_await service_callback::call_value(entry("user32.dll","GetDlgItem"),{call(0),call(3)}));if(!control)co_return finish(0);
                const auto index=static_cast<std::int32_t>(static_cast<std::uint32_t>(co_await send(control,combo?0x147U:0x188U,0,0)));if(index<0)co_return finish(0);
                const auto length=static_cast<std::int32_t>(static_cast<std::uint32_t>(co_await send(control,combo?0x149U:0x18aU,static_cast<std::uint64_t>(index),0)));if(length<0)co_return finish(0);
                const auto buffer=scratch.allocate(static_cast<std::size_t>(length+1)*2);if(!buffer)co_return finish(0);
                (void)co_await send(control,combo?0x148U:0x189U,static_cast<std::uint64_t>(index),buffer);
                auto text=read_utf16(m,GuestAddress{buffer},static_cast<std::size_t>(length)+1);bool directory=false;
                if(text.size()==5 && text.starts_with(u"[-") && text.ends_with(u"-]")){text=std::u16string(1,text[2])+u":";directory=true;}
                else if(text.size()>=2 && text.front()==u'[' && text.back()==u']'){text=text.substr(1,text.size()-2)+u"\\";directory=true;}
                const auto capacity=static_cast<std::int32_t>(static_cast<std::uint32_t>(call(2)));
                if(call(1) && capacity>0){
                    std::vector<std::uint8_t> bytes;
                    if(wide){const auto units=std::min<std::size_t>(text.size(),static_cast<std::size_t>(capacity-1));bytes=utf16_bytes(std::u16string_view(text).substr(0,units));}
                    else {auto ansi=utf16_to_utf8_lossy(text);if(ansi.size()>static_cast<std::size_t>(capacity-1))ansi.resize(static_cast<std::size_t>(capacity-1));bytes.assign(ansi.begin(),ansi.end());bytes.push_back(0);}
                    m.write(GuestAddress{call(1)},bytes);
                }
                co_return finish(directory?1:0);
            }
            // DlgDirList：路徑含目錄時先切換目前目錄、只留檔名樣式（寫回 lpPathSpec），再清空並以 LB_DIR／CB_DIR 填入，靜態控制項顯示目前目錄。
            auto pattern=spec.empty()?std::u16string(u"*.*"):spec;std::u16string directory;
            const auto separator=pattern.find_last_of(u"\\/:");
            if(separator!=pattern.npos){directory=pattern.substr(0,separator+1);pattern=pattern.substr(separator+1);if(pattern.empty())pattern=u"*.*";}
            if(pattern.find_first_of(u"*?")==pattern.npos){
                // 不含萬用字元且本身是目錄：切換進去並列出全部。
                const auto candidate=scratch.text(directory+pattern);
                const auto attributes=static_cast<std::uint32_t>(co_await service_callback::call(entry("kernel32.dll","GetFileAttributesW"),{candidate}));
                if(attributes!=UINT32_MAX && (attributes&0x10U)){directory+=pattern;pattern=u"*.*";}
            }
            if(!directory.empty()){
                if(directory.size()>1 && (directory.back()==u'\\' || directory.back()==u'/') && directory[directory.size()-2]!=u':')directory.pop_back();
                if(!co_await service_callback::call(entry("kernel32.dll","SetCurrentDirectoryW"),{scratch.text(directory)}))co_return finish(0);
            }
            if(call(1)){
                if(wide)m.write(GuestAddress{call(1)},utf16_bytes(pattern));
                else {const auto ansi=utf16_to_utf8_lossy(pattern);std::vector<std::uint8_t> bytes(ansi.begin(),ansi.end());bytes.push_back(0);m.write(GuestAddress{call(1)},bytes);}
            }
            const auto list_id=static_cast<std::uint32_t>(call(2)),static_id=static_cast<std::uint32_t>(call(3)),types=static_cast<std::uint32_t>(call(4));
            if(list_id){
                const auto control=handle(co_await service_callback::call_value(entry("user32.dll","GetDlgItem"),{call(0),list_id}));if(!control)co_return finish(0);
                (void)co_await send(control,combo?0x14bU:0x184U,0,0);
                (void)co_await send(control,combo?0x145U:0x18dU,types&~0x2000U,scratch.text(pattern));
            }
            if(static_id){
                const auto buffer=scratch.allocate(1040);if(!buffer)co_return finish(0);
                const auto length=static_cast<std::uint32_t>(co_await service_callback::call(entry("kernel32.dll","GetCurrentDirectoryW"),{520,buffer}));
                if(!length || length>=520)co_return finish(0);
                (void)co_await service_callback::call(entry("user32.dll","SetDlgItemTextW"),{call(0),static_id,buffer});
            }
            co_return finish(1);
        });
    }
    if(symbol=="SetSysColors"){
        // 依序改寫系統色彩（索引超出 COLOR_* 範圍者略過），更新已發出的系統色筆刷，再通知頂層視窗並重繪。
        const auto count=static_cast<std::int32_t>(d(0));
        if(count<0 || (count && (!arg(1) || !arg(2)))){last_error_=87;arg.return_to(state,0);return std::nullopt;}
        try{
            memory.check(GuestAddress{arg(1)},static_cast<std::size_t>(count)*4,Access::Read);memory.check(GuestAddress{arg(2)},static_cast<std::size_t>(count)*4,Access::Read);
            for(std::int32_t i=0;i<count;++i){
                const auto index=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)+4ULL*static_cast<std::uint64_t>(i)},32));
                const auto color=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(2)+4ULL*static_cast<std::uint64_t>(i)},32));
                if(gui_set_system_color(index,color))if(const auto brush=gui_.brushes.find(0xa000+index);brush!=gui_.brushes.end())brush->second=gui_system_color(index);
            }
        }catch(const MemoryFault&){last_error_=998;arg.return_to(state,0);return std::nullopt;}
        if(count){
            for(const auto handle:gui_broadcast_targets()){GuiMessage message{handle,0,0,0x15};enqueue_gui_message(message);}   // WM_SYSCOLORCHANGE
            for(const auto& [handle,window]:gui_.windows){(void)window;invalidate_window(handle);}
        }
        arg.return_to(state,1);return std::nullopt;
    }
    // BroadcastSystemMessage(Ex)(flags, lpInfo, msg, wParam, lParam[, PBSMINFO])。
    const bool extended=symbol.starts_with("BroadcastSystemMessageEx"),wide=symbol.ends_with('W');   // 無字尾名稱即 A 版本
    const auto flags=d(0),message=d(2);const auto info=arg(1),bsm=extended?arg(5):0;
    const auto failed=[&](std::uint32_t error){last_error_=error;arg.return_to(state,UINT32_MAX);return std::optional<std::uint32_t>{};};
    // BSF_QUERY 1、IGNORECURRENTTASK 2、FLUSHDISK 4、NOHANG 8、POSTMESSAGE 0x10、FORCEIFHUNG 0x20、NOTIMEOUTIFNOTHUNG 0x40、
    // ALLOWSFW 0x80、SENDNOTIFYMESSAGE 0x100、RETURNHDESK 0x200、LUID 0x400；後兩者只有 Ex 版本可用。
    if((flags&~(extended?0x7ffU:0x1ffU)) || ((flags&0x10U) && (flags&0x101U)) || (message>=0x400 && message<0xc000))return failed(87);
    const auto bsm_size=x86_?20U:32U;
    if(extended && (flags&0x600U) && (!bsm || memory.load(GuestAddress{bsm},32)!=bsm_size))return failed(87);
    std::uint32_t recipients=0;
    if(info){recipients=static_cast<std::uint32_t>(memory.load(GuestAddress{info},32));if(recipients&~0x1fU)return failed(87);}
    // 只有應用程式（頂層視窗）是本引擎的廣播對象；回報實際收到訊息的類別。
    const bool applications=!info || !recipients || (recipients&0x18U);
    if(info)memory.store(GuestAddress{info},32,applications?(recipients&0x10U)|0x8U:0U);
    std::vector<std::uint64_t> targets;if(applications && !(flags&2U))targets=gui_broadcast_targets();
    if(flags&0x110U){
        // POSTMESSAGE／SENDNOTIFYMESSAGE：不等待回覆，放入各視窗的訊息佇列。
        if(gui_.queue.size()+targets.size()>10000)return failed(1816);
        for(const auto handle:targets){GuiMessage posted{handle,arg(3),arg(4),message};enqueue_gui_message(posted);}
        arg.return_to(state,1);return std::nullopt;
    }
    return nested_continuation(state,memory,"BroadcastSystemMessage",[this,service,targets,flags,message,wide,bsm](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);
        const auto entry=[&](std::string_view target){const auto stub=host_service_stub("user32.dll",target);return stub.value?stub.value:resolve("user32.dll",target).value;};
        const auto send=entry(wide?"SendMessageW":"SendMessageA");
        for(const auto handle:targets){
            if(!gui_.windows.contains(handle))continue;
            const auto reply=co_await service_callback::call_value(send,{handle,message,call(3),call(4)});
            // BSF_QUERY：任一接收者回 BROADCAST_QUERY_DENY 即停止；RETURNHDESK 回報拒絕者的視窗與桌面。
            if((flags&1U) && static_cast<std::uint32_t>(reply)==0x424d5144U){
                if((flags&0x200U) && bsm){
                    const auto desktop=co_await service_callback::call_value(entry("OpenInputDesktop"),{0,0,0x2000000});
                    const auto unit=x86_?4U:8U;m.store(GuestAddress{bsm+unit},x86_?32:64,desktop);m.store(GuestAddress{bsm+2*unit},x86_?32:64,handle);
                }
                call.return_to(s,std::uint64_t{0});co_return std::nullopt;
            }
        }
        call.return_to(s,std::uint64_t{1});co_return std::nullopt;
    });
}
}
