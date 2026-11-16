// kernelbase 補充（由 kernel32 承接），沿用既有記憶體服務與 API Set 對應表：
// - VirtualAllocFromApp：與 VirtualAlloc 相同，但依文件不建立可執行頁（PAGE_EXECUTE* 為 ERROR_INVALID_PARAMETER）。
// - VirtualProtectFromApp：與 VirtualProtect 相同，但不允許同時可寫可執行（PAGE_EXECUTE_READWRITE／WRITECOPY）。
// - IsApiSetImplemented：契約名稱（可省略 .dll）經同一張 API Set 對應表解析到已實作的內建模組時為 TRUE。
// - StrCpyNXA／W：最多複製 cchMax-1 個碼元並補 NUL，回傳目的字串結尾 NUL 的位置。
// - EnumPageFilesA／W（psapi 與 K32 名稱）：宿主置換空間視為單一分頁檔 C:\pagefile.sys，ENUM_PAGE_FILE_INFORMATION 以頁為單位；
//   沒有置換空間時不呼叫回呼並回傳 TRUE。回呼傳回 FALSE 即停止。
#include "x64arm/windows_services.hpp"
#include "x64arm/windows_api_sets.hpp"
#include "x64arm/system_performance.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>
#include <atomic>

namespace x64arm {
std::span<const WindowsServiceDescriptor> kernelbase_extra_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","VirtualAllocFromApp",4,0,0,0xc},{"kernel32.dll","VirtualProtectFromApp",4,0,0,0x4},
        {"kernel32.dll","IsApiSetImplemented",1},{"kernel32.dll","StrCpyNXA",3,0,0,0x4},{"kernel32.dll","StrCpyNXW",3,0,0,0x4},
        {"psapi.dll","EnumPageFilesA",2},{"psapi.dll","EnumPageFilesW",2},{"kernel32.dll","K32EnumPageFilesA",2},{"kernel32.dll","K32EnumPageFilesW",2}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_kernelbase_extras(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    if(service.symbol.find("EnumPageFiles")!=std::string_view::npos)
        return nested_continuation(state,memory,"分頁檔列舉",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(s,m,service);const bool wide=service.symbol.ends_with('W');
            const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
            if(!call(0)){last_error_=87;co_return finish(0);}
            const auto snapshot=native_system_performance();if(snapshot.error){last_error_=snapshot.error;co_return finish(0);}
            if(!snapshot.swap_total_pages)co_return finish(1);
            static std::atomic<std::uint64_t> peak{0};
            auto previous=peak.load();while(previous<snapshot.swap_used_pages && !peak.compare_exchange_weak(previous,snapshot.swap_used_pages)){}
            const unsigned unit=x86_?4U:8U,size=8U+3U*unit;
            const std::u16string path=u"C:\\pagefile.sys";
            std::vector<std::uint8_t> name;if(wide)name=utf16_bytes(path);else {const auto text=utf16_to_utf8(path);name.assign(text.begin(),text.end());name.push_back(0);}
            const auto block=allocate_heap(m,size+name.size());if(!block){last_error_=8;co_return finish(0);}
            struct Release {WindowsServices& self;GuestMemory& memory;GuestState& state;std::uint64_t block;~Release(){(void)self.free_heap(memory,block,state);}} release{*this,m,s,block};
            m.fill(GuestAddress{block},0,size);m.store(GuestAddress{block},32,size);
            m.store(GuestAddress{block+8},static_cast<std::uint8_t>(unit*8),snapshot.swap_total_pages);m.store(GuestAddress{block+8+unit},static_cast<std::uint8_t>(unit*8),snapshot.swap_used_pages);
            m.store(GuestAddress{block+8+2*unit},static_cast<std::uint8_t>(unit*8),std::max(peak.load(),snapshot.swap_used_pages));
            m.write(GuestAddress{block+size},name);
            (void)co_await service_callback::call(call(0),{call(1),block,block+size});
            co_return finish(1);
        });
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;std::uint64_t result=0;
    const auto api=[&](std::string_view dll,std::string_view symbol,std::initializer_list<std::uint64_t> arguments){return nested_service(state,memory,dll,symbol,arguments);};
    try {
        if(name=="VirtualAllocFromApp"){
            if(static_cast<std::uint32_t>(arg(3))&0xf0U){last_error_=87;result=0;}
            else result=api("kernel32.dll","VirtualAlloc",{arg(0),arg(1),arg(2),arg(3)});
        }else if(name=="VirtualProtectFromApp"){
            if(static_cast<std::uint32_t>(arg(2))&0xc0U){last_error_=87;result=0;}
            else result=api("kernel32.dll","VirtualProtect",{arg(0),arg(1),arg(2),arg(3)});
        }else if(name=="IsApiSetImplemented"){
            if(arg(0)){
                auto contract=read_ansi(memory,arg(0),256);
                for(auto& c:contract)if(c>='A' && c<='Z')c=static_cast<char>(c+32);
                if(!contract.ends_with(".dll"))contract+=".dll";
                const auto host=windows_api_set_host(contract);
                result=(contract.starts_with("api-ms-") || contract.starts_with("ext-ms-")) && host!=contract && builtin_module(host)?1U:0U;
            }
        }else if(name.starts_with("StrCpyNX")){
            const bool wide=name.ends_with('W');const unsigned unit=wide?2U:1U;const auto limit=static_cast<std::int32_t>(arg(2));
            result=arg(0);
            if(arg(0) && arg(1) && limit>0){
                std::uint64_t copied=0;
                for(;copied+1<static_cast<std::uint64_t>(limit);++copied){const auto c=memory.load(GuestAddress{arg(1)+copied*unit},static_cast<std::uint8_t>(unit*8));if(!c)break;memory.store(GuestAddress{arg(0)+copied*unit},static_cast<std::uint8_t>(unit*8),c);}
                memory.store(GuestAddress{arg(0)+copied*unit},static_cast<std::uint8_t>(unit*8),0);result=arg(0)+copied*unit;
            }
        }
    }catch(const MemoryFault&){last_error_=998;if(!name.starts_with("StrCpyNX"))result=0;}
    arg.return_to(state,result);return std::nullopt;
}
}
