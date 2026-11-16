// wtsapi32：本機 RD Session Host 模型，與 WTSQuerySessionInformation（windows_environment.cpp）一致：
// session 0「Services」為中斷（WTSDisconnected）、session 1「Console」為作用中，使用者 user、網域與電腦名稱 X64ARM；pid 0／4 屬 session 0（LocalSystem），其餘程序屬 session 1（主權杖使用者）。
// - WTSOpenServerA／W、WTSOpenServerExA／W：只接受本機名稱（電腦名稱、localhost、"."、空字串），其他為 RPC_S_SERVER_UNAVAILABLE；WTSCloseServer 釋放。
//   NULL（WTS_CURRENT_SERVER_HANDLE）或開啟的 handle 才有效，否則 ERROR_INVALID_HANDLE。
// - WTSEnumerateServersA／W：工作群組內沒有 RD Session Host 伺服器，回傳 0 筆。
// - WTSEnumerateSessionsA／W（WTS_SESSION_INFO）、WTSEnumerateSessionsExA／W（level 1：WTS_SESSION_INFO_1）。
// - WTSEnumerateProcessesA／W（WTS_PROCESS_INFO）、WTSEnumerateProcessesExA／W（level 0 或 1：WTS_PROCESS_INFO_EX，執行緒、handle、時間與本程序的記憶體計數，
//   與 NtQuerySystemInformation 程序清單同一來源；可依 session 篩選）。結果與字串、SID 放在同一個 WTSFreeMemory 區塊。
// - WTSFreeMemoryExA／W：依 WTS_TYPE_CLASS 檢查區塊型別後釋放。
// - WTSRegister／UnRegisterSessionNotificationEx：檢查伺服器 handle 後沿用非 Ex 版本的視窗訂閱。
// - WTSWaitSystemEvent：沙盒內沒有 session 狀態轉換，等待到 WTS_EVENT_FLUSH 為止（以每個伺服器 handle 的事件 PulseEvent 喚醒目前等待者），回傳 WTS_EVENT_NONE。
// - WTSSendMessageA／W：在 session 1 顯示訊息方塊（標題與內容長度以位元組計）；bWait 時以 MessageBoxTimeoutW 等候回應（逾時 IDTIMEOUT），
//   否則以 MessageBoxIndirectW 為執行緒入口非同步顯示並回 IDASYNC，結束的執行緒在後續呼叫時回收。
// - WTSTerminateProcess：以 OpenProcess（PROCESS_TERMINATE）＋TerminateProcess 結束指定程序。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <array>

