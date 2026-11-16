#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "service_callback_task.hpp"
#include <algorithm>

// ntdll 表格與 WOW64 家族：Rtl*AtomTable（與 kernel32／user32 atom 共用 gui_atom_table_add）、RTL_HANDLE_TABLE（RtlInitializeHandleTable／
// RtlAllocateHandle／RtlFreeHandle／RtlIsValidHandle／RtlIsValidIndexHandle／RtlDestroyHandleTable）、RtlWow64GetProcessMachines／GetCurrentMachine／
// IsWowGuestMachineSupported／GetThreadContext／SetThreadContext／SuspendThread，以及 WinSqm*（遙測未加入時的行為）。
namespace x64arm {
namespace {
constexpr std::uint32_t status_invalid_parameter=0xc000000dU,status_invalid_handle=0xc0000008U,status_no_memory=0xc0000017U,status_not_found=0xc0000034U;
constexpr std::uint32_t status_buffer_too_small=0xc0000023U,status_was_locked=0x40000019U,status_unsuccessful=0xc0000001U,status_access_denied=0xc0000022U;
}
std::span<const WindowsServiceDescriptor> ntdll_table_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ntdll.dll","RtlCreateAtomTable",2,0,0,0x1},{"ntdll.dll","RtlDestroyAtomTable",1},{"ntdll.dll","RtlEmptyAtomTable",2,0,0,0x2},
        {"ntdll.dll","RtlAddAtomToAtomTable",3},{"ntdll.dll","RtlLookupAtomInAtomTable",3},{"ntdll.dll","RtlDeleteAtomFromAtomTable",2,0,0,0x2},
        {"ntdll.dll","RtlPinAtomInAtomTable",2,0,0,0x2},{"ntdll.dll","RtlQueryAtomInAtomTable",6,0,0,0x2},
        {"ntdll.dll","RtlInitializeHandleTable",3,0,0,0x3},{"ntdll.dll","RtlAllocateHandle",2},{"ntdll.dll","RtlFreeHandle",2},{"ntdll.dll","RtlIsValidHandle",2},
        {"ntdll.dll","RtlIsValidIndexHandle",3,0,0,0x2},{"ntdll.dll","RtlDestroyHandleTable",1},
        {"ntdll.dll","RtlWow64GetProcessMachines",3},{"ntdll.dll","RtlWow64GetCurrentMachine",0},{"ntdll.dll","RtlWow64IsWowGuestMachineSupported",2,0,0,0x1},
        {"kernel32.dll","IsWow64GuestMachineSupported",2,0,0,0x1},{"kernel32.dll","GetMachineTypeAttributes",2,0,0,0x1},
        {"ntdll.dll","RtlWow64GetThreadContext",2},{"ntdll.dll","RtlWow64SetThreadContext",2},{"ntdll.dll","RtlWow64SuspendThread",2},
        {"ntdll.dll","WinSqmIsOptedIn",0},{"ntdll.dll","WinSqmStartSession",3,0,0,0x6},{"ntdll.dll","WinSqmEndSession",1},
        {"ntdll.dll","WinSqmIncrementDWORD",3,0,0,0x6},{"ntdll.dll","WinSqmSetDWORD",3,0,0,0x6},{"ntdll.dll","WinSqmSetIfMaxDWORD",3,0,0,0x6},
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_ntdll_tables(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const auto host=[&](std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,"kernel32.dll",symbol,args);};
    try{
        if(name.starts_with("WinSqm")){
            // 遙測（SQM）沒有加入：WinSqmIsOptedIn 為 FALSE，StartSession 不建立工作階段（NULL），其餘記錄呼叫不保存資料。
            return done(0);
        }
        if(name.ends_with("AtomTable") || name.find("AtomInAtomTable")!=name.npos || name.find("AtomToAtomTable")!=name.npos || name.find("AtomFromAtomTable")!=name.npos){
            if(name=="RtlCreateAtomTable"){
                // (TableSize, PVOID* 輸入輸出)：*handle 已非 NULL 時沿用；表格 handle 為一塊客體記憶體的位址。
                if(!arg(1))return done(status_invalid_parameter);if(memory.load(GuestAddress{arg(1)},bits))return done(0);
                const auto handle=allocate_heap(memory,16);if(!handle)return done(status_no_memory);rtl_atom_tables_.emplace(handle,RtlAtomTable{});
                memory.store(GuestAddress{arg(1)},bits,handle);return done(0);
            }
            const auto found=rtl_atom_tables_.find(arg(0));if(found==rtl_atom_tables_.end())return done(status_invalid_parameter);auto& table=found->second;
            if(name=="RtlDestroyAtomTable"){rtl_atom_tables_.erase(found);(void)free_heap(memory,arg(0),state);return done(0);}
            if(name=="RtlEmptyAtomTable"){
                const bool pinned=(u32(1)&0xffU)!=0;
                for(auto it=table.atoms.atoms.begin();it!=table.atoms.atoms.end();){if(!pinned && table.pinned.contains(it->first)){++it;continue;}
                    const auto atom=it->first;it=table.atoms.atoms.erase(it);std::erase_if(table.atoms.names,[&](const auto& item){return item.second==atom;});table.pinned.erase(atom);}
                return done(0);
            }
            // 名稱：MAKEINTATOM（< 0xC000，非 0）或 "#數字" 為整數 atom；其他為最多 255 字元的字串。
            const auto parse=[&](std::uint64_t pointer)->std::pair<std::uint16_t,std::u16string>{
                if(pointer<0x10000){if(!pointer || pointer>=0xc000)throw std::invalid_argument("atom");return {static_cast<std::uint16_t>(pointer),{}};}
                auto text=read_utf16(memory,GuestAddress{pointer},256);if(text.empty() || text.size()>255)throw std::invalid_argument("atom");
                if(text.front()==u'#' && text.size()>1 && std::all_of(text.begin()+1,text.end(),[](char16_t c){return c>=u'0' && c<=u'9';})){
                    std::uint32_t number=0;for(std::size_t i=1;i<text.size();++i){number=number*10U+static_cast<std::uint32_t>(text[i]-u'0');if(number>=0xc000)throw std::invalid_argument("atom");}
                    if(!number)throw std::invalid_argument("atom");return {static_cast<std::uint16_t>(number),{}};}
                return {0,std::move(text)};};
            if(name=="RtlAddAtomToAtomTable" || name=="RtlLookupAtomInAtomTable"){
                const bool add=name=="RtlAddAtomToAtomTable";auto [number,text]=parse(arg(1));std::uint16_t atom=number;
                if(!number){std::uint32_t error=0;atom=gui_atom_table_add(table.atoms.atoms,table.atoms.names,std::move(text),add,error);if(error)return done(status_no_memory);if(!atom)return done(status_not_found);}
                if(arg(2))memory.store(GuestAddress{arg(2)},16,atom);return done(0);
            }
            const auto atom=static_cast<std::uint16_t>(u32(1));
            if(name=="RtlDeleteAtomFromAtomTable"){
                // 整數 atom 不在表中，刪除一律成功；釘選的字串 atom 回 STATUS_WAS_LOCKED。
                if(atom<0xc000)return done(0);const auto entry=table.atoms.atoms.find(atom);if(entry==table.atoms.atoms.end())return done(status_invalid_handle);
                if(table.pinned.contains(atom))return done(status_was_locked);
                if(!--entry->second.references){table.atoms.atoms.erase(entry);std::erase_if(table.atoms.names,[&](const auto& item){return item.second==atom;});}return done(0);
            }
            if(name=="RtlPinAtomInAtomTable"){if(atom<0xc000)return done(0);if(!table.atoms.atoms.contains(atom))return done(status_invalid_handle);table.pinned.insert(atom);return done(0);}
            if(name=="RtlQueryAtomInAtomTable"){
                // (table, atom, RefCount*, PinCount*, 名稱緩衝區, 名稱長度* 位元組)：整數 atom 的名稱為 "#n"、參考 1、釘選 1。
                std::u16string text;std::uint32_t references=1,pins=1;
                if(atom>=0xc000){const auto entry=table.atoms.atoms.find(atom);if(entry==table.atoms.atoms.end())return done(status_invalid_handle);text=entry->second.name;references=entry->second.references;pins=table.pinned.contains(atom)?1:0;}
                else {if(!atom)return done(status_invalid_parameter);text=utf8_to_utf16("#"+std::to_string(atom));}
                if(arg(2))memory.store(GuestAddress{arg(2)},32,references);if(arg(3))memory.store(GuestAddress{arg(3)},32,pins);
                if(arg(4) && arg(5)){const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(5)},32));const auto required=static_cast<std::uint32_t>(text.size()*2);
                    if(capacity<required+2){memory.store(GuestAddress{arg(5)},32,required);return done(status_buffer_too_small);}
                    memory.write(GuestAddress{arg(4)},utf16_bytes(text));memory.store(GuestAddress{arg(5)},32,required);}
                return done(0);
            }
        }
        if(name.find("Handle")!=name.npos && name!="RtlWow64SuspendThread"){
            // RTL_HANDLE_TABLE：MaximumNumberOfHandles、SizeOfHandleTableEntry、Reserved[2]、FreeHandles、CommittedHandles、UnCommittedHandles、MaxReservedHandles。
            // 第一次配置時保留整段空間，之後逐頁提交並把新項目串到空閒清單（項目首欄為 Flags／NextFree）；RTL_HANDLE_ALLOCATED（1）由呼叫端設定。
            const auto table=arg(name=="RtlInitializeHandleTable"?2:0);if(!table)return done(name=="RtlInitializeHandleTable" || name=="RtlDestroyHandleTable"?status_invalid_parameter:0);
            const auto field=[&](unsigned index)->std::uint64_t{return memory.load(GuestAddress{table+16+index*unit},bits);};
            const auto set=[&](unsigned index,std::uint64_t value){memory.store(GuestAddress{table+16+index*unit},bits,value);};
            const auto maximum=static_cast<std::uint32_t>(memory.load(GuestAddress{table},32)),entry_size=static_cast<std::uint32_t>(memory.load(GuestAddress{table+4},32));
            if(name=="RtlInitializeHandleTable"){memory.write(GuestAddress{table},std::vector<std::uint8_t>(16+4*unit));memory.store(GuestAddress{table},32,u32(0));memory.store(GuestAddress{table+4},32,u32(1));return done(0);}
            if(name=="RtlDestroyHandleTable"){if(const auto base=field(1))(void)host("VirtualFree",{base,0,0x8000});set(0,0);set(1,0);set(2,0);set(3,0);return done(0);} // MEM_RELEASE
            const auto valid=[&](std::uint64_t entry){const auto base=field(1),end=field(2);return entry && base && entry>=base && entry<end && entry_size && (entry-base)%entry_size==0 && (memory.load(GuestAddress{entry},32)&1U);};
            if(name=="RtlIsValidHandle")return done(valid(arg(1))?1U:0U);
            if(name=="RtlIsValidIndexHandle"){
                if(arg(2))memory.store(GuestAddress{arg(2)},bits,0);const auto base=field(1);if(!base)return done(0);
                const auto entry=base+std::uint64_t{u32(1)}*entry_size;if(!valid(entry))return done(0);if(arg(2))memory.store(GuestAddress{arg(2)},bits,entry);return done(1);
            }
            if(name=="RtlFreeHandle"){if(!valid(arg(1)))return done(0);memory.write(GuestAddress{arg(1)},std::vector<std::uint8_t>(entry_size));memory.store(GuestAddress{arg(1)},bits,field(0));set(0,arg(1));return done(1);}
            if(name=="RtlAllocateHandle"){
                if(!entry_size || entry_size<unit || !maximum)return done(0);
                if(!field(0)){
                    if(!field(1)){const auto total=std::uint64_t{maximum}*entry_size;auto base=host("VirtualAlloc",{0,total,0x2000,4});if(x86_)base&=UINT32_MAX;if(!base)return done(0); // MEM_RESERVE
                        set(1,base);set(2,base);set(3,base+total);}
                    const auto start=field(2),limit=field(3);if(start>=limit)return done(0);
                    const auto chunk=std::min<std::uint64_t>(limit-start,std::max<std::uint64_t>(4096,(std::uint64_t{entry_size}+4095)/4096*4096));
                    auto committed=host("VirtualAlloc",{start,chunk,0x1000,4});if(x86_)committed&=UINT32_MAX;if(!committed)return done(0); // MEM_COMMIT
                    const auto count=chunk/entry_size;std::uint64_t next=0;for(std::uint64_t i=count;i-->0;){const auto entry=start+i*entry_size;memory.store(GuestAddress{entry},bits,next);next=entry;}
                    set(0,start);set(2,start+count*entry_size);
                }
                const auto entry=field(0);set(0,memory.load(GuestAddress{entry},bits));memory.store(GuestAddress{entry},bits,0);
                if(arg(1))memory.store(GuestAddress{arg(1)},32,(entry-field(1))/entry_size);return done(entry);
            }
        }
        if(name=="RtlWow64GetCurrentMachine")return done(x86_?0x14cU:0x8664U); // 目前 CPU 區域：I386（WOW64）或 AMD64
        // 虛擬 Windows 為 x64 系統：只有 i386 以 WOW64 容器執行（ARM64 等其他機器類型不支援）。
        const auto wow_guest=[&](std::uint32_t machine){return (machine&0xffffU)==0x14cU;};
        if(name=="RtlWow64IsWowGuestMachineSupported"){if(!arg(1))return done(status_invalid_parameter);memory.store(GuestAddress{arg(1)},8,wow_guest(u32(0))?1U:0U);return done(0);}
        if(name=="IsWow64GuestMachineSupported"){if(!arg(1))return done(0x80070057U);memory.store(GuestAddress{arg(1)},32,wow_guest(u32(0))?1U:0U);return done(0);}
        if(name=="GetMachineTypeAttributes"){
            // MACHINE_ATTRIBUTES：UserEnabled 1、KernelEnabled 2、Wow64Container 4；AMD64 為原生使用者與核心模式。
            if(!arg(1))return done(0x80070057U);const auto machine=u32(0)&0xffffU;
            memory.store(GuestAddress{arg(1)},32,machine==0x8664U?3U:wow_guest(machine)?5U:0U);return done(0);
        }
        if(name=="RtlWow64GetProcessMachines"){
            // 與 IsWow64Process2 相同的機器類型：原生 64 位元程序為 IMAGE_FILE_MACHINE_UNKNOWN，WOW64 程序為 I386；主機為 AMD64。
            GuestScratch temporary(*this,memory,state);const auto values=temporary.allocate(8);if(!values)return done(status_no_memory);
            if(!(host("IsWow64Process2",{arg(0),values,values+2})&UINT32_MAX))return done(last_error_==6?status_invalid_handle:status_invalid_parameter);
            if(arg(1))memory.store(GuestAddress{arg(1)},16,memory.load(GuestAddress{values},16));if(arg(2))memory.store(GuestAddress{arg(2)},16,memory.load(GuestAddress{values+2},16));return done(0);
        }
        if(name=="RtlWow64GetThreadContext" || name=="RtlWow64SetThreadContext" || name=="RtlWow64SuspendThread"){
            // 轉給 kernel32 的 Wow64 版本（可能需要暫停其他執行緒），BOOL 與最後錯誤轉成 NTSTATUS。
            return nested_continuation(state,memory,"RtlWow64Thread",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
                const WindowsCall call(s,m,service);const auto symbol=service.symbol;const auto target=symbol=="RtlWow64SuspendThread"?"Wow64SuspendThread":symbol=="RtlWow64GetThreadContext"?"Wow64GetThreadContext":"Wow64SetThreadContext";
                const auto stub=host_service_stub("kernel32.dll",target);const auto entry=stub.value?stub.value:resolve("kernel32.dll",target).value;
                const auto status=[&](std::uint32_t error)->std::uint32_t{return error==6?status_invalid_handle:error==5?status_access_denied:error==87?status_invalid_parameter:status_unsuccessful;};
                const auto result=static_cast<std::uint32_t>(co_await service_callback::call(entry,symbol=="RtlWow64SuspendThread"?std::vector<std::uint64_t>{call(0)}:std::vector<std::uint64_t>{call(0),call(1)}));
                if(symbol=="RtlWow64SuspendThread"){if(result==UINT32_MAX){call.return_to(s,std::uint64_t{status(last_error_)});co_return std::nullopt;}if(call(1))m.store(GuestAddress{call(1)},32,result);call.return_to(s,std::uint64_t{0});co_return std::nullopt;}
                call.return_to(s,std::uint64_t{result?0U:status(last_error_)});co_return std::nullopt;
            });
        }
    }catch(const std::invalid_argument&){return done(status_invalid_parameter);}catch(const MemoryFault&){return done(0xc0000005U);}catch(const std::bad_alloc&){return done(status_no_memory);}
    throw HostCallFault("未知的 ntdll 表格服務");
}
}
