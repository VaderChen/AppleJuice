// kernel32 舊版相容家族（Windows SDK 公開語義），全部轉接既有服務：
// - CancelTimerQueueTimer：DeleteTimerQueueTimer(TimerQueue, Timer, NULL)。
// - ReinitializeCriticalSection（重新初始化）／UninitializeCriticalSection（等同 DeleteCriticalSection）。
// - GlobalCompact／LocalCompact：回報可用的最大區塊（以 GlobalMemoryStatusEx 的可用虛擬位址空間計，不超過 SIZE_T）。
// - GlobalFix／GlobalUnfix／GlobalWire／GlobalUnWire：鎖定／解除鎖定可移動記憶體（GlobalLock／GlobalUnlock）。
// - HeapSummary：以 HeapWalk 累計使用中區塊（cbAllocated）與含額外負擔的已提交量；cb 必須是 HEAP_SUMMARY 大小。
// - FatalExit：沒有除錯器時直接以指定代碼結束程序（TerminateProcess）。
// - LoadModule：LOADPARMS32 的 Pascal 字串命令列（去掉結尾 CR）、環境區塊與 nCmdShow 經 CreateProcessA 啟動；成功回傳大於 31 的值，
//   失敗回傳 Win32 錯誤碼（0 為記憶體不足）。
#include "x64arm/windows_services.hpp"
#include "service_callback_task.hpp"
#include <algorithm>

