// ntdll 補充家族（Windows SDK／WDK 公開語義），以既有服務轉接，回傳 NTSTATUS 且不改 LastError：
// - NtGetWriteWatch／NtResetWriteWatch（Zw* 同一實作）：kernel32 GetWriteWatch／ResetWriteWatch；只接受目前程序。
// - NtCreateSectionEx（Zw*）：沒有延伸參數、或只有 NUMA 節點 0 時等同 NtCreateSection。
// - RtlCreateUserStack／RtlFreeUserStack：保留 MaximumStackSize（依 ReserveAlignment 對齊），自頂端提交 CommittedStackSize，
//   保留空間足夠時下方加一個 guard page，填寫 INITIAL_TEB。
// - RtlWalkHeap：以 HeapWalk 列舉，RTL_HEAP_WALK_ENTRY 與 PROCESS_HEAP_ENTRY 互轉（BUSY／SEGMENT／UNCOMMITTED_RANGE）。
// - RtlSetSearchPathMode：kernel32 SetSearchPathMode。
// - RtlCreateServiceSid：S-1-5-80-（大寫服務名稱 UTF-16LE 的 SHA-1，5 個 DWORD）。
// - RtlDeriveCapabilitySidsFromName：大寫能力名稱 UTF-16LE 的 SHA-256（8 個 DWORD），群組 SID S-1-5-32-…、能力 SID S-1-15-3-1024-…。
// - RtlFindMessage：LdrFindResource_U／LdrAccessResource 取得訊息表，依區塊與項目長度找出 MESSAGE_RESOURCE_ENTRY。
// - RtlPushFrame／RtlPopFrame／RtlGetFrame：TEB.ActiveFrame（x86 +0xFB0、x64 +0x17C0）的 TEB_ACTIVE_FRAME 串列（Previous 在 +指標寬度）。
// - RtlGetCurrentTransaction／RtlSetCurrentTransaction：TEB.CurrentTransactionHandle（x86 +0xFAC、x64 +0x17B8）。
// - NtAreMappedFilesTheSame（Zw*）：兩個位址所在映射的檔案相同為 STATUS_SUCCESS，不同檔案 STATUS_NOT_SAME_DEVICE，
//   未配置 STATUS_INVALID_ADDRESS，私有記憶體（非檔案映射）STATUS_CONFLICTING_ADDRESSES；沿用 GetMappedFileName 的映射查詢。
// - kernel32 Heap32First／Heap32Next：以 HeapWalk 列舉目前程序堆積的使用中與空閒區塊；HEAPENTRY32.dwResvd 保存目前序號，
//   Heap32Next 依此從頭找下一個。其他程序為 ERROR_ACCESS_DENIED，結束為 ERROR_NO_MORE_FILES。
// - ApiSetQueryApiSetPresence／Ex：與 IsApiSetImplemented 同一張 API Set 對應表；IsInSchema 為表中有對應，Present 再要求承接模組已內建。
// - NtAlertMultipleThreadByThreadId（Zw*）：對 MultipleThreadId 陣列逐一呼叫 NtAlertThreadByThreadId，回傳第一個失敗狀態。
// - RtlQueryPackageIdentity：沙箱程序沒有 MSIX 套件身分，與 GetCurrentPackageFullName 一致回 STATUS_NOT_FOUND，Packaged 為 FALSE。
// - NtSetInformationVirtualMemory（Zw*）：VmPrefetchInformation（0）轉 PrefetchVirtualMemory（旗標 ULONG 必須為 0）；其他類別 STATUS_INVALID_INFO_CLASS。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "message_table_entry.hpp"
#include "service_callback_task.hpp"
#include "x64arm/windows_api_sets.hpp"
#include <openssl/evp.h>
#include <algorithm>

