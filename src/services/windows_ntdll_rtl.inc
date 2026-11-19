// 包含於 windows_nt_native.cpp：ntdll 的 Rtl 轉接家族（臨界區段、條件變數、一次性初始化、FLS、計時器佇列、等待登錄、
// 安全描述元／SID／ACL、時區、環境變數與目前目錄）。語義交給既有 Win32 實作，只轉換引數形式與回傳的 NTSTATUS／BOOLEAN，
// 並保留呼叫者的 Win32 last error（Rtl 函式不改變它）。
namespace {
// 引數位置相同、Win32 版回 BOOL 而 Rtl 版回 NTSTATUS 的函式。bytes 標出 Rtl 版為 BOOLEAN／UCHAR 的引數（只取低 8 位元），
// words 標出 USHORT 引數，booleans 標出 Rtl 版為 BOOLEAN*（1 byte）而 Win32 版為 BOOL*（4 bytes）的輸出引數。
// 預設在宿主端直接呼叫 Win32 實作（不經客體）；guest 標出可能阻塞或需要客體回呼（執行緒池、FLS 回呼）而必須走客體回呼續行的項目。
struct RtlForward {std::string_view symbol,family,target;std::uint16_t bytes{},words{},booleans{};bool guest{};};
constexpr RtlForward rtl_forwards[]{
    {"RtlCopySid","advapi32.dll","CopySid"},
    {"RtlInitializeSid","advapi32.dll","InitializeSid",0x4},
    {"RtlAllocateAndInitializeSid","advapi32.dll","AllocateAndInitializeSid",0x2},
    {"RtlCreateAcl","advapi32.dll","InitializeAcl"},
    {"RtlAddAce","advapi32.dll","AddAce"},
    {"RtlGetAce","advapi32.dll","GetAce"},
    {"RtlDeleteAce","advapi32.dll","DeleteAce"},
    {"RtlAddAccessAllowedAce","advapi32.dll","AddAccessAllowedAce"},
    {"RtlAddAccessAllowedAceEx","advapi32.dll","AddAccessAllowedAceEx"},
    {"RtlAddAccessDeniedAce","advapi32.dll","AddAccessDeniedAce"},
    {"RtlAddAccessDeniedAceEx","advapi32.dll","AddAccessDeniedAceEx"},
    {"RtlAddAuditAccessAce","advapi32.dll","AddAuditAccessAce",0x30},
    {"RtlAddAuditAccessAceEx","advapi32.dll","AddAuditAccessAceEx",0x60},
    {"RtlAddAccessAllowedObjectAce","advapi32.dll","AddAccessAllowedObjectAce"},
    {"RtlAddAccessDeniedObjectAce","advapi32.dll","AddAccessDeniedObjectAce"},
    {"RtlAddAuditAccessObjectAce","advapi32.dll","AddAuditAccessObjectAce",0x180},
    // 私有物件安全：IsDirectoryObject 在 Rtl 版為 BOOLEAN，其餘引數與 advapi32 版相同。
    {"RtlNewSecurityObject","advapi32.dll","CreatePrivateObjectSecurity",0x8},
    {"RtlNewSecurityObjectEx","advapi32.dll","CreatePrivateObjectSecurityEx",0x10},
    {"RtlNewSecurityObjectWithMultipleInheritance","advapi32.dll","CreatePrivateObjectSecurityWithMultipleInheritance",0x20},
    {"RtlConvertToAutoInheritSecurityObject","advapi32.dll","ConvertToAutoInheritPrivateObjectSecurity",0x10},
    {"RtlDeleteSecurityObject","advapi32.dll","DestroyPrivateObjectSecurity"},
    {"RtlQueryInformationAcl","advapi32.dll","GetAclInformation"},
    {"RtlCreateSecurityDescriptor","advapi32.dll","InitializeSecurityDescriptor"},
    {"RtlSetDaclSecurityDescriptor","advapi32.dll","SetSecurityDescriptorDacl",0xa},
    {"RtlSetSaclSecurityDescriptor","advapi32.dll","SetSecurityDescriptorSacl",0xa},
    {"RtlSetOwnerSecurityDescriptor","advapi32.dll","SetSecurityDescriptorOwner",0x4},
    {"RtlSetGroupSecurityDescriptor","advapi32.dll","SetSecurityDescriptorGroup",0x4},
    {"RtlGetOwnerSecurityDescriptor","advapi32.dll","GetSecurityDescriptorOwner",0,0,0x4},
    {"RtlGetGroupSecurityDescriptor","advapi32.dll","GetSecurityDescriptorGroup",0,0,0x4},
    {"RtlGetDaclSecurityDescriptor","advapi32.dll","GetSecurityDescriptorDacl",0,0,0xa},
    {"RtlGetSaclSecurityDescriptor","advapi32.dll","GetSecurityDescriptorSacl",0,0,0xa},
    {"RtlGetControlSecurityDescriptor","advapi32.dll","GetSecurityDescriptorControl"},
    {"RtlSetControlSecurityDescriptor","advapi32.dll","SetSecurityDescriptorControl",0,0x6},
    {"RtlMakeSelfRelativeSD","advapi32.dll","MakeSelfRelativeSD"},
    {"RtlAbsoluteToSelfRelativeSD","advapi32.dll","MakeSelfRelativeSD"},
    {"RtlSelfRelativeToAbsoluteSD","advapi32.dll","MakeAbsoluteSD"},
    {"RtlImpersonateSelf","advapi32.dll","ImpersonateSelf"},
    {"RtlLocalTimeToSystemTime","kernel32.dll","LocalFileTimeToFileTime"},
    {"RtlSystemTimeToLocalTime","kernel32.dll","FileTimeToLocalFileTime"},
    {"RtlFlsFree","kernel32.dll","FlsFree",0,0,0,true},
    {"RtlFlsSetValue","kernel32.dll","FlsSetValue"},
    {"RtlDeleteTimer","kernel32.dll","DeleteTimerQueueTimer",0,0,0,true},
    {"RtlDeleteTimerQueueEx","kernel32.dll","DeleteTimerQueueEx",0,0,0,true},
    {"RtlUpdateTimer","kernel32.dll","ChangeTimerQueueTimer",0,0,0,true},
    {"RtlRegisterWait","kernel32.dll","RegisterWaitForSingleObject",0,0,0,true},
    {"RtlDeregisterWait","kernel32.dll","UnregisterWait",0,0,0,true},
    {"RtlDeregisterWaitEx","kernel32.dll","UnregisterWaitEx",0,0,0,true},
    {"RtlQueueWorkItem","kernel32.dll","QueueUserWorkItem",0,0,0,true},
    {"RtlSetThreadErrorMode","kernel32.dll","SetThreadErrorMode"},
    {"RtlRunOnceComplete","kernel32.dll","InitOnceComplete"},
    {"RtlQueryHeapInformation","kernel32.dll","HeapQueryInformation"},
    {"RtlSetHeapInformation","kernel32.dll","HeapSetInformation"},
};
constexpr std::string_view rtl_custom_symbols[]{
    "RtlEnterCriticalSection","RtlLeaveCriticalSection","RtlTryEnterCriticalSection","RtlInitializeCriticalSection",
    "RtlInitializeCriticalSectionAndSpinCount","RtlInitializeCriticalSectionEx","RtlIsCriticalSectionLocked","RtlIsCriticalSectionLockedByThread",
    "RtlSleepConditionVariableCS","RtlSleepConditionVariableSRW","RtlRunOnceBeginInitialize",
    "RtlFlsAlloc","RtlFlsGetValue","RtlCreateTimerQueue","RtlCreateTimer","RtlAdjustPrivilege","RtlAddMandatoryAce",
    "RtlQueryTimeZoneInformation","RtlQueryDynamicTimeZoneInformation","RtlGetSystemTimePrecise",
    "RtlGetCurrentDirectory_U","RtlSetCurrentDirectory_U","RtlDoesFileExists_U","RtlDosSearchPath_U",
    "RtlQueryEnvironmentVariable_U","RtlQueryEnvironmentVariable","RtlSetEnvironmentVariable","RtlExpandEnvironmentStrings_U",
    "RtlExpandEnvironmentStrings","RtlCreateEnvironment","RtlDestroyEnvironment","RtlCopySecurityDescriptor","RtlValidRelativeSecurityDescriptor",
    "RtlSetCurrentEnvironment","RtlDefaultNpAcl","RtlAddProcessTrustLabelAce"};
const RtlForward* rtl_forward(std::string_view symbol){
    const auto found=std::find_if(std::begin(rtl_forwards),std::end(rtl_forwards),[&](const auto& item){return item.symbol==symbol;});
    return found==std::end(rtl_forwards)?nullptr:found;
}
// SYSTEMTIME（年、月、星期、日、時、分、秒、毫秒）→ TIME_FIELDS（年、月、日、時、分、秒、毫秒、星期）。
void rtl_time_fields_from_systemtime(GuestMemory& memory,std::uint64_t source,std::uint64_t target){
    std::array<std::uint16_t,8> value{};for(unsigned i=0;i<8;++i)value[i]=static_cast<std::uint16_t>(memory.load(GuestAddress{source+i*2},16));
    const std::array<std::uint16_t,8> fields{value[0],value[1],value[3],value[4],value[5],value[6],value[7],value[2]};
    for(unsigned i=0;i<8;++i)memory.store(GuestAddress{target+i*2},16,fields[i]);
}
}
bool rtl_family_service(std::string_view symbol){
    return rtl_forward(symbol) || std::find(std::begin(rtl_custom_symbols),std::end(rtl_custom_symbols),symbol)!=std::end(rtl_custom_symbols);
}
std::optional<std::uint32_t> WindowsServices::invoke_rtl_family(GuestState& entry_state,GuestMemory& entry_memory,const WindowsServiceDescriptor& service){
    if(const auto* forward=rtl_forward(service.symbol);forward && !forward->guest){
        // 同步轉接：在宿主端直接執行 Win32 實作，轉換 BOOLEAN 引數／輸出與 NTSTATUS。
        const WindowsCall call(entry_state,entry_memory,service);const auto saved_error=last_error_;
        const auto finish=[&](std::uint64_t value){last_error_=saved_error;call.return_to(entry_state,value);return std::optional<std::uint32_t>{};};
        std::vector<std::uint64_t> args;args.reserve(service.arguments);
        for(unsigned i=0;i<service.arguments;++i){
            auto value=call(i);
            if(forward->bytes&(1U<<i))value&=0xffU;else if(forward->words&(1U<<i))value&=0xffffU;
            args.push_back(value);
        }
        GuestScratch temporary(*this,entry_memory,entry_state);std::uint64_t scratch=0;
        try{
            if(forward->booleans){
                scratch=temporary.allocate(64);if(!scratch)return finish(0xc0000017U);
                for(unsigned i=0;i<args.size();++i)if(forward->booleans&(1U<<i)){if(!args[i])return finish(0xc0000005U);args[i]=scratch+i*4;}
            }
            last_error_=0;
            const auto ok=static_cast<std::uint32_t>(nested_service(entry_state,entry_memory,forward->family,forward->target,std::span<const std::uint64_t>(args)));
            if(ok && forward->booleans)for(unsigned i=0;i<service.arguments;++i)if(forward->booleans&(1U<<i))
                entry_memory.store(GuestAddress{call(i)},8,entry_memory.load(GuestAddress{scratch+i*4},32)?1U:0U);
            return finish(ok?0U:nt_status(last_error_));
        }catch(const MemoryFault&){return finish(0xc0000005U);}
    }
    if(service.symbol=="RtlEnterCriticalSection"){
        // 未競爭時在宿主端直接取得（含遞迴）；只有需要等待時才走客體回呼續行。
        const WindowsCall call(entry_state,entry_memory,service);const auto saved_error=last_error_;
        if(nested_service(entry_state,entry_memory,"kernel32.dll","TryEnterCriticalSection",{call(0)})){last_error_=saved_error;call.return_to(entry_state,0);return std::nullopt;}
        last_error_=saved_error;
    }
    return nested_continuation(entry_state,entry_memory,"RtlFamily",
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(state,memory,service);const auto name=service.symbol;const auto saved_error=last_error_;
            const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
            constexpr std::uint32_t invalid_parameter=0xc000000dU,access_violation=0xc0000005U,no_memory=0xc0000017U,buffer_too_small=0xc0000023U;
            const auto finish=[&](std::uint64_t value){last_error_=saved_error;call.return_to(state,value);return std::optional<std::uint32_t>{};};
            // 經客體回呼續行時也直接進宿主服務入口，不經匯出改導向的客體快速常式。
            const auto kernel=[&](std::string_view symbol){const auto stub=host_service_stub("kernel32.dll",symbol);return stub.value?stub.value:resolve("kernel32.dll",symbol).value;};
            const auto word=[&](unsigned index){return static_cast<std::uint32_t>(call(index));};
            const auto status_of=[&](std::uint32_t ok){return ok?0U:nt_status(last_error_);};
            // 不會阻塞、也不需要客體回呼的 Win32 實作在宿主端直接執行。
            const auto host=[&](std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> args){return static_cast<std::uint32_t>(nested_service(state,memory,family,symbol,args));};
            const auto host_value=[&](std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,family,symbol,args);};
            GuestScratch temporary(*this,memory,state);std::uint64_t scratch=0;
            const auto allocate=[&](std::size_t size)->bool{scratch=temporary.allocate(size);return scratch!=0;};
            // UNICODE_STRING 內容（不含 NUL）。
            const auto counted=[&](std::uint64_t address){return native_string(memory,address,x86_);};
            const auto string_header=[&](std::uint64_t address,std::uint16_t length){memory.store(GuestAddress{address},16,length);};
            const auto string_capacity=[&](std::uint64_t address){return std::uint32_t{read_counted_header(memory,address,x86_).capacity};};
            const auto string_buffer=[&](std::uint64_t address){return read_counted_header(memory,address,x86_).buffer;};
            // 目前程序環境的 RtlCreateEnvironment 格式複本（雙 NUL 結尾；空環境為四個 0）。
            const auto environment_snapshot=[&]{
                std::vector<std::uint8_t> bytes;const auto source=host_value("kernel32.dll","GetEnvironmentStringsW",{});
                if(source){
                    auto at=x86_?source&UINT32_MAX:source;
                    for(;;){const auto entry=read_utf16(memory,GuestAddress{at},32767);if(entry.empty())break;const auto chunk=utf16_bytes(entry);bytes.insert(bytes.end(),chunk.begin(),chunk.end());at+=(entry.size()+1)*2;}
                    (void)host("kernel32.dll","FreeEnvironmentStringsW",{source});
                }
                bytes.push_back(0);bytes.push_back(0);if(bytes.size()==2){bytes.push_back(0);bytes.push_back(0);}
                return bytes;
            };
            try{
                if(const auto* forward=rtl_forward(name)){
                    std::vector<std::uint64_t> args;args.reserve(service.arguments);
                    for(unsigned i=0;i<service.arguments;++i){
                        auto value=call(i);
                        if(forward->bytes&(1U<<i))value&=0xffU;else if(forward->words&(1U<<i))value&=0xffffU;
                        args.push_back(value);
                    }
                    // BOOLEAN* 輸出改經暫存 BOOL，再寫回 1 byte。
                    if(forward->booleans){if(!allocate(4*16))co_return finish(no_memory);
                        for(unsigned i=0;i<args.size();++i)if(forward->booleans&(1U<<i)){if(!args[i])co_return finish(access_violation);args[i]=scratch+i*4;}}
                    const auto stub=host_service_stub(forward->family,forward->target);
                    const auto ok=co_await service_callback::call(stub.value?stub.value:resolve(forward->family,forward->target).value,std::move(args));
                    if(ok && forward->booleans)for(unsigned i=0;i<service.arguments;++i)if(forward->booleans&(1U<<i))
                        memory.store(GuestAddress{call(i)},8,memory.load(GuestAddress{scratch+i*4},32)?1U:0U);
                    co_return finish(status_of(ok));
                }
                if(name=="RtlLeaveCriticalSection"){(void)host("kernel32.dll","LeaveCriticalSection",{call(0)});co_return finish(0);}
                if(name=="RtlEnterCriticalSection"){
                    // 競爭時需要讓執行緒等待，經客體回呼續行讓排程器接手；取得後回 STATUS_SUCCESS。
                    (void)co_await service_callback::call(kernel("EnterCriticalSection"),{call(0)});co_return finish(0);
                }
                if(name=="RtlTryEnterCriticalSection"){
                    co_return finish(host("kernel32.dll","TryEnterCriticalSection",{call(0)})?1U:0U);
                }
                if(name=="RtlInitializeCriticalSection" || name=="RtlInitializeCriticalSectionAndSpinCount" || name=="RtlInitializeCriticalSectionEx"){
                    std::vector<std::uint64_t> args{call(0)};for(unsigned i=1;i<service.arguments;++i)args.push_back(word(i));
                    last_error_=0;const auto ok=static_cast<std::uint32_t>(nested_service(state,memory,"kernel32.dll",name.substr(3),std::span<const std::uint64_t>(args)));
                    co_return finish(name=="RtlInitializeCriticalSection" || ok?0U:nt_status(last_error_));
                }
                if(name=="RtlIsCriticalSectionLocked" || name=="RtlIsCriticalSectionLockedByThread"){
                    // 已登錄的物件依共用字組協定判斷；未登錄者直接讀結構的 LockCount（-1 為未持有）與 OwningThread。
                    const auto address=call(0);if(!address)co_return finish(access_violation);
                    const auto found=runtime_.critical_sections.find(address);bool held=false,mine=false;
                    if(found!=runtime_.critical_sections.end()){held=critical_held(memory,address,found->second);mine=critical_owned_by(memory,address,found->second,threads_.current);}
                    else {
                        held=static_cast<std::int32_t>(memory.load(GuestAddress{address+(x86_?4U:8U)},32))!=-1;
                        mine=held && memory.load(GuestAddress{address+(x86_?12U:16U)},bits)==thread_identifier(threads_.current);
                    }
                    co_return finish((name=="RtlIsCriticalSectionLocked"?held:mine)?1U:0U);
                }
                if(name=="RtlSleepConditionVariableCS" || name=="RtlSleepConditionVariableSRW"){
                    // (Cond, Lock, Timeout LARGE_INTEGER*[, Flags])：逾時回 STATUS_TIMEOUT。
                    const auto milliseconds=nt_timeout_milliseconds(memory,call(2),runtime_.system_filetime());
                    const bool srw=name.ends_with("SRW");
                    const auto ok=srw?co_await service_callback::call(kernel("SleepConditionVariableSRW"),{call(0),call(1),milliseconds,word(3)}):
                        co_await service_callback::call(kernel("SleepConditionVariableCS"),{call(0),call(1),milliseconds});
                    co_return finish(ok?0U:last_error_==1460?0x102U:nt_status(last_error_));
                }
                if(name=="RtlRunOnceBeginInitialize"){
                    // 已完成回 STATUS_SUCCESS，需由呼叫者完成初始化時回 STATUS_PENDING；CHECK_ONLY 且尚未完成為 STATUS_UNSUCCESSFUL。
                    if(!allocate(8))co_return finish(no_memory);
                    const auto ok=co_await service_callback::call(kernel("InitOnceBeginInitialize"),{call(0),word(1),scratch,call(2)});
                    if(!ok)co_return finish(last_error_==87?invalid_parameter:0xc0000001U);
                    co_return finish(memory.load(GuestAddress{scratch},32)?0x103U:0U);
                }
                if(name=="RtlFlsAlloc"){
                    if(!call(1))co_return finish(access_violation);
                    const auto index=host("kernel32.dll","FlsAlloc",{call(0)});
                    if(index==UINT32_MAX)co_return finish(no_memory);
                    memory.store(GuestAddress{call(1)},32,index);co_return finish(0);
                }
                if(name=="RtlFlsGetValue"){
                    if(!call(1))co_return finish(access_violation);
                    last_error_=0;const auto value=host_value("kernel32.dll","FlsGetValue",{word(0)});
                    if(!value && last_error_)co_return finish(invalid_parameter);
                    memory.store(GuestAddress{call(1)},bits,x86_?value&UINT32_MAX:value);co_return finish(0);
                }
                if(name=="RtlCreateTimerQueue"){
                    if(!call(0))co_return finish(access_violation);
                    auto queue=host_value("kernel32.dll","CreateTimerQueue",{});if(x86_)queue&=UINT32_MAX;
                    if(!queue)co_return finish(nt_status(last_error_));
                    memory.store(GuestAddress{call(0)},bits,queue);co_return finish(0);
                }
                if(name=="RtlCreateTimer"){
                    // (Queue, NewTimer*, Callback, Parameter, DueTime, Period, Flags) → CreateTimerQueueTimer(NewTimer*, Queue, …)
                    if(!call(1))co_return finish(access_violation);
                    const auto ok=host("kernel32.dll","CreateTimerQueueTimer",{call(1),call(0),call(2),call(3),word(4),word(5),word(6)});
                    co_return finish(status_of(ok));
                }
                if(name=="RtlAdjustPrivilege"){
                    // (Privilege LUID 低位, Enable, CurrentThread, WasEnabled*)：沿用目前執行緒或程序權杖的 AdjustTokenPrivileges。
                    if(!call(3))co_return finish(access_violation);
                    if(!allocate(64))co_return finish(no_memory);
                    const bool thread=(call(2)&0xffU)!=0;const auto token=scratch;
                    const auto opened=thread?host("advapi32.dll","OpenThreadToken",{x86_?UINT32_MAX-1U:UINT64_MAX-1U,0x28,0,token}):
                        host("advapi32.dll","OpenProcessToken",{x86_?UINT32_MAX:UINT64_MAX,0x28,token});
                    if(!opened)co_return finish(thread && last_error_==1008?0xc000007cU:nt_status(last_error_));
                    const auto handle=memory.load(GuestAddress{token},bits);
                    const auto request=scratch+16,previous=scratch+32,length=scratch+56;
                    memory.store(GuestAddress{request},32,1);memory.store(GuestAddress{request+4},32,word(0));memory.store(GuestAddress{request+8},32,0);
                    memory.store(GuestAddress{request+12},32,(call(1)&0xffU)?2U:0U);
                    const auto ok=host("advapi32.dll","AdjustTokenPrivileges",{handle,0,request,20,previous,length});
                    const auto error=last_error_;(void)host("kernel32.dll","CloseHandle",{handle});
                    if(!ok)co_return finish(nt_status(error));
                    if(error==1300)co_return finish(0xc0000061U); // STATUS_PRIVILEGE_NOT_HELD
                    // PreviousState 只列出實際變動的特權；沒有變動代表原本就是要求的狀態。
                    const bool changed=memory.load(GuestAddress{previous},32)!=0;
                    const bool was=changed?(memory.load(GuestAddress{previous+12},32)&2U)!=0:(call(1)&0xffU)!=0;
                    memory.store(GuestAddress{call(3)},8,was?1U:0U);co_return finish(0);
                }
                if(name=="RtlAddMandatoryAce"){
                    // (Acl, Revision, AceFlags, MandatoryFlags, AceType, LabelSid)：只接受 SYSTEM_MANDATORY_LABEL_ACE_TYPE（0x11）。
                    if((call(4)&0xffU)!=0x11U)co_return finish(invalid_parameter);
                    co_return finish(status_of(host("advapi32.dll","AddMandatoryAce",{call(0),word(1),word(2),word(3),call(5)})));
                }
                if(name=="RtlAddProcessTrustLabelAce"){
                    // (Acl, AceRevision, AceFlags, ProcessTrustLabelSid, AceType, AccessMask)：只接受 SYSTEM_PROCESS_TRUST_LABEL_ACE_TYPE（0x14），
                    // SID 須為程序信任 SID（S-1-19-類型-等級，兩個子授權）；ACE 經 AddAce 附加在 ACL 尾端，共用空間與版本檢查。
                    if((call(4)&0xffU)!=0x14U || (word(2)&~0x1fU) || !call(0) || !call(3))co_return finish(invalid_parameter);
                    std::vector<std::uint8_t> sid(8);memory.read(GuestAddress{call(3)},sid);
                    if(sid[0]!=1 || sid[1]!=2 || !std::all_of(sid.begin()+2,sid.begin()+7,[](auto value){return value==0;}) || sid[7]!=19)co_return finish(invalid_parameter);
                    sid.resize(16);memory.read(GuestAddress{call(3)},sid);
                    std::vector<std::uint8_t> ace(8+sid.size());ace[0]=0x14;ace[1]=static_cast<std::uint8_t>(word(2));ace[2]=static_cast<std::uint8_t>(ace.size());
                    for(unsigned i=0;i<4;++i)ace[4+i]=static_cast<std::uint8_t>(word(5)>>(i*8));std::copy(sid.begin(),sid.end(),ace.begin()+8);
                    if(!allocate(ace.size()))co_return finish(no_memory);memory.write(GuestAddress{scratch},ace);
                    if(host("advapi32.dll","AddAce",{call(0),word(1),UINT32_MAX,scratch,ace.size()}))co_return finish(0);
                    co_return finish(last_error_==122?0xc0000099U:nt_status(last_error_)); // 空間不足為 STATUS_ALLOTTED_SPACE_EXCEEDED
                }
                if(name=="RtlQueryTimeZoneInformation" || name=="RtlQueryDynamicTimeZoneInformation"){
                    // RTL_TIME_ZONE_INFORMATION 與 TIME_ZONE_INFORMATION 版面相同，只有兩個日期是 TIME_FIELDS 而非 SYSTEMTIME。
                    if(!call(0))co_return finish(access_violation);
                    const bool dynamic=name=="RtlQueryDynamicTimeZoneInformation";const std::size_t size=dynamic?432:172;
                    if(!allocate(size))co_return finish(no_memory);
                    const auto id=host("kernel32.dll",dynamic?"GetDynamicTimeZoneInformation":"GetTimeZoneInformation",{scratch});
                    if(id==UINT32_MAX)co_return finish(nt_status(last_error_));
                    std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{scratch},bytes);memory.write(GuestAddress{call(0)},bytes);
                    rtl_time_fields_from_systemtime(memory,scratch+68,call(0)+68);rtl_time_fields_from_systemtime(memory,scratch+152,call(0)+152);
                    co_return finish(0);
                }
                if(name=="RtlGetSystemTimePrecise"){
                    if(!allocate(8))co_return finish(0);
                    (void)host("kernel32.dll","GetSystemTimePreciseAsFileTime",{scratch});
                    const auto value=memory.load(GuestAddress{scratch},64);
                    last_error_=saved_error;call.return_to(state,value,true);co_return std::optional<std::uint32_t>{};
                }
                if(name=="RtlGetCurrentDirectory_U"){
                    // (ULONG 位元組數, PWSTR)：成功回不含 NUL 的位元組數；緩衝區不足回需要的位元組數（含 NUL）。
                    const auto chars=host("kernel32.dll","GetCurrentDirectoryW",{word(0)/2,call(1)});
                    co_return finish(std::uint64_t{chars}*2);
                }
                if(name=="RtlSetCurrentDirectory_U" || name=="RtlDoesFileExists_U"){
                    std::u16string text;
                    if(name=="RtlSetCurrentDirectory_U"){if(!call(0))co_return finish(access_violation);text=counted(call(0));}
                    else {if(!call(0))co_return finish(0);text=read_utf16(memory,GuestAddress{call(0)},32767);}
                    if(!allocate(text.size()*2+2))co_return finish(no_memory);memory.write(GuestAddress{scratch},utf16_bytes(text));
                    if(name=="RtlDoesFileExists_U")co_return finish(host("kernel32.dll","GetFileAttributesW",{scratch})!=UINT32_MAX?1U:0U);
                    const auto ok=host("kernel32.dll","SetCurrentDirectoryW",{scratch});
                    co_return finish(ok?0U:last_error_==2 || last_error_==3?0xc000003aU:nt_status(last_error_));
                }
                if(name=="RtlDosSearchPath_U"){
                    // (Path, FileName, Extension, ULONG 位元組數, Buffer, FilePart*)：與 SearchPathW 同一流程，長度改以位元組計。
                    const auto chars=host("kernel32.dll","SearchPathW",{call(0),call(1),call(2),word(3)/2,call(4),call(5)});
                    co_return finish(std::uint64_t{chars}*2);
                }
                if(name=="RtlQueryEnvironmentVariable_U" || name=="RtlQueryEnvironmentVariable"){
                    // _U：(Environment, UNICODE_STRING* Name, UNICODE_STRING* Value)；新版：(Environment, Name, NameLength, Value, ValueLength, ReturnLength*)，長度以字元計。
                    const bool counted_form=name.ends_with("_U");std::u16string key;
                    if(counted_form){if(!call(1) || !call(2))co_return finish(access_violation);key=counted(call(1));}
                    else {if(!call(1))co_return finish(access_violation);key=read_utf16_counted(memory,GuestAddress{call(1)},static_cast<std::size_t>(std::min<std::uint64_t>(call(2),32767)));}
                    std::optional<std::u16string> value;
                    if(call(0)){
                        // 明確的環境區塊：NAME=VALUE\0 … \0\0；名稱比對不分大小寫，略過以 = 開頭的磁碟目錄項目以外的空名稱。
                        auto at=call(0);
                        for(unsigned guard=0;guard<65536;++guard){
                            const auto entry=read_utf16(memory,GuestAddress{at},32767);if(entry.empty())break;
                            const auto equal=entry.find(u'=',1);
                            if(equal!=entry.npos && equal==key.size() && std::equal(key.begin(),key.end(),entry.begin(),[](auto a,auto b){return windows_path_upper(a)==windows_path_upper(b);})){value=entry.substr(equal+1);break;}
                            at+=(entry.size()+1)*2;
                        }
                    }else {
                        if(!allocate(key.size()*2+2+65536))co_return finish(no_memory);memory.write(GuestAddress{scratch},utf16_bytes(key));
                        last_error_=0;const auto output=scratch+key.size()*2+2;
                        const auto chars=host("kernel32.dll","GetEnvironmentVariableW",{scratch,output,32767});
                        if(chars || !last_error_)value=read_utf16_counted(memory,GuestAddress{output},chars);
                    }
                    if(!value)co_return finish(0xc0000100U); // STATUS_VARIABLE_NOT_FOUND
                    if(counted_form){
                        const auto bytes=value->size()*2;if(bytes>0xfffe)co_return finish(buffer_too_small);
                        string_header(call(2),static_cast<std::uint16_t>(bytes));
                        if(bytes>string_capacity(call(2)))co_return finish(buffer_too_small);
                        const auto buffer=string_buffer(call(2));auto data=utf16_bytes(*value);data.resize(bytes);memory.write(GuestAddress{buffer},data);
                        if(bytes+2<=string_capacity(call(2)))memory.store(GuestAddress{buffer+bytes},16,0);
                        co_return finish(0);
                    }
                    if(call(5))memory.store(GuestAddress{call(5)},bits,value->size());
                    if(value->size()>=call(4))co_return finish(buffer_too_small);
                    memory.write(GuestAddress{call(3)},utf16_bytes(*value));co_return finish(0);
                }
                if(name=="RtlSetEnvironmentVariable"){
                    // (PWSTR* Environment, UNICODE_STRING* Name, UNICODE_STRING* Value)：Value 為 NULL 時刪除。Environment 為 NULL 時改目前程序環境。
                    if(!call(1))co_return finish(access_violation);
                    const auto key=counted(call(1));if(key.empty() || key.find(u'=',1)!=key.npos)co_return finish(invalid_parameter);
                    const std::optional<std::u16string> value=call(2)?std::optional{counted(call(2))}:std::nullopt;
                    const auto parameters=memory.load(GuestAddress{peb_base_.value+(x86_?0x10U:0x20U)},bits);
                    const auto environment_field=parameters+detail::ProcessParameterLayout::for_x86(x86_).environment;
                    if(!call(0) || call(0)==environment_field){
                        if(!allocate(key.size()*2+2+(value?value->size()*2+2:0)))co_return finish(no_memory);
                        memory.write(GuestAddress{scratch},utf16_bytes(key));const auto text=scratch+key.size()*2+2;
                        if(value)memory.write(GuestAddress{text},utf16_bytes(*value));
                        co_return finish(status_of(host("kernel32.dll","SetEnvironmentVariableW",{scratch,value?text:0})));
                    }
                    // 指定的環境區塊：重建後配置新區塊並釋放舊區塊（與 RtlCreateEnvironment 同一個 heap）。
                    const auto block=memory.load(GuestAddress{call(0)},bits);std::vector<std::u16string> entries;bool replaced=false;
                    for(auto at=block;at;){
                        const auto entry=read_utf16(memory,GuestAddress{at},32767);if(entry.empty())break;at+=(entry.size()+1)*2;
                        const auto equal=entry.find(u'=',1);
                        if(equal!=entry.npos && equal==key.size() && std::equal(key.begin(),key.end(),entry.begin(),[](auto a,auto b){return windows_path_upper(a)==windows_path_upper(b);})){
                            if(value && !replaced)entries.push_back(key+u"="+*value);replaced=true;continue;}
                        entries.push_back(entry);
                    }
                    if(value && !replaced)entries.push_back(key+u"="+*value);
                    std::vector<std::uint8_t> bytes;for(const auto& entry:entries){const auto chunk=utf16_bytes(entry);bytes.insert(bytes.end(),chunk.begin(),chunk.end());}
                    bytes.push_back(0);bytes.push_back(0);if(entries.empty()){bytes.push_back(0);bytes.push_back(0);}
                    const auto created=allocate_heap(memory,bytes.size());if(!created)co_return finish(no_memory);
                    memory.write(GuestAddress{created},bytes);memory.store(GuestAddress{call(0)},bits,created);
                    if(block)(void)free_heap(memory,block,state);
                    co_return finish(0);
                }
                if(name=="RtlExpandEnvironmentStrings_U" || name=="RtlExpandEnvironmentStrings"){
                    // _U：(Environment, Source UNICODE_STRING*, Destination UNICODE_STRING*, ReturnedLength 位元組*)；
                    // 新版：(Environment, Source, SourceLength, Destination, DestinationLength, ReturnLength*)，長度以字元計。只支援目前程序環境。
                    if(call(0))co_return finish(0xc00000bbU);
                    const bool counted_form=name.ends_with("_U");std::u16string source;
                    if(counted_form){if(!call(1) || !call(2))co_return finish(access_violation);source=counted(call(1));}
                    else source=read_utf16_counted(memory,GuestAddress{call(1)},static_cast<std::size_t>(std::min<std::uint64_t>(call(2),32767)));
                    if(!allocate(source.size()*2+2+65536))co_return finish(no_memory);memory.write(GuestAddress{scratch},utf16_bytes(source));
                    const auto output=scratch+source.size()*2+2;
                    const auto chars=host("kernel32.dll","ExpandEnvironmentStringsW",{scratch,output,32767});
                    if(!chars)co_return finish(nt_status(last_error_));
                    const auto expanded=read_utf16_counted(memory,GuestAddress{output},chars-1);
                    if(counted_form){
                        if(call(3))memory.store(GuestAddress{call(3)},32,chars*2);
                        const auto bytes=expanded.size()*2;if(bytes+2>string_capacity(call(2)))co_return finish(buffer_too_small);
                        memory.write(GuestAddress{string_buffer(call(2))},utf16_bytes(expanded));string_header(call(2),static_cast<std::uint16_t>(bytes));co_return finish(0);
                    }
                    if(call(5))memory.store(GuestAddress{call(5)},bits,chars);
                    if(chars>call(4))co_return finish(buffer_too_small);
                    memory.write(GuestAddress{call(3)},utf16_bytes(expanded));co_return finish(0);
                }
                if(name=="RtlCreateEnvironment"){
                    // (BOOLEAN Inherit, PWSTR* Environment)：Inherit 時複製目前程序環境，否則建立空白區塊。
                    if(!call(1))co_return finish(access_violation);
                    const auto bytes=(call(0)&0xffU)?environment_snapshot():std::vector<std::uint8_t>{0,0,0,0};
                    const auto created=allocate_heap(memory,bytes.size());if(!created)co_return finish(no_memory);
                    memory.write(GuestAddress{created},bytes);memory.store(GuestAddress{call(1)},bits,created);co_return finish(0);
                }
                if(name=="RtlSetCurrentEnvironment"){
                    // 新區塊直接成為目前環境；有輸出指標時將舊區塊所有權交回呼叫端。
                    if(!call(0))co_return finish(invalid_parameter);
                    std::uint64_t previous=0;
                    if(call(1))memory.check(GuestAddress{call(1)},unit,Access::Write);
                    const auto bytes=detail::capture_native_environment(memory,call(0));
                    const auto error=replace_process_environment(memory,state,bytes,call(0),call(1)?&previous:nullptr);
                    if(error)co_return finish(nt_status(error));
                    if(call(1))memory.store(GuestAddress{call(1)},bits,previous);
                    co_return finish(0);
                }
                if(name=="RtlDefaultNpAcl"){
                    // (PACL* Acl)：與 CreateNamedPipe 未帶安全描述元時相同的預設 DACL，以程序 heap 配置（呼叫端以 RtlFreeHeap 釋放）。
                    if(!call(0))co_return finish(access_violation);
                    initialize_tokens();const auto acl=default_pipe_acl(runtime_.token_objects.at(effective_token_id()).owner);
                    const auto block=allocate_heap(memory,acl.size());if(!block)co_return finish(no_memory);
                    memory.write(GuestAddress{block},acl);memory.store(GuestAddress{call(0)},bits,block);co_return finish(0);
                }
                if(name=="RtlDestroyEnvironment"){
                    if(call(0) && !free_heap(memory,call(0),state))co_return finish(invalid_parameter);co_return finish(0);
                }
                if(name=="RtlValidRelativeSecurityDescriptor"){
                    // (Input, Length, RequiredInformation)：整個自我相對描述元須落在 Length 內，且具備要求的擁有者／群組／DACL／SACL；回 BOOLEAN。
                    const auto length=static_cast<std::uint32_t>(call(1)),required=static_cast<std::uint32_t>(call(2));
                    if(!call(0) || length<20 || length>262144)co_return finish(0);
                    std::vector<std::uint8_t> bytes(length);memory.read(GuestAddress{call(0)},bytes);
                    try{
                        const auto sd=security::parse_relative_descriptor(bytes);
                        const bool valid=(!(required&1U) || !sd.data[0].empty()) && (!(required&2U) || !sd.data[1].empty()) &&
                            (!(required&4U) || (sd.control&4U)) && (!(required&8U) || (sd.control&16U));
                        co_return finish(valid?1U:0U);
                    }catch(const SecurityDescriptorError&){co_return finish(0);}
                }
                if(name=="RtlCopySecurityDescriptor"){
                    // (Source, PSECURITY_DESCRIPTOR* Destination)：配置同長度的自我相對複本（以程序 heap 配置，由 RtlFreeHeap 釋放）。
                    if(!call(0) || !call(1))co_return finish(access_violation);
                    const auto length=host("advapi32.dll","GetSecurityDescriptorLength",{call(0)});
                    if(!length)co_return finish(invalid_parameter);
                    const auto created=allocate_heap(memory,length);if(!created)co_return finish(no_memory);
                    const auto control=memory.load(GuestAddress{call(0)+2},16);
                    if(control&0x8000U){std::vector<std::uint8_t> bytes(length);memory.read(GuestAddress{call(0)},bytes);memory.write(GuestAddress{created},bytes);}
                    else {
                        if(!allocate(8)){(void)free_heap(memory,created,state);co_return finish(no_memory);}
                        memory.store(GuestAddress{scratch},32,length);
                        if(!host("advapi32.dll","MakeSelfRelativeSD",{call(0),created,scratch})){const auto error=last_error_;(void)free_heap(memory,created,state);co_return finish(nt_status(error));}
                    }
                    memory.store(GuestAddress{call(1)},bits,created);co_return finish(0);
                }
            }catch(const NativeError& error){co_return finish(error.status);}
            catch(const detail::NativeProcessError& error){co_return finish(error.status);}
            catch(const std::bad_alloc&){co_return finish(no_memory);}
            catch(const MemoryFault&){co_return finish(access_violation);}
            throw HostCallFault("未知的 Rtl 轉接服務");
        });
}