namespace x64arm {
std::span<const WindowsServiceDescriptor> kernel32_legacy_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"kernel32.dll","CancelTimerQueueTimer",2},{"kernel32.dll","ReinitializeCriticalSection",1},{"kernel32.dll","UninitializeCriticalSection",1},
        {"kernel32.dll","GlobalCompact",1,0,0,0x1},{"kernel32.dll","LocalCompact",1,0,0,0x1},{"kernel32.dll","GlobalFix",1},{"kernel32.dll","GlobalUnfix",1},
        {"kernel32.dll","GlobalWire",1},{"kernel32.dll","GlobalUnWire",1},{"kernel32.dll","HeapSummary",3,0,0,0x2},{"kernel32.dll","FatalExit",1,0,0,0x1},
        {"kernel32.dll","LoadModule",2}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_kernel32_legacy(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"kernel32 舊版相容",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);const auto name=service.symbol;
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto api=[&](std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
            co_return co_await service_callback::call_value(resolve("kernel32.dll",symbol).value,std::move(arguments));
        };
        const auto error_at=(x86_?s.fs_base:s.gs_base)+(x86_?0x34U:0x68U);
        const auto set_error=[&](std::uint32_t error){m.store(GuestAddress{error_at},32,error);last_error_=error;};

        if(name=="CancelTimerQueueTimer")co_return finish(static_cast<std::uint32_t>(co_await api("DeleteTimerQueueTimer",{call(0),call(1),0})));
        if(name=="ReinitializeCriticalSection"){(void)co_await api("InitializeCriticalSection",{call(0)});co_return finish(0);}
        if(name=="UninitializeCriticalSection"){(void)co_await api("DeleteCriticalSection",{call(0)});co_return finish(0);}
        if(name=="GlobalCompact" || name=="LocalCompact"){
            // MEMORYSTATUSEX {dwLength=64, dwMemoryLoad, ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual@40, …}。
            const auto status=scratch(64);m.store(GuestAddress{status},32,64);
            if(!co_await api("GlobalMemoryStatusEx",{status}))co_return finish(0);
            const auto available=m.load(GuestAddress{status+40},64);
            co_return finish(x86_?std::min<std::uint64_t>(available,UINT32_MAX):available);
        }
        if(name=="GlobalFix"){(void)co_await api("GlobalLock",{call(0)});co_return finish(0);}
        if(name=="GlobalUnfix"){(void)co_await api("GlobalUnlock",{call(0)});co_return finish(0);}
        if(name=="GlobalWire")co_return finish(co_await api("GlobalLock",{call(0)}));
        if(name=="GlobalUnWire")co_return finish(static_cast<std::uint32_t>(co_await api("GlobalUnlock",{call(0)})));
        if(name=="HeapSummary"){
            // HEAP_SUMMARY {DWORD cb; SIZE_T cbAllocated, cbCommitted, cbReserved, cbMaxReserve}。
            const auto summary=call(2);const auto size=x86_?20U:40U;
            if(!summary || static_cast<std::uint32_t>(m.load(GuestAddress{summary},32))!=size){set_error(87);co_return finish(0);}
            const auto entry=scratch(x86_?28U:40U);std::uint64_t allocated=0,overhead=0,committed=0,uncommitted=0;
            for(unsigned n=0;n<1000000;++n){
                if(!co_await api("HeapWalk",{call(0),entry}))break;
                const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{entry+unit+6},16));
                const auto data=m.load(GuestAddress{entry+unit},32);const auto extra=m.load(GuestAddress{entry+unit+4},8);
                if(flags&1U){committed+=m.load(GuestAddress{entry+(x86_?12U:16U)},32);uncommitted+=m.load(GuestAddress{entry+(x86_?16U:20U)},32);}
                else if(flags&4U){allocated+=data;overhead+=extra;}
            }
            if(static_cast<std::uint32_t>(m.load(GuestAddress{error_at},32))!=259U)co_return finish(0); // HeapWalk 的錯誤沿用其 LastError。
            set_error(0);
            // 沒有區域項目時，已提交量取使用中區塊與其額外負擔的總和。
            const auto in_use=allocated+overhead;committed=std::max(committed,in_use);
            const std::array<std::uint64_t,4> values{allocated,committed,committed+uncommitted,committed+uncommitted};
            for(unsigned i=0;i<4;++i)m.store(GuestAddress{summary+unit+i*unit},bits,values[i]);
            co_return finish(1);
        }
        if(name=="FatalExit"){(void)co_await api("TerminateProcess",{x86_?0xffffffffU:UINT64_MAX,call(0)});co_return finish(0);}
        if(name=="LoadModule"){
            // LOADPARMS32 {lpEnvAddress, lpCmdLine（Pascal 字串，CR 結尾）, lpCmdShow（WORD[2]：2、nCmdShow）, dwReserved}。
            const auto module=call(0),parameters=call(1);if(!module || !parameters)co_return finish(87);
            const auto environment=m.load(GuestAddress{parameters},bits),line=m.load(GuestAddress{parameters+unit},bits),show=m.load(GuestAddress{parameters+2*unit},bits);
            const auto path=read_ansi(m,module,32767);
            std::string arguments;
            if(line){const auto length=static_cast<std::size_t>(m.load(GuestAddress{line},8));std::vector<std::uint8_t> bytes(length);m.read(GuestAddress{line+1},bytes);
                arguments.assign(bytes.begin(),bytes.end());while(!arguments.empty() && (arguments.back()=='\r' || arguments.back()=='\0'))arguments.pop_back();}
            std::string command="\""+path+"\"";if(!arguments.empty()){if(arguments.front()!=' ')command.push_back(' ');command+=arguments;}
            const auto text=scratch(command.size()+1);m.write(GuestAddress{text},std::span(reinterpret_cast<const std::uint8_t*>(command.data()),command.size()));
            // STARTUPINFOA：Win64 104 bytes（dwFlags@60、wShowWindow@64）；Win32 68 bytes（dwFlags@44、wShowWindow@48）。
            const auto startup=scratch(x86_?68U:104U);m.store(GuestAddress{startup},32,x86_?68U:104U);
            if(show && m.load(GuestAddress{show},16)==2){m.store(GuestAddress{startup+(x86_?44U:60U)},32,1);m.store(GuestAddress{startup+(x86_?48U:64U)},16,m.load(GuestAddress{show+2},16));}
            const auto process=scratch(x86_?16U:24U);
            if(!co_await api("CreateProcessA",{0,text,0,0,0,0,environment,0,startup,process})){
                const auto error=static_cast<std::uint32_t>(m.load(GuestAddress{error_at},32));co_return finish(error==8U || error==14U?0U:error);
            }
            (void)co_await api("CloseHandle",{m.load(GuestAddress{process+unit},bits)});(void)co_await api("CloseHandle",{m.load(GuestAddress{process},bits)});
            co_return finish(33);
        }
        co_return finish(0);
    });
}
}