namespace x64arm {
namespace {
constexpr std::uint32_t wts_active=0,wts_disconnected=4,current_session=UINT32_MAX,any_session=0xfffffffeU;
struct Session {std::uint32_t id,state;std::u16string_view name;bool user;};
constexpr std::array<Session,2> sessions{{{0,wts_disconnected,u"Services",false},{1,wts_active,u"Console",true}}};
const std::vector<std::uint8_t> local_system{1,1,0,0,0,0,0,5,18,0,0,0}; // S-1-5-18
bool local_server(std::u16string name){
    for(auto& c:name)if(c>=u'a' && c<=u'z')c=static_cast<char16_t>(c-32);
    if(name.starts_with(u"\\\\"))name.erase(0,2);
    return name.empty() || name==u"." || name==u"LOCALHOST" || name==u"X64ARM" || name==u"127.0.0.1";
}
// 一個 WTSFreeMemory 區塊：固定大小的項目陣列在前，字串與 SID 依序附在後面。
struct Block {
    std::vector<std::uint8_t> bytes;std::vector<std::pair<std::size_t,std::size_t>> pointers; // （欄位位置, 區塊內位移）
    void put(std::size_t at,std::uint64_t value,unsigned size){if(bytes.size()<at+size)bytes.resize(at+size);for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));}
    void append(std::size_t field,std::span<const std::uint8_t> data,std::size_t align){while(bytes.size()%align)bytes.push_back(0);pointers.push_back({field,bytes.size()});bytes.insert(bytes.end(),data.begin(),data.end());}
};
}
std::span<const WindowsServiceDescriptor> wts_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"wtsapi32.dll","WTSOpenServerA",1},{"wtsapi32.dll","WTSOpenServerW",1},{"wtsapi32.dll","WTSOpenServerExA",1},{"wtsapi32.dll","WTSOpenServerExW",1},{"wtsapi32.dll","WTSCloseServer",1},
        {"wtsapi32.dll","WTSEnumerateServersA",5,0,0,0x6},{"wtsapi32.dll","WTSEnumerateServersW",5,0,0,0x6},
        {"wtsapi32.dll","WTSEnumerateSessionsA",5,0,0,0x6},{"wtsapi32.dll","WTSEnumerateSessionsW",5,0,0,0x6},
        {"wtsapi32.dll","WTSEnumerateSessionsExA",5,0,0,0x4},{"wtsapi32.dll","WTSEnumerateSessionsExW",5,0,0,0x4},
        {"wtsapi32.dll","WTSEnumerateProcessesA",5,0,0,0x6},{"wtsapi32.dll","WTSEnumerateProcessesW",5,0,0,0x6},
        {"wtsapi32.dll","WTSEnumerateProcessesExA",5,0,0,0x4},{"wtsapi32.dll","WTSEnumerateProcessesExW",5,0,0,0x4},
        {"wtsapi32.dll","WTSFreeMemoryExA",3,0,0,0x5},{"wtsapi32.dll","WTSFreeMemoryExW",3,0,0,0x5},
        {"wtsapi32.dll","WTSRegisterSessionNotificationEx",3,0,0,0x4},{"wtsapi32.dll","WTSUnRegisterSessionNotificationEx",2},
        {"wtsapi32.dll","WTSWaitSystemEvent",3,0,0,0x2},
        {"wtsapi32.dll","WTSSendMessageA",10,0,0,0x2ea},{"wtsapi32.dll","WTSSendMessageW",10,0,0,0x2ea},
        {"wtsapi32.dll","WTSTerminateProcess",3,0,0,0x6}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_wts(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=!name.ends_with('A') || name=="WTSWaitSystemEvent";
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(0);};
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const unsigned unit=x86_?4:8;const std::uint8_t bits=x86_?32:64;
    const auto valid_server=[&](std::uint64_t handle){return !handle || runtime_.wts_servers.contains(handle);};
    const auto text=[&](std::u16string_view value){std::vector<std::uint8_t> out;if(wide)out=utf16_bytes(value);else{const auto s=utf16_to_utf8(std::u16string(value));out.assign(s.begin(),s.end());out.push_back(0);}return out;};
    // 區塊寫入客體堆積並補上絕對指標；登錄於 WTSFreeMemory／WTSFreeMemoryEx。
    const auto publish=[&](Block& block,std::uint32_t type_class,std::uint64_t out,std::uint64_t count_out,std::uint32_t count)->bool{
        memory.check(GuestAddress{out},unit,Access::Write);memory.check(GuestAddress{count_out},4,Access::Write);
        const auto address=allocate_heap(memory,std::max<std::size_t>(block.bytes.size(),1));if(!address){last_error_=8;return false;}
        for(const auto& [field,offset]:block.pointers)block.put(field,address+offset,unit);
        if(!block.bytes.empty())memory.write(GuestAddress{address},block.bytes);
        runtime_.wts_allocations.insert(address);runtime_.wts_allocation_classes[address]=type_class;
        memory.store(GuestAddress{out},bits,address);memory.store(GuestAddress{count_out},32,count);return true;
    };
    try{
        if(name.starts_with("WTSOpenServer")){
            const auto server=arg(0)?(wide?read_utf16(memory,GuestAddress{arg(0)},256):utf8_to_utf16_lossy(read_ansi(memory,arg(0),256))):std::u16string{};
            if(!local_server(server))return fail(1722);
            if(runtime_.wts_servers.size()>=1024)return fail(8);
            const auto handle=runtime_.wts_next_server;runtime_.wts_next_server+=4;runtime_.wts_servers[handle]=0;return finish(handle);
        }
        if(name=="WTSCloseServer"){
            if(const auto found=runtime_.wts_servers.find(arg(0));arg(0) && found!=runtime_.wts_servers.end()){if(found->second)(void)nested_service(state,memory,"kernel32.dll","CloseHandle",{found->second});runtime_.wts_servers.erase(found);}
            return finish(0);
        }
        if(name.starts_with("WTSEnumerateServers")){
            if(d(1) || d(2)!=1 || !arg(3) || !arg(4))return fail(87);
            Block block;return finish(publish(block,UINT32_MAX,arg(3),arg(4),0)?1:0);
        }
        if(name.starts_with("WTSEnumerateSessions")){
            const bool ex=name.starts_with("WTSEnumerateSessionsEx");
            if(!valid_server(arg(0)))return fail(6);
            if(ex){if(!arg(1) || static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32))!=1 || d(2) || !arg(3) || !arg(4))return fail(87);}
            else if(d(1) || d(2)!=1 || !arg(3) || !arg(4))return fail(87);
            const std::size_t size=ex?(x86_?32:56):(x86_?12:24);Block block;block.bytes.resize(size*sessions.size());
            for(std::size_t i=0;i<sessions.size();++i){
                const auto& s=sessions[i];const auto at=i*size;
                if(ex){
                    block.put(at,s.id,4);block.put(at+4,s.state,4);block.put(at+8,s.id,4);const auto ptr=at+(x86_?12:16);
                    block.append(ptr,text(s.name),2);
                    if(s.user){block.append(ptr+unit,text(u"X64ARM"),2);block.append(ptr+2*unit,text(u"user"),2);block.append(ptr+3*unit,text(u"X64ARM"),2);}
                }else{block.put(at,s.id,4);block.append(at+(x86_?4:8),text(s.name),2);block.put(at+(x86_?8:16),s.state,4);}
            }
            return finish(publish(block,ex?2:UINT32_MAX,arg(3),arg(4),static_cast<std::uint32_t>(sessions.size()))?1:0);
        }
        if(name.starts_with("WTSEnumerateProcesses")){
            const bool ex=name.starts_with("WTSEnumerateProcessesEx");std::uint32_t level=0,filter=any_session;
            if(!valid_server(arg(0)))return fail(6);
            if(ex){if(!arg(1) || !arg(3) || !arg(4))return fail(87);level=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(1)},32));filter=d(2);if(level>1)return fail(87);if(filter==current_session)filter=1;}
            else if(d(1) || d(2)!=1 || !arg(3) || !arg(4))return fail(87);
            std::vector<ProcessSnapshot::Process> processes;if(const auto error=capture_process_snapshot(memory,processes))return fail(error);
            if(std::none_of(processes.begin(),processes.end(),[](const auto& p){return p.id==0;})){processes.insert(processes.begin(),{{0,0,1,0,u"System Idle Process"},{4,0,1,8,u"System"}});}
            std::erase_if(processes,[&](const auto& p){const std::uint32_t session=p.id<=4?0:1;return filter!=any_session && filter!=session;});
            initialize_tokens();const auto& user_sid=runtime_.token_objects.at(1).user.sid;
            const std::size_t size=level==1?(x86_?56:64):(x86_?16:24);Block block;block.bytes.resize(size*processes.size());
            for(std::size_t i=0;i<processes.size();++i){
                const auto& p=processes[i];const auto at=i*size;const std::uint32_t session=p.id<=4?0:1;
                auto image=p.executable;if(const auto slash=image.find_last_of(u"\\/");slash!=image.npos)image=image.substr(slash+1);
                block.put(at,session,4);block.put(at+4,p.id,4);block.append(at+8,text(image),2);block.append(at+8+unit,session?std::span<const std::uint8_t>(user_sid):std::span<const std::uint8_t>(local_system),4);
                if(level==1){
                    std::array<std::uint64_t,5> accounting{};std::uint32_t handles=0,threads=p.threads;const bool local=p.id==process_id();
                    if(p.id>4){if(local?local_process_accounting(accounting)!=0:remote_process_accounting(memory,p.id,accounting)!=0)accounting={};std::uint32_t counted=0;if(query_process_counters(memory,p.id,handles,counted)==0 && counted)threads=counted;}
                    const auto fields=at+8+2*unit;block.put(fields,threads,4);block.put(fields+4,handles,4);
                    if(local){const auto usage=memory.usage();const auto clamp=[](std::uint64_t v){return std::min<std::uint64_t>(v,UINT32_MAX);};
                        block.put(fields+8,clamp(usage.private_commit),4);block.put(fields+12,clamp(usage.peak_private_commit),4);block.put(fields+16,clamp(usage.committed),4);block.put(fields+20,clamp(usage.peak_committed),4);}
                    block.put(fields+24,accounting[3],8);block.put(fields+32,accounting[2],8);
                }
            }
            return finish(publish(block,level==1?1U:0U,arg(3),arg(4),static_cast<std::uint32_t>(processes.size()))?1:0);
        }
        if(name.starts_with("WTSFreeMemoryEx")){
            const auto type=d(0);if(type>2 || !arg(1))return fail(87);
            const auto found=runtime_.wts_allocation_classes.find(arg(1));
            if(!runtime_.wts_allocations.contains(arg(1)) || found==runtime_.wts_allocation_classes.end() || found->second!=type)return fail(87);
            if(!free_heap(memory,arg(1),state))return fail(87);runtime_.wts_allocations.erase(arg(1));runtime_.wts_allocation_classes.erase(found);return finish(1);
        }
        if(name=="WTSRegisterSessionNotificationEx" || name=="WTSUnRegisterSessionNotificationEx"){
            if(!valid_server(arg(0)))return fail(6);
            const bool registering=name=="WTSRegisterSessionNotificationEx";
            last_error_=0;const auto result=registering?nested_service(state,memory,"wtsapi32.dll","WTSRegisterSessionNotification",{arg(1),d(2)}):nested_service(state,memory,"wtsapi32.dll","WTSUnRegisterSessionNotification",{arg(1)});
            return finish(result);
        }
        if(name=="WTSWaitSystemEvent"){
            if(!valid_server(arg(0)))return fail(6);if(!arg(2))return fail(87);memory.check(GuestAddress{arg(2)},4,Access::Write);
            auto& event=runtime_.wts_servers[arg(0)];
            if(!event){event=nested_service(state,memory,"kernel32.dll","CreateEventW",{0,1,0,0});if(!event)return fail(8);}
            if(d(1)&0x80000000U){(void)nested_service(state,memory,"kernel32.dll","PulseEvent",{event});memory.store(GuestAddress{arg(2)},32,0);return finish(1);}
            if(!d(1))return fail(87);
            const auto wait=resolve("kernel32.dll","WaitForSingleObject").value;const auto handle=event;const auto flags=arg(2);
            return nested_continuation(state,memory,"WTSWaitSystemEvent",[service,wait,handle,flags](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const auto descriptor=service;const auto result=co_await service_callback::call_value(wait,{handle,UINT32_MAX});
                const WindowsCall call(s,m,descriptor);if(static_cast<std::uint32_t>(result)!=0){call.return_to(s,0);co_return std::optional<std::uint32_t>{};}
                m.store(GuestAddress{flags},32,0);call.return_to(s,1);co_return std::optional<std::uint32_t>{};});
        }
        if(name.starts_with("WTSSendMessage")){
            if(!valid_server(arg(0)))return fail(6);
            const auto session=d(1);if(session!=1 && session!=current_session)return fail(session==0?5:7022);
            if(!arg(8))return fail(87);memory.check(GuestAddress{arg(8)},4,Access::Write);
            const auto read=[&](std::uint64_t at,std::uint32_t bytes)->std::u16string{
                if(!at || !bytes)return {};if(bytes>64*1024)throw std::invalid_argument("WTS 訊息過長");
                std::vector<std::uint8_t> raw(bytes);memory.read(GuestAddress{at},raw);
                if(wide){std::u16string out;for(std::size_t i=0;i+1<raw.size();i+=2){const auto c=static_cast<char16_t>(raw[i]|raw[i+1]<<8);if(!c)break;out.push_back(c);}return out;}
                std::string s(raw.begin(),raw.end());if(const auto nul=s.find('\0');nul!=s.npos)s.resize(nul);return utf8_to_utf16_lossy(s);
            };
            const auto title=read(arg(2),d(3)),message=read(arg(4),d(5));const auto style=d(6),timeout=d(7);const bool wait=d(9)!=0;
            // 回收已結束的非同步訊息執行緒與其參數區塊。
            if(!runtime_.wts_async_messages.empty()){
                const auto code=allocate_heap(memory,4);if(!code)return fail(8);
                std::erase_if(runtime_.wts_async_messages,[&](const auto& entry){
                // GetExitCodeThread 不需排程即可判斷；STILL_ACTIVE（259）表示訊息方塊仍在顯示。
                if(!nested_service(state,memory,"kernel32.dll","GetExitCodeThread",{entry.first,code}) || memory.load(GuestAddress{code},32)==259)return false;
                (void)nested_service(state,memory,"kernel32.dll","CloseHandle",{entry.first});(void)free_heap(memory,entry.second,state);return true;});
                (void)free_heap(memory,code,state);
            }
            const auto title_bytes=utf16_bytes(title),message_bytes=utf16_bytes(message);const std::size_t params=x86_?40:80;
            const auto block=allocate_heap(memory,params+message_bytes.size()+title_bytes.size());if(!block)return fail(8);
            const auto text_at=block+params,caption_at=text_at+message_bytes.size();
            memory.write(GuestAddress{text_at},message_bytes);memory.write(GuestAddress{caption_at},title_bytes);
            if(!wait){
                if(runtime_.wts_async_messages.size()>=64){(void)free_heap(memory,block,state);return fail(8);}
                // MSGBOXPARAMSW：cbSize、hwndOwner、hInstance、lpszText、lpszCaption、dwStyle、lpszIcon、dwContextHelpId、lpfnMsgBoxCallback、dwLanguageId。
                std::vector<std::uint8_t> bytes(params);const auto put=[&](std::size_t at,std::uint64_t value,unsigned size){for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));};
                put(0,params,4);put(3*unit,text_at,unit);put(4*unit,caption_at,unit);put(5*unit,style,4);memory.write(GuestAddress{block},bytes);
                // MessageBoxIndirectW 為執行緒入口（單一參數、回傳按鈕值為結束碼）。
                const auto thread=nested_service(state,memory,"kernel32.dll","CreateThread",{0,0,resolve("user32.dll","MessageBoxIndirectW").value,block,0,0});
                if(!thread){(void)free_heap(memory,block,state);return finish(0);}
                runtime_.wts_async_messages.emplace_back(thread,block);memory.store(GuestAddress{arg(8)},32,32001);return finish(1); // IDASYNC
            }
            const auto box=resolve("user32.dll","MessageBoxTimeoutW").value;const auto response=arg(8);
            return nested_continuation(state,memory,"WTSSendMessage",[this,service,box,text_at,caption_at,style,timeout,response,block](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const auto descriptor=service;
                const auto result=co_await service_callback::call_value(box,{0,text_at,caption_at,style,0,timeout?static_cast<std::uint64_t>(timeout)*1000:UINT32_MAX});
                (void)free_heap(m,block,s);m.store(GuestAddress{response},32,static_cast<std::uint32_t>(result)); // MessageBoxTimeout 逾時回 32000＝IDTIMEOUT
                const WindowsCall call(s,m,descriptor);call.return_to(s,static_cast<std::uint32_t>(result)?1:0);co_return std::optional<std::uint32_t>{};});
        }
        if(name=="WTSTerminateProcess"){
            if(!valid_server(arg(0)))return fail(6);
            const auto open=resolve("kernel32.dll","OpenProcess").value,terminate=resolve("kernel32.dll","TerminateProcess").value,close=resolve("kernel32.dll","CloseHandle").value;
            const auto pid=d(1),code=d(2);
            return nested_continuation(state,memory,"WTSTerminateProcess",[service,open,terminate,close,pid,code](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const auto descriptor=service;const auto handle=co_await service_callback::call_value(open,{1,0,pid}); // PROCESS_TERMINATE
                std::uint64_t result=0;
                if(handle){result=co_await service_callback::call_value(terminate,{handle,code});(void)co_await service_callback::call_value(close,{handle});}
                const WindowsCall call(s,m,descriptor);call.return_to(s,static_cast<std::uint32_t>(result)?1:0);co_return std::optional<std::uint32_t>{};});
        }
    }catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(8);}catch(const std::invalid_argument&){return fail(87);}
    return fail(120);
}
}
