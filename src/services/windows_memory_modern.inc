// Windows 10 以後的記憶體與計時補充（由 windows_kernel32_extras.cpp 引入；kernelbase 由 kernel32 承接，同一入口兩邊都匯出）。
// MapViewOfFile3／MapViewOfFile3FromApp／MapViewOfFileNuma2 轉成既有 NtMapViewOfSectionEx（placeholder 取代、位址限制、NUMA 節點檢查同一套）；
// CreateFileMapping2 沿用 CreateFileMappingW 的命名、安全性與 ERROR_ALREADY_EXISTS，再以 DuplicateHandle 收斂成要求的存取權；
// QueryVirtualMemoryInformation 以 VirtualQueryEx 走過整個配置區塊，統計認可大小並依類型與對應檔名判定旗標。
// 輔助計數器是宿主硬體計數器（macOS mach_continuous_time，Apple Silicon 為 24 MHz）；QPC 正是同一計數器換成 100 ns，換算與 monotonic_ticks 一致。
namespace {
constexpr std::uint32_t hresult_notimpl=0x80004001U,hresult_invalidarg=0x80070057U,hresult_bounds=0x8000000bU,hresult_noaccess=0x800703e6U;
constexpr std::uint32_t status_not_supported=0xc00000bbU,status_invalid_parameter=0xc000000dU,status_integer_overflow=0xc0000095U,status_access_violation=0xc0000005U;
// 宿主時鐘模式才有硬體關係；確定性比較模式的 QPC 由指令數推進，與任何硬體計數器無關。
std::optional<std::pair<std::uint64_t,std::uint64_t>> auxiliary_timebase(bool host_clock){
#if defined(__APPLE__)
    if(!host_clock)return std::nullopt;
    static const auto timebase=[]{mach_timebase_info_data_t value{};if(mach_timebase_info(&value) || !value.denom)value={1,1};return value;}();
    // 計數 × numer ÷ denom＝奈秒；100 ns 刻度再除以 100。
    return std::pair<std::uint64_t,std::uint64_t>{timebase.numer,std::uint64_t{timebase.denom}*100U};
#else
    (void)host_clock;return std::nullopt;
#endif
}
struct AuxiliaryConversion {std::uint32_t status=0;std::uint64_t value=0,error=0;};
// to_performance：輔助→QPC 取下整（與 monotonic_ticks 相同），誤差 1 個 QPC 刻度；QPC→輔助取最小的對應計數，誤差為一個 QPC 刻度涵蓋的計數數。
AuxiliaryConversion auxiliary_convert(bool host_clock,bool to_performance,std::uint64_t value){
    const auto timebase=auxiliary_timebase(host_clock);if(!timebase)return {status_not_supported};
    const auto [numer,denom]=*timebase;
    const auto product=static_cast<unsigned __int128>(value)*(to_performance?numer:denom);const auto divisor=to_performance?denom:numer;
    const auto result=to_performance?product/divisor:(product+divisor-1)/divisor;
    if(result>UINT64_MAX)return {status_integer_overflow};
    return {0,static_cast<std::uint64_t>(result),to_performance?(numer+denom-1)/denom:(denom+numer-1)/numer};
}
bool memory_modern_service(std::string_view name){
    static constexpr std::string_view names[]{"MapViewOfFile3","MapViewOfFile3FromApp","MapViewOfFileNuma2","CreateFileMapping2","QueryVirtualMemoryInformation",
        "QueryAuxiliaryCounterFrequency","ConvertAuxiliaryCounterToPerformanceCounter","ConvertPerformanceCounterToAuxiliaryCounter",
        "NtQueryAuxiliaryCounterFrequency","ZwQueryAuxiliaryCounterFrequency","NtConvertBetweenAuxiliaryCounterAndPerformanceCounter","ZwConvertBetweenAuxiliaryCounterAndPerformanceCounter"};
    return std::find(std::begin(names),std::end(names),name)!=std::end(names);
}
}
std::optional<std::uint32_t> WindowsServices::invoke_memory_modern(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto saved_error=last_error_;
    std::string_view name=service.symbol;if(name.starts_with("Zw"))name=name.substr(2);else if(name.starts_with("Nt"))name=name.substr(2);
    const bool native=service.symbol.starts_with("Nt") || service.symbol.starts_with("Zw");
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto done=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return done(0);};
    const auto u32=[&](unsigned index){return static_cast<std::uint32_t>(arg(index));};
    const std::uint64_t current_process=x86_?UINT32_MAX:UINT64_MAX;
    if(name=="QueryAuxiliaryCounterFrequency" || name=="ConvertBetweenAuxiliaryCounterAndPerformanceCounter" ||
       name=="ConvertAuxiliaryCounterToPerformanceCounter" || name=="ConvertPerformanceCounterToAuxiliaryCounter"){
        // Win32 介面回 HRESULT：不支援 E_NOTIMPL、超出範圍 E_BOUNDS、輸出指標為 NULL E_INVALIDARG；NT 介面回對應 NTSTATUS。
        const auto hresult=[&](std::uint32_t status){return status==0?0U:status==status_not_supported?hresult_notimpl:status==status_integer_overflow?hresult_bounds:
            status==status_access_violation?hresult_noaccess:hresult_invalidarg;};
        const auto finish=[&](std::uint32_t status){return done(native?status:hresult(status));};
        try{
            if(name=="QueryAuxiliaryCounterFrequency"){
                const auto timebase=auxiliary_timebase(runtime_.host_clock);if(!timebase)return finish(status_not_supported);
                if(!arg(0))return finish(native?status_access_violation:status_invalid_parameter);
                // 每秒計數＝10^7 × 100 ns 刻度換算比（Apple Silicon：10^9 × 3 ÷ 125＝24 000 000）。
                memory.store(GuestAddress{arg(0)},64,UINT64_C(10000000)*timebase->second/timebase->first);return finish(0);
            }
            const bool to_performance=name=="ConvertBetweenAuxiliaryCounterAndPerformanceCounter"?(arg(0)&0xffU)!=0:name=="ConvertAuxiliaryCounterToPerformanceCounter";
            std::uint64_t input=0,output=0,error=0;
            if(native){if(!arg(1) || !arg(2))return finish(status_invalid_parameter);input=memory.load(GuestAddress{arg(1)},64);output=arg(2);error=arg(3);}
            else {input=arg(0);output=arg(1);error=arg(2);if(!output)return finish(status_invalid_parameter);}
            memory.check(GuestAddress{output},8,Access::Write);if(error)memory.check(GuestAddress{error},8,Access::Write);
            const auto converted=auxiliary_convert(runtime_.host_clock,to_performance,input);if(converted.status)return finish(converted.status);
            memory.store(GuestAddress{output},64,converted.value);if(error)memory.store(GuestAddress{error},64,converted.error);return finish(0);
        }catch(const MemoryFault&){return finish(status_access_violation);}
    }
    try{
        const auto nt_error=[&](std::uint64_t status){return static_cast<std::uint32_t>(nested_service(state,memory,"ntdll.dll","RtlNtStatusToDosError",{status&UINT32_MAX}));};
        if(name=="MapViewOfFile3" || name=="MapViewOfFile3FromApp" || name=="MapViewOfFileNuma2"){
            const bool numa=name=="MapViewOfFileNuma2";
            // MapViewOfFile3(Mapping, Process, Base, Offset, Size, Type, Protect, Params, Count)；
            // MapViewOfFileNuma2(Mapping, Process, Offset, Base, Size, Type, Protect, PreferredNode)。
            const auto handle=arg(0),process=arg(1),base=arg(numa?3:2),offset=arg(numa?2:3),size=arg(4);const auto allocation=u32(5),protection=u32(6);
            // FromApp 不允許可執行保護（沒有 codeGeneration 能力的應用程式）。
            if(name=="MapViewOfFile3FromApp" && (protection&0xf0U))return fail(87);
            GuestScratch scratch(*this,memory,state);const auto block=scratch.allocate(48);if(!block)return fail(8);
            memory.store(GuestAddress{block},bits,base);memory.store(GuestAddress{block+8},64,offset);memory.store(GuestAddress{block+16},bits,size);
            std::uint64_t parameters=numa?0:arg(7),count=numa?0:u32(8);
            if(numa && u32(7)!=UINT32_MAX){
                // 指定節點時以 MemExtendedParameterNumaNode 傳入；所有虛擬 CPU 與記憶體都位於節點 0。
                parameters=block+32;count=1;memory.store(GuestAddress{parameters},64,2);memory.store(GuestAddress{parameters+8},64,u32(7));
            }
            const auto status=nested_service(state,memory,"ntdll.dll","NtMapViewOfSectionEx",{handle,process,block,block+8,block+16,allocation,protection,parameters,count});
            if(static_cast<std::uint32_t>(status)>=0x80000000U)return fail(nt_error(status));
            last_error_=saved_error;return done(memory.load(GuestAddress{block},bits));
        }
        if(name=="CreateFileMapping2"){
            // (File, SecurityAttributes, DesiredAccess, PageProtection, AllocationAttributes, MaximumSize, Name, Params, Count)
            const auto parameters=arg(7);const auto count=u32(8);
            if(count>32 || (count && !parameters))return fail(87);
            if(count)memory.check(GuestAddress{parameters},static_cast<std::size_t>(count)*16,Access::Read);
            std::uint32_t used=0;
            for(std::uint32_t i=0;i<count;++i){
                const auto type=memory.load(GuestAddress{parameters+i*16},64),value=memory.load(GuestAddress{parameters+i*16+8},64);
                if(!type || type>3 || (used&(1U<<type)))return fail(87);used|=1U<<type;
                // MemSectionExtendedParameterNumaNode：只有節點 0；使用者實體頁旗標與簽章等級需要 AWE／映像簽章物件，不支援。
                if(type==2){if(static_cast<std::uint32_t>(value))return fail(87);}
                else return fail(50);
            }
            const auto size=arg(5);
            const auto created=nested_service(state,memory,"kernel32.dll","CreateFileMappingW",{arg(0),arg(1),u32(3)|u32(4),size>>32,size&UINT32_MAX,arg(6)})&(x86_?UINT32_MAX:UINT64_MAX);
            if(!created)return done(0);
            const auto creation_error=last_error_;const auto access=u32(2);
            // CreateFileMappingW 的控制代碼具備 SECTION_ALL_ACCESS；其他要求以複製收斂權限，繼承旗標沿用 SECURITY_ATTRIBUTES。
            if(access==0xf001fU || access==0x02000000U){last_error_=creation_error;return done(created);}
            const auto inherit=arg(1)?memory.load(GuestAddress{arg(1)+(x86_?8U:16U)},32):0;
            GuestScratch scratch(*this,memory,state);const auto slot=scratch.allocate(8);if(!slot){(void)nested_service(state,memory,"kernel32.dll","CloseHandle",{created});return fail(8);}
            const auto duplicated=nested_service(state,memory,"kernel32.dll","DuplicateHandle",{current_process,created,current_process,slot,access,inherit,0});
            const auto duplicate_error=last_error_;(void)nested_service(state,memory,"kernel32.dll","CloseHandle",{created});
            if(!(duplicated&UINT32_MAX))return fail(duplicate_error);
            last_error_=creation_error;return done(memory.load(GuestAddress{slot},bits));
        }
        if(name=="QueryVirtualMemoryInformation"){
            // (Process, Address, Class, Buffer, Size, ReturnSize)：MemoryRegionInfo（0）＝WIN32_MEMORY_REGION_INFORMATION。
            const auto process=arg(0),address=arg(1),buffer=arg(3),capacity=arg(4),returned=arg(5);
            const unsigned size=x86_?20U:32U;
            if(u32(2)!=0)return fail(87);
            if(returned)memory.store(GuestAddress{returned},bits,size);
            if(capacity<size)return fail(24);
            memory.check(GuestAddress{buffer},size,Access::Write);
            GuestScratch scratch(*this,memory,state);const auto mbi=scratch.allocate(48),path=scratch.allocate(520);if(!mbi || !path)return fail(8);
            const unsigned mbi_size=x86_?28U:48U,allocation_at=unit,protect_at=unit*2,region_at=x86_?12U:24U,state_at=x86_?16U:32U,type_at=x86_?24U:40U;
            const auto query=[&](std::uint64_t at){return nested_service(state,memory,"kernel32.dll","VirtualQueryEx",{process,at,mbi,mbi_size})!=0;};
            if(!query(address))return fail(last_error_?last_error_:87);
            if(memory.load(GuestAddress{mbi+state_at},32)==0x10000U)return fail(487);
            const auto allocation=memory.load(GuestAddress{mbi+allocation_at},bits);
            if(!query(allocation))return fail(last_error_?last_error_:87);
            const auto protection=static_cast<std::uint32_t>(memory.load(GuestAddress{mbi+protect_at},32)),type=static_cast<std::uint32_t>(memory.load(GuestAddress{mbi+type_at},32));
            std::uint64_t at=allocation,committed=0;
            for(;;){
                const auto region=memory.load(GuestAddress{mbi+region_at},bits);
                if(!region || memory.load(GuestAddress{mbi+allocation_at},bits)!=allocation || memory.load(GuestAddress{mbi+state_at},32)==0x10000U)break;
                if(memory.load(GuestAddress{mbi+state_at},32)==0x1000U)committed+=region;
                at+=region;if(at<region || !query(at))break;
            }
            // 旗標：Private、MappedDataFile、MappedImage、MappedPageFile；對應區段有檔名者是資料檔，否則由分頁檔支援。
            std::uint32_t flags=type==0x20000U?1U:type==0x1000000U?4U:0U;
            if(type==0x40000U)flags=nested_service(state,memory,"kernel32.dll","K32GetMappedFileNameW",{process,allocation,path,260})?2U:8U;
            memory.write(GuestAddress{buffer},std::vector<std::uint8_t>(size));
            memory.store(GuestAddress{buffer},bits,allocation);memory.store(GuestAddress{buffer+unit},32,protection);memory.store(GuestAddress{buffer+unit+4},32,flags);
            memory.store(GuestAddress{buffer+unit+8},bits,at-allocation);memory.store(GuestAddress{buffer+unit*2+8},bits,committed);
            last_error_=saved_error;return done(1);
        }
    }catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(8);}
    throw HostCallFault("未知的記憶體補充服務");
}
