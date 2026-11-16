#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"

namespace x64arm {
// ResolveDelayLoadedAPI（ntdll 的 LdrResolveDelayLoadedAPI）：Windows 元件的延遲載入協助函式以它解析一個延遲匯入。
// 依描述元載入模組（模組 handle 槽為空時）、解析該 thunk 對應的名稱或序數並寫回延遲 IAT；
// 失敗時以 DELAYLOAD_INFO 呼叫 DLL 失敗掛鉤，沒有時改呼叫系統失敗常式，回傳值即為結果。
// 載入與解析都走既有 LoadLibraryExA／GetProcAddress，DllMain、搜尋路徑與轉送匯出語義不另做一套。
std::optional<std::uint32_t> WindowsServices::invoke_resolve_delay_loaded(GuestState& entry_state,GuestMemory& entry_memory,const WindowsServiceDescriptor& service){
    return service_callback::invoke<std::optional<std::uint32_t>>(
        this,threads_.current,entry_state,entry_memory,"ResolveDelayLoadedAPI",
        [this](GuestState& state,GuestMemory& memory,std::uint64_t target,std::vector<std::uint64_t> args,std::uint64_t id){
            start_callbacks(state,memory,{{target,std::move(args)}},Continuation::NestedResult,id);
        },
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(state,memory,service);
            const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
            const auto done=[&](std::uint64_t value){call.return_to(state,value);return std::optional<std::uint32_t>{};};
            const auto base=call(0),descriptor=call(1),dll_hook=call(2),system_hook=call(3),thunk=call(4);
            // IMAGE_DELAYLOAD_DESCRIPTOR：Attributes、DllNameRVA、ModuleHandleRVA、ImportAddressTableRVA、ImportNameTableRVA……（皆為 RVA）。
            const auto rva=[&](unsigned offset){return base+memory.load(GuestAddress{descriptor+offset},32);};
            const auto name=rva(4),handle_cell=rva(8),iat=rva(12),names=rva(16);
            if(thunk<iat || (thunk-iat)%unit)throw HostCallFault("ResolveDelayLoadedAPI 的 thunk 不在延遲 IAT 內");
            const auto index=(thunk-iat)/unit;
            const auto entry=memory.load(GuestAddress{names+index*unit},bits);
            const bool by_ordinal=x86_?(entry&0x80000000U)!=0:(entry>>63)!=0;
            const auto ordinal=entry&0xffffU;const auto procedure=by_ordinal?ordinal:base+(entry&0xffffffffU)+2; // IMAGE_IMPORT_BY_NAME.Name
            auto module=memory.load(GuestAddress{handle_cell},bits);std::uint32_t error=0;
            if(!module){
                const auto loaded=co_await service_callback::call_value(resolve("kernel32.dll","LoadLibraryExA").value,{name,0,0});
                if(!loaded)error=last_error_?last_error_:126U;
                else if(const auto raced=memory.load(GuestAddress{handle_cell},bits)){
                    // 其他執行緒已先完成：沿用它的 handle，釋放本次多取得的參照。
                    (void)co_await service_callback::call(resolve("kernel32.dll","FreeLibrary").value,{loaded});module=raced;
                }else {memory.store(GuestAddress{handle_cell},bits,loaded);module=loaded;}
            }
            if(module){
                const auto address=co_await service_callback::call_value(resolve("kernel32.dll","GetProcAddress").value,{module,procedure});
                if(address){memory.store(GuestAddress{thunk},bits,address);co_return done(address);}
                error=last_error_?last_error_:127U;
            }
            const auto info_size=x86_?36U:72U;const auto info=allocate_heap(memory,info_size);
            if(!info)throw HostCallFault("ResolveDelayLoadedAPI 無法配置 DELAYLOAD_INFO");
            struct ReleaseInfo {WindowsServices& self;GuestState& state;GuestMemory& memory;std::uint64_t address;
                ~ReleaseInfo(){(void)self.free_heap(memory,address,state);}} release_info{*this,state,memory,info};
            memory.write(GuestAddress{info},std::vector<std::uint8_t>(info_size));
            memory.store(GuestAddress{info},32,info_size);memory.store(GuestAddress{info+unit},bits,descriptor);
            memory.store(GuestAddress{info+2*unit},bits,thunk);memory.store(GuestAddress{info+3*unit},bits,name);
            memory.store(GuestAddress{info+4*unit},32,by_ordinal?0U:1U);memory.store(GuestAddress{info+5*unit},bits,by_ordinal?ordinal:procedure);
            memory.store(GuestAddress{info+6*unit},bits,module);memory.store(GuestAddress{info+8*unit},32,error);
            last_error_=error;
            if(dll_hook)co_return done(co_await service_callback::call_value(dll_hook,{4,info})); // dliFailGetProc 之後的系統通知碼
            if(!system_hook)co_return done(0);
            co_return done(co_await service_callback::call_value(system_hook,{name,procedure}));
        });
}
}