namespace x64arm {
namespace {
constexpr std::uint32_t status_invalid_parameter=0xc000000dU,status_invalid_handle=0xc0000008U,status_no_memory=0xc0000017U;
constexpr std::uint32_t status_buffer_too_small=0xc0000023U,status_no_more_entries=0x8000001aU,status_message_not_found=0xc0000109U,status_access_denied=0xc0000022U;
template<std::size_t N> std::optional<std::array<std::uint8_t,N>> digest(const EVP_MD* method,std::span<const std::uint8_t> data){
    std::array<std::uint8_t,N> out{};unsigned length=0;
    if(EVP_Digest(data.data(),data.size(),out.data(),&length,method,nullptr)!=1 || length!=N)return std::nullopt;
    return out;
}
std::uint64_t round_up(std::uint64_t value,std::uint64_t unit){return unit?(value+unit-1)/unit*unit:value;}
}
std::span<const WindowsServiceDescriptor> ntdll_extras_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"ntdll.dll","NtGetWriteWatch",7,0,0,0x2},{"ntdll.dll","ZwGetWriteWatch",7,0,0,0x2},{"ntdll.dll","NtResetWriteWatch",3},{"ntdll.dll","ZwResetWriteWatch",3},
        {"ntdll.dll","NtCreateSectionEx",9,0,0,0x132},{"ntdll.dll","ZwCreateSectionEx",9,0,0,0x132},
        {"ntdll.dll","RtlCreateUserStack",6},{"ntdll.dll","RtlFreeUserStack",1},{"ntdll.dll","RtlWalkHeap",2},
        {"ntdll.dll","RtlSetSearchPathMode",1,0,0,0x1},
        {"ntdll.dll","RtlCreateServiceSid",3},{"ntdll.dll","RtlDeriveCapabilitySidsFromName",3},{"ntdll.dll","RtlFindMessage",5,0,0,0xe},
        {"ntdll.dll","RtlPushFrame",1},{"ntdll.dll","RtlPopFrame",1},{"ntdll.dll","RtlGetFrame",0},{"ntdll.dll","RtlGetCurrentTransaction",0},{"ntdll.dll","RtlSetCurrentTransaction",1},
        {"ntdll.dll","NtAreMappedFilesTheSame",2},{"ntdll.dll","ZwAreMappedFilesTheSame",2},{"kernel32.dll","Heap32First",3},{"kernel32.dll","Heap32Next",1},
        {"ntdll.dll","ApiSetQueryApiSetPresence",2},{"ntdll.dll","ApiSetQueryApiSetPresenceEx",3},
        {"ntdll.dll","NtAlertMultipleThreadByThreadId",4,0,0,0xa},{"ntdll.dll","ZwAlertMultipleThreadByThreadId",4,0,0,0xa},{"ntdll.dll","RtlQueryPackageIdentity",6},
        {"ntdll.dll","NtSetInformationVirtualMemory",6,0,0,0x22},{"ntdll.dll","ZwSetInformationVirtualMemory",6,0,0,0x22}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_ntdll_extras(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    return nested_continuation(state,memory,"ntdll 補充",[this,service](GuestState& s,GuestMemory& m)->service_callback::Task<std::optional<std::uint32_t>>{
        const WindowsCall call(s,m,service);const auto finish=[&](std::uint64_t value){call.return_to(s,value);return std::optional<std::uint32_t>{};};
        const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
        auto name=service.symbol;if(name.starts_with("Zw"))name.remove_prefix(2);else if(name.starts_with("Nt"))name.remove_prefix(2);
        std::vector<std::uint64_t> blocks;
        struct Release {WindowsServices& self;GuestMemory& m;GuestState& s;std::vector<std::uint64_t>& blocks;~Release(){for(const auto block:blocks)(void)self.free_heap(m,block,s);}} release{*this,m,s,blocks};
        const auto scratch=[&](std::size_t size)->std::uint64_t{const auto block=allocate_heap(m,std::max<std::size_t>(size,1));if(!block)throw std::bad_alloc{};m.fill(GuestAddress{block},0,std::max<std::size_t>(size,1));blocks.push_back(block);return block;};
        const auto entry=[&](std::string_view dll,std::string_view symbol){return resolve(dll,symbol).value;};
        // 轉接的 Win32 函式會設定 LastError；Nt／Rtl 介面不改 LastError，結束時還原。
        const auto teb=x86_?s.fs_base:s.gs_base;const auto error_at=teb+(x86_?0x34U:0x68U);
        const auto saved_error=m.load(GuestAddress{error_at},32);
        const auto last_error=[&]{return static_cast<std::uint32_t>(m.load(GuestAddress{error_at},32));};
        // 協調者的 last_error_ 快取與 TEB 都要還原（直譯模式的 GetLastError 讀快取）。
        const auto done=[&](std::uint64_t value){m.store(GuestAddress{error_at},32,saved_error);last_error_=static_cast<std::uint32_t>(saved_error);return finish(value);};
        const auto win32=[&](std::string_view dll,std::string_view symbol,std::vector<std::uint64_t> arguments)->service_callback::Task<std::uint64_t>{
            co_return co_await service_callback::call_value(entry(dll,symbol),std::move(arguments));
        };

        // TEB_ACTIVE_FRAME 串列與目前交易 handle：只讀寫目前執行緒的 TEB 欄位。
        const auto active_frame=teb+(x86_?0xfb0U:0x17c0U),current_transaction=teb+(x86_?0xfacU:0x17b8U);
        if(name=="RtlGetFrame")co_return done(m.load(GuestAddress{active_frame},bits));
        if(name=="ApiSetQueryApiSetPresence" || name=="ApiSetQueryApiSetPresenceEx"){
            // (PCUNICODE_STRING Namespace, [PBOOLEAN IsInSchema,] PBOOLEAN Present)
            const bool extended=name.ends_with("Ex");const auto present_at=call(extended?2:1),schema_at=extended?call(1):0;
            if(!call(0) || !present_at || (extended && !schema_at))co_return done(status_invalid_parameter);
            const auto length=static_cast<std::uint32_t>(m.load(GuestAddress{call(0)},16));const auto buffer=m.load(GuestAddress{call(0)+unit},bits);
            if(length%2 || length>512 || (length && !buffer))co_return done(status_invalid_parameter);
            std::string contract;for(std::uint32_t i=0;i<length;i+=2){auto c=static_cast<char16_t>(m.load(GuestAddress{buffer+i},16));if(c>=u'A' && c<=u'Z')c=static_cast<char16_t>(c+32);
                if(c>0x7f)co_return done(status_invalid_parameter);contract.push_back(static_cast<char>(c));}
            if(!contract.ends_with(".dll"))contract+=".dll";
            const auto host=windows_api_set_host(contract);const bool in_schema=(contract.starts_with("api-ms-") || contract.starts_with("ext-ms-")) && host!=contract;
            m.store(GuestAddress{present_at},8,in_schema && builtin_module(host)?1U:0U);if(extended)m.store(GuestAddress{schema_at},8,in_schema?1U:0U);
            co_return done(0);
        }
        if(name=="AlertMultipleThreadByThreadId"){
            const auto ids=call(0);const auto count=static_cast<std::uint32_t>(call(1));
            if(count && !ids)co_return done(status_invalid_parameter);if(count>1024)co_return done(status_invalid_parameter);
            std::uint32_t status=0;
            for(std::uint32_t i=0;i<count;++i){const auto id=m.load(GuestAddress{ids+std::uint64_t{i}*unit},bits);
                const auto result=static_cast<std::uint32_t>(co_await win32("ntdll.dll","NtAlertThreadByThreadId",{id}));if(result && !status)status=result;}
            co_return done(status);
        }
        if(name=="RtlQueryPackageIdentity"){
            // (TokenObject, PackageFullName, PackageSize*, AppId, AppIdSize*, Packaged*)
            if(call(5))m.store(GuestAddress{call(5)},8,0);
            co_return done(0xc0000225U); // STATUS_NOT_FOUND
        }
        if(name=="SetInformationVirtualMemory"){
            // (ProcessHandle, VmInformationClass, NumberOfEntries, VirtualAddresses, VmInformation, VmInformationLength)
            const auto kind=static_cast<std::uint32_t>(call(1)),length=static_cast<std::uint32_t>(call(5));
            if(kind!=0)co_return done(0xc0000003U); // STATUS_INVALID_INFO_CLASS
            if(length!=4)co_return done(0xc0000004U); // STATUS_INFO_LENGTH_MISMATCH
            if(!call(4))co_return done(status_invalid_parameter);
            const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{call(4)},32));if(flags)co_return done(status_invalid_parameter);
            if(co_await win32("kernel32.dll","PrefetchVirtualMemory",{call(0),call(2),call(3),0})&UINT32_MAX)co_return done(0);
            const auto error=last_error();
            co_return done(error==5?status_access_denied:error==6?status_invalid_handle:error==8?status_no_memory:error==50?0xc00000bbU:status_invalid_parameter);
        }
        if(name=="RtlPushFrame"){if(call(0)){m.store(GuestAddress{call(0)+unit},bits,m.load(GuestAddress{active_frame},bits));m.store(GuestAddress{active_frame},bits,call(0));}co_return done(0);}
        if(name=="RtlPopFrame"){if(call(0))m.store(GuestAddress{active_frame},bits,m.load(GuestAddress{call(0)+unit},bits));co_return done(0);}
        if(name=="RtlGetCurrentTransaction")co_return done(m.load(GuestAddress{current_transaction},bits));
        if(name=="RtlSetCurrentTransaction"){m.store(GuestAddress{current_transaction},bits,call(0));co_return done(1);}
        if(name=="AreMappedFilesTheSame"){
            constexpr std::uint32_t status_invalid_address=0xc0000141U,status_conflicting_addresses=0xc0000018U,status_not_same_device=0xc00000d4U;
            if(!m.query(GuestAddress{call(0)}) || !m.query(GuestAddress{call(1)}))co_return done(status_invalid_address);
            auto first=mapped_file_name(call(0)),second=mapped_file_name(call(1));if(first.empty() || second.empty())co_return done(status_conflicting_addresses);
            for(auto* text:{&first,&second})for(auto& c:*text)c=unicode_case_unit(c,true);
            co_return done(first==second?0U:status_not_same_device);
        }
        if(name=="Heap32First" || name=="Heap32Next"){
            // HEAPENTRY32：dwSize、hHandle、dwAddress、dwBlockSize（皆為指標寬度）、dwFlags、dwLockCount、dwResvd、th32ProcessID（DWORD）、th32HeapID。
            const auto win32_fail=[&](std::uint32_t code){m.store(GuestAddress{error_at},32,code);last_error_=code;return finish(0);};
            const auto entry_out=call(0);const unsigned entry_size=x86_?36U:56U,flags_at=4*unit,index_at=4*unit+8,process_at=4*unit+12,heap_at=4*unit+16;
            if(!entry_out)co_return win32_fail(87);if(m.load(GuestAddress{entry_out},bits)<entry_size)co_return win32_fail(24);
            const bool first=name=="Heap32First";const auto process=first?static_cast<std::uint32_t>(call(1)):static_cast<std::uint32_t>(m.load(GuestAddress{entry_out+process_at},32));
            const auto heap=first?call(2):m.load(GuestAddress{entry_out+heap_at},bits);const std::uint32_t wanted=first?0U:static_cast<std::uint32_t>(m.load(GuestAddress{entry_out+index_at},32))+1U;
            if(process && process!=process_id())co_return win32_fail(5);
            // PROCESS_HEAP_ENTRY 走訪：略過區域與未提交範圍，使用中與空閒區塊依序編號。
            const unsigned walk_size=x86_?28U:40U,handle_at=x86_?12U:16U;const auto walk=scratch(walk_size);std::uint32_t index=0;
            for(;;){
                if(!co_await win32("kernel32.dll","HeapWalk",{heap,walk})){const auto error=last_error();co_return win32_fail(error==259U?18U:error==6U?87U:error);}
                const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{walk+unit+6},16));if(flags&3U)continue;
                if(index++!=wanted)continue;
                const auto address=m.load(GuestAddress{walk},bits);const bool moveable=(flags&0x10U)!=0;
                m.store(GuestAddress{entry_out+unit},bits,moveable?m.load(GuestAddress{walk+handle_at},bits):address);m.store(GuestAddress{entry_out+2*unit},bits,address);
                m.store(GuestAddress{entry_out+3*unit},bits,m.load(GuestAddress{walk+unit},32));m.store(GuestAddress{entry_out+flags_at},32,(flags&4U)?(moveable?4U:1U):2U);
                m.store(GuestAddress{entry_out+flags_at+4},32,0);m.store(GuestAddress{entry_out+index_at},32,wanted);m.store(GuestAddress{entry_out+process_at},32,process_id());
                m.store(GuestAddress{entry_out+heap_at},bits,heap);co_return done(1);
            }
        }
        if(name=="GetWriteWatch" || name=="ResetWriteWatch"){
            if(!current_process_handle(call(0)))co_return done(status_invalid_handle);
            const auto result=name=="GetWriteWatch"?co_await win32("kernel32.dll","GetWriteWatch",{call(1),call(2),call(3),call(4),call(5),call(6)})
                :co_await win32("kernel32.dll","ResetWriteWatch",{call(1),call(2)});
            co_return done(static_cast<std::uint32_t>(result)?status_invalid_parameter:0U);
        }
        if(name=="CreateSectionEx"){
            // MEM_EXTENDED_PARAMETER：16 bytes，低 8 位元為型別；NUMA 節點（2）只接受 0。
            const auto parameters=call(7);const auto count=static_cast<std::uint32_t>(call(8));
            if(count && !parameters)co_return done(status_invalid_parameter);
            for(std::uint32_t i=0;i<count && i<64;++i){
                const auto type=m.load(GuestAddress{parameters+i*16U},8);const auto value=m.load(GuestAddress{parameters+i*16U+8},32);
                if(type!=2 || value!=0)co_return done(status_invalid_parameter);
            }
            if(count>64)co_return done(status_invalid_parameter);
            co_return done(static_cast<std::uint32_t>(co_await service_callback::call(entry("ntdll.dll","NtCreateSection"),{call(0),call(1),call(2),call(3),call(4),call(5),call(6)})));
        }
        if(name=="RtlCreateUserStack"){
            const auto page=call(3)?call(3):4096U,alignment=call(4)?call(4):65536U;const auto initial=call(5);
            if(!initial || (page&(page-1)) || (alignment&(alignment-1)))co_return done(status_invalid_parameter);
            auto maximum=call(1)?call(1):(runtime_.image.stack_size?runtime_.image.stack_size:UINT64_C(0x100000));
            auto commit=round_up(call(0)?call(0):page,page);
            if(commit>=maximum)maximum=round_up(commit,0x100000);
            maximum=round_up(maximum,alignment);
            if(maximum>(x86_?UINT64_C(0x40000000):UINT64_C(0x100000000)))co_return done(status_no_memory);
            const auto base=co_await win32("kernel32.dll","VirtualAlloc",{0,maximum,0x2000,4});
            if(!base)co_return done(status_no_memory);
            const bool guard=maximum-commit>=page;
            const auto low=base+maximum-commit-(guard?page:0);
            if(!co_await win32("kernel32.dll","VirtualAlloc",{low,commit+(guard?page:0),0x1000,4})){
                (void)co_await win32("kernel32.dll","VirtualFree",{base,0,0x8000});co_return done(status_no_memory);
            }
            if(guard){const auto old=scratch(4);(void)co_await win32("kernel32.dll","VirtualProtect",{low,page,0x104,old});}
            // INITIAL_TEB {OldStackBase, OldStackLimit, StackBase, StackLimit, StackAllocationBase}。
            const std::array<std::uint64_t,5> values{0,0,base+maximum,base+maximum-commit,base};
            for(unsigned i=0;i<5;++i)m.store(GuestAddress{initial+i*unit},bits,values[i]);
            co_return done(0);
        }
        if(name=="RtlFreeUserStack"){if(call(0))(void)co_await win32("kernel32.dll","VirtualFree",{call(0),0,0x8000});co_return done(0);}
        if(name=="RtlWalkHeap"){
            // RTL_HEAP_WALK_ENTRY：Win64 {DataAddress, DataSize@8, OverheadBytes@16(4), SegmentIndex@20(2), Flags@22(2), union@24(24)}；
            // Win32 {4, 4@4, 4@8, 2@12, 2@14, union@16(16)}。PROCESS_HEAP_ENTRY：{lpData, cbData(4), cbOverhead(1), iRegionIndex(1), wFlags(2), union}。
            const auto walk=call(1);if(!walk)co_return done(status_invalid_parameter);
            const unsigned size_at=unit,overhead_at=2*unit,segment_at=2*unit+4,flags_at=2*unit+6,union_at=2*unit+8;
            // 兩者的 union 版面相同：區域／區段為 {committed(4), uncommitted(4), 首, 尾}；區塊的第一欄為 hMem／Settable（指標寬度）。
            const unsigned union_bytes=x86_?16U:24U,entry_bytes=x86_?28U:40U,heap_union=x86_?12U:16U;
            const auto process=scratch(entry_bytes);
            const auto rtl_flags=static_cast<std::uint32_t>(m.load(GuestAddress{walk+flags_at},16));
            std::uint32_t win_flags=0;if(rtl_flags&1U)win_flags|=4U;if(rtl_flags&2U)win_flags|=1U;if(rtl_flags&0x100U)win_flags|=2U;
            m.store(GuestAddress{process},bits,m.load(GuestAddress{walk},bits));m.store(GuestAddress{process+unit},32,m.load(GuestAddress{walk+size_at},32));
            m.store(GuestAddress{process+unit+4},8,m.load(GuestAddress{walk+overhead_at},8));m.store(GuestAddress{process+unit+5},8,m.load(GuestAddress{walk+segment_at},8));
            m.store(GuestAddress{process+unit+6},16,win_flags);
            {std::vector<std::uint8_t> payload(union_bytes);m.read(GuestAddress{walk+union_at},payload);m.write(GuestAddress{process+heap_union},payload);}
            if(!co_await win32("kernel32.dll","HeapWalk",{call(0),process})){
                const auto error=last_error();co_return done(error==259U?status_no_more_entries:error==6U?status_invalid_handle:status_invalid_parameter);
            }
            const auto flags=static_cast<std::uint32_t>(m.load(GuestAddress{process+unit+6},16));
            std::uint32_t out_flags=0;if(flags&4U)out_flags|=1U;if(flags&1U)out_flags|=2U;if(flags&2U)out_flags|=0x100U;
            m.store(GuestAddress{walk},bits,m.load(GuestAddress{process},bits));m.store(GuestAddress{walk+size_at},bits,m.load(GuestAddress{process+unit},32));
            m.store(GuestAddress{walk+overhead_at},32,m.load(GuestAddress{process+unit+4},8));m.store(GuestAddress{walk+segment_at},16,m.load(GuestAddress{process+unit+5},8));
            m.store(GuestAddress{walk+flags_at},16,out_flags);
            {std::vector<std::uint8_t> payload(union_bytes);m.read(GuestAddress{process+heap_union},payload);m.write(GuestAddress{walk+union_at},payload);}
            co_return done(0);
        }
        if(name=="RtlSetSearchPathMode"){
            if(co_await win32("kernel32.dll","SetSearchPathMode",{call(0)}))co_return done(0);
            co_return done(last_error()==5U?status_access_denied:status_invalid_parameter);
        }
        if(name=="RtlCreateServiceSid" || name=="RtlDeriveCapabilitySidsFromName"){
            // UNICODE_STRING {Length, MaximumLength, Buffer@unit}；名稱以 RtlUpcaseUnicodeString 的規則轉大寫後雜湊 UTF-16LE 位元組。
            const auto text=call(0);if(!text)co_return done(status_invalid_parameter);
            const auto length=static_cast<std::size_t>(m.load(GuestAddress{text},16));const auto buffer=m.load(GuestAddress{text+unit},bits);
            if((length&1U) || (length && !buffer))co_return done(status_invalid_parameter);
            std::vector<std::uint8_t> bytes(length);
            for(std::size_t i=0;i<length/2;++i){const auto c=unicode_case_unit(static_cast<char16_t>(m.load(GuestAddress{buffer+i*2},16)),true);
                bytes[i*2]=static_cast<std::uint8_t>(c);bytes[i*2+1]=static_cast<std::uint8_t>(c>>8);}
            // SID：Revision 1、SubAuthorityCount、IdentifierAuthority（6 bytes 大端序）、SubAuthority[]（小端序 DWORD）。
            const auto write_sid=[&](std::uint64_t at,std::uint8_t authority,std::span<const std::uint32_t> leading,std::span<const std::uint8_t> hash){
                const auto count=leading.size()+hash.size()/4;
                m.store(GuestAddress{at},8,1);m.store(GuestAddress{at+1},8,count);for(unsigned i=0;i<5;++i)m.store(GuestAddress{at+2+i},8,0);m.store(GuestAddress{at+7},8,authority);
                for(std::size_t i=0;i<leading.size();++i)m.store(GuestAddress{at+8+i*4},32,leading[i]);
                for(std::size_t i=0;i<hash.size()/4;++i){std::uint32_t value=0;for(unsigned b=0;b<4;++b)value|=std::uint32_t{hash[i*4+b]}<<(b*8);
                    m.store(GuestAddress{at+8+(leading.size()+i)*4},32,value);}
            };
            if(name=="RtlCreateServiceSid"){
                const auto sid=call(1),size=call(2);if(!size)co_return done(status_invalid_parameter);
                constexpr std::uint32_t needed=8+4*6;
                if(static_cast<std::uint32_t>(m.load(GuestAddress{size},32))<needed || !sid){m.store(GuestAddress{size},32,needed);co_return done(status_buffer_too_small);}
                const auto hash=digest<20>(EVP_sha1(),bytes);if(!hash)co_return done(status_no_memory);
                const std::uint32_t service_rid[]{80};write_sid(sid,5,service_rid,*hash);m.store(GuestAddress{size},32,needed);co_return done(0);
            }
            if(!call(1) || !call(2))co_return done(status_invalid_parameter);
            const auto hash=digest<32>(EVP_sha256(),bytes);if(!hash)co_return done(status_no_memory);
            const std::uint32_t group[]{32},capability[]{3,1024};
            write_sid(call(1),5,group,*hash);write_sid(call(2),15,capability,*hash);co_return done(0);
        }
        if(name=="RtlFindMessage"){
            // ResourceIdPath {型別（訊息表 ID）, 名稱 1, 語言}；MESSAGE_RESOURCE_DATA {NumberOfBlocks, {LowId, HighId, OffsetToEntries}[]}，
            // MESSAGE_RESOURCE_ENTRY {USHORT Length, USHORT Flags, Text}，依 Length 逐項前進。
            const auto module=call(0),output=call(4);const auto table=static_cast<std::uint32_t>(call(1)),language=static_cast<std::uint32_t>(call(2)),id=static_cast<std::uint32_t>(call(3));
            if(!output)co_return done(status_invalid_parameter);
            const auto path=scratch(3*unit),data_entry=scratch(unit),address=scratch(unit),length=scratch(4);
            m.store(GuestAddress{path},bits,table);m.store(GuestAddress{path+unit},bits,1);m.store(GuestAddress{path+2*unit},bits,language);
            auto status=static_cast<std::uint32_t>(co_await service_callback::call(entry("ntdll.dll","LdrFindResource_U"),{module,path,3,data_entry}));
            if(status)co_return done(status);
            status=static_cast<std::uint32_t>(co_await service_callback::call(entry("ntdll.dll","LdrAccessResource"),{module,m.load(GuestAddress{data_entry},bits),address,length}));
            if(status)co_return done(status);
            const auto base=m.load(GuestAddress{address},bits);const auto size=static_cast<std::uint32_t>(m.load(GuestAddress{length},32));
            const auto found=message_table_entry(m,base,size,id);
            if(found){m.store(GuestAddress{output},bits,found);co_return done(0);}
            co_return done(status_message_not_found);
        }
        co_return done(status_invalid_parameter);
    });
}
}
