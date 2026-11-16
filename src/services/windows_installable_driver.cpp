// winmm 可安裝驅動程式（Installable Drivers）：
// - OpenDriver／OpenDriverA（DrvOpen／DrvOpenA）：名稱先查 HKLM\Software\Microsoft\Windows NT\CurrentVersion\<區段>（預設 Drivers32）的同名值，
//   值的第一段為檔名、其餘為設定字串；查無時名稱即檔名。以 LoadLibraryW 載入並取 DriverProc；同模組第一個執行個體先送 DRV_LOAD、DRV_ENABLE，
//   每個執行個體送 DRV_OPEN（lParam1 為設定字串、lParam2 為呼叫者的值），回傳值作為之後訊息的 dwDriverId；任一步失敗即回收並回傳 NULL。
// - SendDriverMessage（DrvSendMessage）：以執行個體的 dwDriverId 呼叫 DriverProc；無效控制代碼回 0。
// - CloseDriver（DrvClose）：送 DRV_CLOSE（呼叫者的 lParam1／lParam2）並回傳其結果；模組最後一個執行個體再送 DRV_DISABLE、DRV_FREE，每次都 FreeLibrary。
// - DefDriverProc（DrvDefDriverProc）：DRV_LOAD／ENABLE／OPEN／CLOSE／DISABLE／FREE 與 DRV_INSTALL／REMOVE（DRVCNF_OK）為 1，其餘為 0。
// - GetDriverModuleHandle（DrvGetModuleHandle）：執行個體所屬模組。
// - DriverCallback：依 DCB_* 類型張貼視窗訊息（wParam 為裝置、lParam 為 dwParam1）、執行緒訊息、呼叫 DRVCALLBACK 函式或設定事件；DCB_NULL 不做事。
// - mmTaskCreate／mmTaskBlock／mmTaskSignal／mmTaskYield／mmGetCurrentTask：驅動程式的工作執行緒。工作即執行緒（HTASK 為執行緒 ID）；
//   mmTaskCreate 以 CreateThread 從內部入口 TaskStart 開始，再以客體回呼執行 lpfn(dwInst)（TASKCALLBACK 未指定呼叫慣例，回呼框架不依賴被呼叫端清理堆疊），mmTaskSignal 張貼 WM_USER 執行緒訊息，mmTaskBlock 以 GetMessage 等候並分派視窗訊息直到收到 WM_USER。
// 所有驅動程式碼皆以客體呼叫執行（DllMain、DriverProc 與回呼可重入任何 API）。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> installable_driver_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"winmm.dll","OpenDriver",3},{"winmm.dll","OpenDriverA",3},{"winmm.dll","CloseDriver",3},{"winmm.dll","SendDriverMessage",4,0,0,0x2},
        {"winmm.dll","DefDriverProc",5,0,0,0x4},{"winmm.dll","GetDriverModuleHandle",1},{"winmm.dll","DriverCallback",7,0,0,0xa},
        {"winmm.dll","mmTaskCreate",3},{"winmm.dll","mmTaskBlock",1,0,0,0x1},{"winmm.dll","mmTaskSignal",1,0,0,0x1},{"winmm.dll","mmTaskYield",0},{"winmm.dll","mmGetCurrentTask",0},
        {"internal-winmm","TaskStart",1}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_installable_driver(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const auto name=service.symbol;
    if(name=="DefDriverProc"){
        const WindowsCall call(state,memory,service);const auto message=static_cast<std::uint32_t>(call(2));
        call.return_to(state,(message>=1 && message<=6) || message==9 || message==10?1U:0U);return std::nullopt;
    }
    if(name=="TaskStart"){
        // 執行緒起點：參數區塊 {lpfn, dwInst} 由 mmTaskCreate 配置，讀出後釋放。
        const WindowsCall call(state,memory,service);const unsigned unit=x86_?4:8;const auto block=call(0);
        const std::uint8_t bits=x86_?32:64;const auto target=memory.load(GuestAddress{block},bits),instance=memory.load(GuestAddress{block+unit},bits);(void)free_heap(memory,block,state);
        start_callbacks(state,memory,{{target,{instance}}},Continuation::Return,0);return std::nullopt;
    }
    if(name=="mmGetCurrentTask" || name=="mmTaskYield"){
        const WindowsCall call(state,memory,service);if(name=="mmTaskYield")threads_.yield=true;
        call.return_to(state,name=="mmGetCurrentTask"?thread_identifier(threads_.current):0);return std::nullopt;
    }
    if(name=="GetDriverModuleHandle"){
        const WindowsCall call(state,memory,service);const auto found=installable_drivers_.find(call(0));
        call.return_to(state,found==installable_drivers_.end()?0:found->second.module);return std::nullopt;
    }
    return nested_continuation(state,memory,"可安裝驅動程式",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto name=service.symbol;
        const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::span<const std::uint8_t> bytes)->std::uint64_t{
            const auto block=allocate_heap(m,std::max<std::size_t>(bytes.size(),1));if(!block)throw std::bad_alloc{};
            if(!bytes.empty())m.write(GuestAddress{block},std::vector<std::uint8_t>(bytes.begin(),bytes.end()));blocks.push_back(block);return block;
        };
        const auto guest=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
            co_return co_await service_callback::call_value(resolve(dll,symbol).value,std::move(arguments));
        };
        const auto procedure=[&](std::uint64_t target,std::uint64_t id,std::uint64_t handle,std::uint32_t message,std::uint64_t first,std::uint64_t second)->service_callback::Task<std::uint64_t>{
            const auto value=co_await service_callback::call_value(target,{id,handle,message,first,second});
            co_return x86_?static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)):value;
        };
        if(name=="mmTaskCreate"){
            // (lpfn, lph, dwInst)：成功回 0，無法建立執行緒回 TASKERR_OUTOFMEMORY；lph 為 NULL 時不保留執行緒 handle。
            if(!call(0))co_return finish(2);
            const unsigned unit=x86_?4:8;const auto block=allocate_heap(m,unit*2);if(!block)co_return finish(2);
            const std::uint8_t bits=x86_?32:64;m.store(GuestAddress{block},bits,call(0));m.store(GuestAddress{block+unit},bits,call(2));
            const auto thread=co_await guest("kernel32.dll","CreateThread",{0,0,resolve("internal-winmm","TaskStart").value,block,0,0});
            if(!thread){(void)free_heap(m,block,s);co_return finish(2);}
            if(call(1))m.store(GuestAddress{call(1)},x86_?32:64,thread);else (void)co_await guest("kernel32.dll","CloseHandle",{thread});
            co_return finish(0);
        }
        if(name=="mmTaskSignal")co_return finish(co_await guest("user32.dll","PostThreadMessageW",{static_cast<std::uint32_t>(call(0)),0x400,0,0}));
        if(name=="mmTaskBlock"){
            // 只等候目前執行緒；GetMessage 失敗（-1）時停止，避免空轉。
            const auto message=scratch(std::vector<std::uint8_t>(x86_?28:48));const unsigned message_offset=x86_?4:8;
            for(;;){
                const auto got=static_cast<std::int32_t>(static_cast<std::uint32_t>(co_await guest("user32.dll","GetMessageA",{message,0,0,0})));if(got==-1)break;
                if(m.load(GuestAddress{message},x86_?32:64))(void)co_await guest("user32.dll","DispatchMessageA",{message});
                if(m.load(GuestAddress{message+message_offset},32)==0x400)break;
            }
            co_return finish(0);
        }
        if(name=="DriverCallback"){
            const auto target=call(0),device=call(2),user=call(4),first=call(5),second=call(6);const auto type=static_cast<std::uint32_t>(call(1))&7U,message=static_cast<std::uint32_t>(call(3));
            if(type==0)co_return finish(1);
            if(!target)co_return finish(0);
            std::uint64_t result=0;
            if(type==1)result=co_await guest("user32.dll","PostMessageW",{target,message,device,first});
            else if(type==2)result=co_await guest("user32.dll","PostThreadMessageW",{static_cast<std::uint32_t>(target),message,device,first});
            else if(type==3){(void)co_await service_callback::call_value(target,{device,message,user,first,second});result=1;}
            else if(type==5)result=co_await guest("kernel32.dll","SetEvent",{target});
            co_return finish(result?1:0);
        }
        if(name=="SendDriverMessage"){
            const auto found=installable_drivers_.find(call(0));if(found==installable_drivers_.end())co_return finish(0);
            const auto driver=found->second;
            co_return finish(co_await procedure(driver.procedure,driver.id,call(0),static_cast<std::uint32_t>(call(1)),call(2),call(3)));
        }
        if(name=="CloseDriver"){
            const auto handle=call(0);const auto found=installable_drivers_.find(handle);if(found==installable_drivers_.end())co_return finish(0);
            const auto driver=found->second;
            const auto result=co_await procedure(driver.procedure,driver.id,handle,4,call(1),call(2));
            installable_drivers_.erase(handle);
            if(std::none_of(installable_drivers_.begin(),installable_drivers_.end(),[&](const auto& item){return item.second.module==driver.module;})){
                (void)co_await procedure(driver.procedure,0,handle,5,0,0);(void)co_await procedure(driver.procedure,0,handle,6,0,0);
            }
            (void)co_await guest("kernel32.dll","FreeLibrary",{driver.module});
            co_return finish(result);
        }
        // OpenDriver／OpenDriverA
        const bool wide=name=="OpenDriver";
        const auto text=[&](std::uint64_t address){return wide?read_utf16(m,GuestAddress{address},32767):utf8_to_utf16_lossy(read_ansi(m,address,32767));};
        if(!call(0))co_return finish(0);
        const auto driver_name=text(call(0));const std::u16string section=call(1)?text(call(1)):std::u16string(u"Drivers32");
        if(driver_name.empty())co_return finish(0);
        std::u16string file=driver_name,configuration;
        {
            const auto key_path=scratch(utf16_bytes(u"Software\\Microsoft\\Windows NT\\CurrentVersion\\"+section));
            const auto key_slot=scratch(std::vector<std::uint8_t>(8));const auto predefined=x86_?UINT64_C(0x80000002):UINT64_C(0xffffffff80000002);
            if(!co_await guest("advapi32.dll","RegOpenKeyExW",{predefined,key_path,0,0x20019,key_slot})){
                const auto key=m.load(GuestAddress{key_slot},x86_?32:64);
                const auto value_name=scratch(utf16_bytes(driver_name));const auto type=scratch(std::vector<std::uint8_t>(4));
                const auto data=scratch(std::vector<std::uint8_t>(2048));const auto size=scratch(std::vector<std::uint8_t>{0xfe,0x07,0,0});
                if(!co_await guest("advapi32.dll","RegQueryValueExW",{key,value_name,0,type,data,size})){
                    const auto kind=m.load(GuestAddress{type},32);
                    if(kind==1 || kind==2){
                        auto value=read_utf16(m,GuestAddress{data},1023);
                        while(!value.empty() && (value.front()==u' ' || value.front()==u'\t'))value.erase(value.begin());
                        const auto split=value.find_first_of(u" \t");
                        if(!value.empty()){file=value.substr(0,split);if(split!=std::u16string::npos){configuration=value.substr(split+1);while(!configuration.empty() && (configuration.front()==u' ' || configuration.front()==u'\t'))configuration.erase(configuration.begin());}}
                    }
                }
                (void)co_await guest("advapi32.dll","RegCloseKey",{key});
            }
        }
        const auto module=co_await guest("kernel32.dll","LoadLibraryW",{scratch(utf16_bytes(file))});
        if(!module)co_return finish(0);
        static constexpr std::string_view export_name{"DriverProc"};
        std::vector<std::uint8_t> export_bytes(export_name.begin(),export_name.end());export_bytes.push_back(0);
        const auto entry=co_await guest("kernel32.dll","GetProcAddress",{module,scratch(export_bytes)});
        if(!entry){(void)co_await guest("kernel32.dll","FreeLibrary",{module});co_return finish(0);}
        const bool first=std::none_of(installable_drivers_.begin(),installable_drivers_.end(),[&](const auto& item){return item.second.module==module;});
        const auto handle=next_installable_driver_+=4;
        installable_drivers_.emplace(handle,InstallableDriver{module,entry,0});
        const auto abandon=[&](bool unload)->service_callback::Task<std::uint64_t>{
            installable_drivers_.erase(handle);
            if(unload){(void)co_await procedure(entry,0,handle,5,0,0);(void)co_await procedure(entry,0,handle,6,0,0);}
            (void)co_await guest("kernel32.dll","FreeLibrary",{module});co_return 0;
        };
        if(first){
            if(!co_await procedure(entry,0,handle,1,0,0)){installable_drivers_.erase(handle);(void)co_await guest("kernel32.dll","FreeLibrary",{module});co_return finish(0);}
            (void)co_await procedure(entry,0,handle,2,0,0);
        }
        const auto id=co_await procedure(entry,0,handle,3,scratch(utf16_bytes(configuration)),call(2));
        if(!id){(void)co_await abandon(first);co_return finish(0);}
        if(const auto found=installable_drivers_.find(handle);found!=installable_drivers_.end())found->second.id=id;
        co_return finish(handle);
    });
}
}
