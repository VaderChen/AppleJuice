// 包含於 windows_nt_native.cpp：權杖、程序、Job、Atom、地區設定、電源與雜項 NT 系統服務（ntdll 的 Nt／Zw 匯出與直接系統呼叫共用）。
// 語義交給既有 Win32 實作（同一份權杖、Job、Atom、程序與等待物件狀態），這一層只轉換 OBJECT_ATTRIBUTES、
// 參數結構與 NTSTATUS；Win32 呼叫的 last error 不外漏給呼叫者。
namespace {
constexpr std::string_view nt_family_symbols[]{
    "NtQueryInformationToken","NtSetInformationToken","NtAdjustPrivilegesToken","NtAdjustGroupsToken","NtDuplicateToken",
    "NtFilterToken","NtCompareTokens","NtPrivilegeCheck","NtImpersonateAnonymousToken","NtAllocateLocallyUniqueId","NtAllocateUuids",
    "NtSetInformationProcess","NtIsProcessInJob","NtCreateJobObject","NtOpenJobObject","NtAssignProcessToJobObject",
    "NtTerminateJobObject","NtQueryInformationJobObject","NtSetInformationJobObject",
    "NtAddAtom","NtFindAtom","NtDeleteAtom","NtQueryInformationAtom",
    "NtQueryDefaultLocale","NtQueryDefaultUILanguage","NtQueryInstallUILanguage",
    "NtPowerInformation","NtSetThreadExecutionState","NtPulseEvent","NtQuerySemaphore",
    "NtLockVirtualMemory","NtUnlockVirtualMemory","NtFlushBuffersFileEx","NtCancelSynchronousIoFile","NtQuerySystemInformationEx"};
// NtSetInformationProcess 的 PROCESS_PRIORITY_CLASS.PriorityClass（1～6）對應 Win32 優先權類別。
constexpr std::uint32_t nt_priority_classes[]{0,0x40,0x20,0x80,0x100,0x4000,0x8000};
}
bool nt_family_service(std::string_view symbol){return std::find(std::begin(nt_family_symbols),std::end(nt_family_symbols),symbol)!=std::end(nt_family_symbols);}
std::optional<std::uint32_t> WindowsServices::invoke_nt_family(GuestState& entry_state,GuestMemory& entry_memory,const WindowsServiceDescriptor& service){
    return nested_continuation(entry_state,entry_memory,"NtFamily",
        [this,service](GuestState& state,GuestMemory& memory)->service_callback::Task<std::optional<std::uint32_t>>{
            const WindowsCall call(state,memory,service);const auto name=service.symbol;const auto saved_error=last_error_;
            const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
            constexpr std::uint32_t invalid_parameter=0xc000000dU,invalid_handle=0xc0000008U,type_mismatch=0xc0000024U,access_violation=0xc0000005U,
                invalid_info_class=0xc0000003U,length_mismatch=0xc0000004U,no_memory=0xc0000017U;
            const auto finish=[&](std::uint32_t status){threads_.items.at(threads_.current).last_native_status=status;last_error_=saved_error;call.return_to(state,status);return std::optional<std::uint32_t>{};};
            // 經客體回呼續行時也直接進宿主服務入口，不經匯出改導向的客體快速常式。
            const auto kernel=[&](std::string_view symbol){const auto stub=host_service_stub("kernel32.dll",symbol);return stub.value?stub.value:resolve("kernel32.dll",symbol).value;};
            const auto word=[&](unsigned index){return static_cast<std::uint32_t>(call(index));};
            // 不會阻塞、也不需要客體回呼的 Win32 實作在宿主端直接執行（nested_service），不經客體；
            // Job 指派／終止／資訊（可能觸發程序終止輪詢）與檔案 I/O 取消、寫回仍走客體回呼續行。
            const auto host=[&](std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> args){return static_cast<std::uint32_t>(nested_service(state,memory,family,symbol,args));};
            const auto host_value=[&](std::string_view family,std::string_view symbol,std::initializer_list<std::uint64_t> args){return nested_service(state,memory,family,symbol,args);};
            // Win32 失敗碼轉 NTSTATUS；無效 handle 依 handle 是否存在區分「無效」與「物件型別不符」。
            const auto failure=[&](std::uint64_t handle){const auto error=last_error_;return error==6 && handle && !object_type(handle).empty()?type_mismatch:nt_status(error);};
            // 暫存區：scratch 為最近一次配置的區塊（共用 GuestScratch 管理生命週期）。
            GuestScratch temporary(*this,memory,state);std::uint64_t scratch=0;
            const auto allocate=[&](std::size_t size)->bool{scratch=temporary.allocate(size);return scratch!=0;};
            try{
                if(name=="NtQueryInformationToken" || name=="NtSetInformationToken"){
                    const bool query=name=="NtQueryInformationToken";
                    if(query && !call(4))co_return finish(access_violation);
                    // 長度輸出走暫存格：Win32 版在緩衝區不足時同樣寫回需要的長度。
                    if(!allocate(8))co_return finish(no_memory);
                    const auto ok=query?host("advapi32.dll","GetTokenInformation",{call(0),word(1),call(2),word(3),scratch}):
                        host("advapi32.dll","SetTokenInformation",{call(0),word(1),call(2),word(3)});
                    if(query)memory.store(GuestAddress{call(4)},32,memory.load(GuestAddress{scratch},32));
                    if(ok)co_return finish(0);
                    // 87：未知類別、主要權杖查詢模擬層級回 STATUS_INVALID_INFO_CLASS，其餘是 NULL 緩衝區。
                    const auto error=last_error_,kind=word(1);
                    if(error==87)co_return finish(kind==0 || kind>=50 || kind==9 || !query?invalid_info_class:access_violation);
                    co_return finish(error==24?length_mismatch:failure(call(0)));
                }
                if(name=="NtAdjustPrivilegesToken" || name=="NtAdjustGroupsToken"){
                    // ReturnLength 只在 PreviousState 提供時必填；與 Win32 版相同，緩衝區不足時不套用任何變更。
                    const bool groups=name=="NtAdjustGroupsToken";
                    const auto ok=host("advapi32.dll",groups?"AdjustTokenGroups":"AdjustTokenPrivileges",{call(0),call(1)&0xffU,call(2),word(3),call(4),call(5)});
                    if(!ok)co_return finish(failure(call(0)));
                    co_return finish(last_error_==1300?0x106U:0U); // STATUS_NOT_ALL_ASSIGNED 是成功碼
                }
                if(name=="NtDuplicateToken"){
                    // (Existing, DesiredAccess, ObjectAttributes, EffectiveOnly, TokenType, NewToken)
                    // 模擬層級取自 ObjectAttributes.SecurityQualityOfService；沒有時為 SecurityAnonymous。
                    if(!call(5))co_return finish(access_violation);memory.check(GuestAddress{call(5)},unit,Access::Write);
                    const auto type=word(4);if(type!=1 && type!=2)co_return finish(invalid_parameter);
                    std::uint32_t level=0;std::uint64_t security=0;
                    if(const auto attributes=call(2)){
                        if(memory.load(GuestAddress{attributes},32)!=(x86_?24U:48U))co_return finish(invalid_parameter);
                        if(memory.load(GuestAddress{attributes+2*unit},bits))co_return finish(0xc0000033U); // Token 沒有名稱
                        security=memory.load(GuestAddress{attributes+4*unit},bits);
                        if(const auto qos=memory.load(GuestAddress{attributes+5*unit},bits)){
                            if(memory.load(GuestAddress{qos},32)!=12)co_return finish(invalid_parameter);
                            level=static_cast<std::uint32_t>(memory.load(GuestAddress{qos+4},32));if(level>3)co_return finish(invalid_parameter);
                        }
                    }
                    if(!allocate(64))co_return finish(no_memory);
                    std::uint64_t descriptor=0;
                    if(security){memory.store(GuestAddress{scratch},32,x86_?12U:24U);memory.store(GuestAddress{scratch+unit},bits,security);descriptor=scratch;}
                    if(!host("advapi32.dll","DuplicateTokenEx",{call(0),word(1),descriptor,level,type,scratch+32}))co_return finish(failure(call(0)));
                    const auto handle=memory.load(GuestAddress{scratch+32},bits);
                    if(call(3)&0xffU){
                        // EffectiveOnly：新權杖只保留已啟用的群組與特權（新權杖尚未共用，直接修改其物件）。
                        auto& token=runtime_.token_objects.at(runtime_.token_handle_objects.at(handle));
                        std::erase_if(token.groups,[](const auto& group){return !(group.attributes&4U);});
                        std::erase_if(token.privileges,[](const auto& item){return !(item.attributes&2U);});++token.modified_id;
                    }
                    memory.store(GuestAddress{call(5)},bits,handle);co_return finish(0);
                }
                if(name=="NtFilterToken"){
                    // (Existing, Flags, SidsToDisable TOKEN_GROUPS*, PrivilegesToDelete TOKEN_PRIVILEGES*, RestrictedSids TOKEN_GROUPS*, NewToken)
                    // TOKEN_GROUPS／TOKEN_PRIVILEGES 的計數與陣列直接交給 CreateRestrictedToken 的 SID_AND_ATTRIBUTES／LUID_AND_ATTRIBUTES 參數。
                    if(!call(5))co_return finish(access_violation);
                    const auto groups=[&](std::uint64_t list,std::uint64_t& count){count=list?memory.load(GuestAddress{list},32):0;return list && count?list+unit:0;};
                    std::uint64_t disable_count=0,restrict_count=0,delete_count=0;
                    const auto disable=groups(call(2),disable_count),restrict=groups(call(4),restrict_count);
                    if(call(3))delete_count=memory.load(GuestAddress{call(3)},32);
                    if(!allocate(16))co_return finish(no_memory);
                    if(!host("advapi32.dll","CreateRestrictedToken",{call(0),word(1),disable_count,disable,delete_count,delete_count?call(3)+4:0,restrict_count,restrict,scratch}))
                        co_return finish(failure(call(0)));
                    memory.store(GuestAddress{call(5)},bits,memory.load(GuestAddress{scratch},bits));co_return finish(0);
                }
                if(name=="NtCompareTokens"){
                    // 兩個權杖都需 TOKEN_QUERY；SID 集合（使用者與群組，含啟用狀態）、特權與限制 SID 相同即視為相等。
                    initialize_tokens();
                    const auto token=[&](std::uint64_t handle,std::uint32_t& status)->const VirtualToken*{
                        const auto access=runtime_.token_handles.find(handle);
                        if(access==runtime_.token_handles.end()){status=object_type(handle).empty()?invalid_handle:type_mismatch;return nullptr;}
                        if(!(access->second&8U)){status=0xc0000022U;return nullptr;}
                        return &runtime_.token_objects.at(runtime_.token_handle_objects.at(handle));
                    };
                    std::uint32_t status=0;const auto* first=token(call(0),status);if(!first)co_return finish(status);
                    const auto* second=token(call(1),status);if(!second)co_return finish(status);
                    if(!call(2))co_return finish(access_violation);
                    const auto sids=[](const VirtualToken& value){
                        std::vector<std::pair<std::vector<std::uint8_t>,bool>> list{{value.user.sid,true}};
                        for(const auto& group:value.groups)list.emplace_back(group.sid,(group.attributes&4U)!=0);
                        std::sort(list.begin(),list.end());return list;};
                    const auto privileges=[](const VirtualToken& value){
                        std::vector<std::pair<std::uint64_t,bool>> list;for(const auto& item:value.privileges)list.emplace_back(item.luid,(item.attributes&2U)!=0);
                        std::sort(list.begin(),list.end());return list;};
                    const auto restricted=[](const VirtualToken& value){
                        std::vector<std::vector<std::uint8_t>> list;for(const auto& item:value.restricted)list.push_back(item.sid);std::sort(list.begin(),list.end());return list;};
                    const bool equal=sids(*first)==sids(*second) && privileges(*first)==privileges(*second) && restricted(*first)==restricted(*second);
                    memory.store(GuestAddress{call(2)},8,equal?1U:0U);co_return finish(0);
                }
                if(name=="NtPrivilegeCheck"){
                    if(!call(2))co_return finish(access_violation);if(!allocate(8))co_return finish(no_memory);
                    if(!host("advapi32.dll","PrivilegeCheck",{call(0),call(1),scratch}))co_return finish(failure(call(0)));
                    memory.store(GuestAddress{call(2)},8,memory.load(GuestAddress{scratch},32)?1U:0U);co_return finish(0);
                }
                if(name=="NtImpersonateAnonymousToken"){
                    if(!host("advapi32.dll","ImpersonateAnonymousToken",{call(0)}))co_return finish(failure(call(0)));co_return finish(0);
                }
                if(name=="NtAllocateLocallyUniqueId"){
                    if(!call(0))co_return finish(access_violation);
                    if(!host("advapi32.dll","AllocateLocallyUniqueId",{call(0)}))co_return finish(nt_status(last_error_));co_return finish(0);
                }
                if(name=="NtAllocateUuids"){
                    // (Time ULARGE_INTEGER*, Range ULONG*, Sequence ULONG*, Seed UCHAR[6]*)：以 1582-10-15 起的 100 ns 時間保留一段
                    // 時間版 UUID 區間；同一程序內單調遞增，時間停滯時往後推，序號在時鐘倒退時遞增。
                    if(!call(0) || !call(1) || !call(2))co_return finish(access_violation);
                    constexpr std::uint64_t gregorian_offset=0x01b21dd213814000ULL; // 1582-10-15 到 1601-01-01 的 100 ns 數
                    auto now=runtime_.system_filetime()+gregorian_offset;
                    if(now<=uuid_clock_.last){if(now+10000<uuid_clock_.last)++uuid_clock_.sequence;now=uuid_clock_.last+1;}
                    uuid_clock_.last=now;const std::uint32_t range=1;
                    memory.store(GuestAddress{call(0)},64,now);memory.store(GuestAddress{call(1)},32,range);
                    memory.store(GuestAddress{call(2)},32,uuid_clock_.sequence&0x3fffU);
                    if(call(3)){
                        // 節點：無網路卡身分時以本程序的隨機節點（multicast 位元設定，與 RFC 4122 的隨機節點相同）。
                        if(!uuid_clock_.node_ready){std::uint64_t seed=now^(std::uint64_t{process_id()}<<32);for(auto& byte:uuid_clock_.node){seed=seed*6364136223846793005ULL+1442695040888963407ULL;byte=static_cast<std::uint8_t>(seed>>56);}uuid_clock_.node[0]|=1;uuid_clock_.node_ready=true;}
                        memory.write(GuestAddress{call(3)},std::vector<std::uint8_t>(uuid_clock_.node.begin(),uuid_clock_.node.end()));
                    }
                    co_return finish(0);
                }
                if(name=="NtSetInformationProcess"){
                    const auto kind=word(1),length=word(3);const auto buffer=call(2);
                    if(trace_native_call(name)){
                        // 僅在選定的診斷模式讀取有界輸入；無效指標不得改變原 API 的錯誤順序。
                        std::fprintf(stderr,"[process_information_input] pid=%u class=%u length=%u bytes=",process_id(),kind,length);
                        try{std::array<std::uint8_t,32> bytes{};const auto count=std::min<std::size_t>(length,bytes.size());
                            if(count)memory.read(GuestAddress{buffer},std::span<std::uint8_t>(bytes.data(),count));
                            for(std::size_t n=0;n<count;++n)std::fprintf(stderr,"%02x",unsigned(bytes[n]));
                        }catch(const MemoryFault&){std::fprintf(stderr,"unreadable");}
                        std::fprintf(stderr,"\n");
                    }
                    const auto need=[&](std::uint32_t size){return length==size;};
                    switch(kind){
#ifdef X64ARM_HOST_SERVICES_MEMORY_WATCH
                    case 15:case 42:{
                        if(length)co_return finish(length_mismatch);
                        std::uint32_t returned{};const auto error=process_working_set_watch(memory,call(0),true,kind==42,0,0,returned,true);
                        co_return finish(error==6 && !object_type(call(0)).empty()?type_mismatch:working_set_watch_status(error));}
#endif
                    case 40:{ // ProcessInstrumentationCallback；32 位元的原生 ABI 接收指標。
                        if(x86_?length<4:(length!=8 && length!=16))co_return finish(length_mismatch);
                        memory.check(GuestAddress{buffer},length,Access::Read);
                        if(!x86_ && length==16 && (memory.load(GuestAddress{buffer},32) || memory.load(GuestAddress{buffer+4},32)))
                            co_return finish(invalid_parameter);
                        const auto callback=memory.load(GuestAddress{buffer+(!x86_ && length==16?8U:0U)},bits);
                        std::uint32_t pid{},target_bits{};std::uint64_t birth{};
                        auto error=process_memory_target(memory,call(0),0x200U,pid,birth);
                        if(error==6 && !object_type(call(0)).empty())co_return finish(type_mismatch);
                        std::array<std::uint64_t,5> values{callback,bits};
                        if(!error)error=pid==process_id()?local_process_information(15,values,target_bits,-1):
                            remote_process_request(memory,pid,birth,14,15,values,target_bits);
                        co_return finish(nt_status(error));}
                    case 12:{ // ProcessDefaultHardErrorMode：NT 值的第 0 位元與 SEM_FAILCRITICALERRORS 相反
                        if(!need(4))co_return finish(length_mismatch);
                        (void)host("kernel32.dll","SetErrorMode",{static_cast<std::uint32_t>(memory.load(GuestAddress{buffer},32))^1U});co_return finish(0);}
                    case 18:{ // ProcessPriorityClass：{BOOLEAN Foreground; UCHAR PriorityClass}
                        if(!need(2))co_return finish(length_mismatch);
                        const auto value=memory.load(GuestAddress{buffer+1},8);if(!value || value>6)co_return finish(invalid_parameter);
                        if(!host("kernel32.dll","SetPriorityClass",{call(0),nt_priority_classes[value]}))co_return finish(failure(call(0)));co_return finish(0);}
                    case 21:{ // ProcessAffinityMask：KAFFINITY
                        if(!need(unit))co_return finish(length_mismatch);
                        if(!host("kernel32.dll","SetProcessAffinityMask",{call(0),memory.load(GuestAddress{buffer},bits)}))co_return finish(failure(call(0)));co_return finish(0);}
                    case 22:{ // ProcessPriorityBoost：ULONG DisableBoost
                        if(!need(4))co_return finish(length_mismatch);
                        if(!host("kernel32.dll","SetProcessPriorityBoost",{call(0),memory.load(GuestAddress{buffer},32)!=0}))co_return finish(failure(call(0)));co_return finish(0);}
                    case 39:case 77:{ // ProcessPagePriority→ProcessMemoryPriority、ProcessPowerThrottlingState→ProcessPowerThrottling
                        if(!need(kind==39?4U:12U))co_return finish(length_mismatch);
                        if(!host("kernel32.dll","SetProcessInformation",{call(0),kind==39?0U:4U,buffer,length}))co_return finish(failure(call(0)));co_return finish(0);}
                    case 52:{ // ProcessMitigationPolicy：{PROCESS_MITIGATION_POLICY Policy; ULONG Flags}，只適用目前程序
                        if(!need(8))co_return finish(length_mismatch);
                        std::uint32_t pid{};std::uint64_t birth{};
                        if(const auto error=process_memory_target(memory,call(0),0x200U,pid,birth))co_return finish(error==6 && !object_type(call(0)).empty()?type_mismatch:nt_status(error));
                        if(pid!=process_id())co_return finish(invalid_parameter);
                        if(!host("kernel32.dll","SetProcessMitigationPolicy",{memory.load(GuestAddress{buffer},32),buffer+4,4}))co_return finish(nt_status(last_error_));co_return finish(0);}
                    default:co_return finish(invalid_info_class);
                    }
                }
                if(name=="NtIsProcessInJob"){
                    if(!allocate(8))co_return finish(no_memory);
                    if(!host("kernel32.dll","IsProcessInJob",{call(0),call(1),scratch}))co_return finish(failure(call(0)));
                    co_return finish(memory.load(GuestAddress{scratch},32)?0x123U:0x124U); // STATUS_PROCESS_IN_JOB／NOT_IN_JOB
                }
                if(name=="NtCreateJobObject" || name=="NtOpenJobObject"){
                    const bool creating=name=="NtCreateJobObject";
                    if(!call(0))co_return finish(access_violation);memory.check(GuestAddress{call(0)},unit,Access::Write);
                    Win32ObjectRequest request;if(const auto status=win32_object_request(memory,temporary,runtime_,call(2),x86_,creating,request))co_return finish(status);
                    const auto text=request.name;
                    auto handle=creating?host_value("kernel32.dll","CreateJobObjectW",{request.security,text}):
                        host_value("kernel32.dll","OpenJobObjectW",{word(1),request.inherit()?1U:0U,text});
                    if(x86_)handle&=UINT32_MAX;
                    if(!handle)co_return finish(last_error_==6?type_mismatch:nt_status(last_error_));
                    if(creating && last_error_==183){
                        if(!(request.flags&0x80U)){(void)host("kernel32.dll","CloseHandle",{handle});co_return finish(0xc0000035U);}
                        memory.store(GuestAddress{call(0)},bits,handle);co_return finish(0x40000000U);
                    }
                    memory.store(GuestAddress{call(0)},bits,handle);co_return finish(0);
                }
                if(name=="NtAssignProcessToJobObject"){
                    if(!co_await service_callback::call(kernel("AssignProcessToJobObject"),{call(0),call(1)}))co_return finish(failure(call(0)));co_return finish(0);
                }
                if(name=="NtTerminateJobObject"){
                    if(!co_await service_callback::call(kernel("TerminateJobObject"),{call(0),word(1)}))co_return finish(failure(call(0)));co_return finish(0);
                }
                if(name=="NtQueryInformationJobObject" || name=="NtSetInformationJobObject"){
                    const bool query=name=="NtQueryInformationJobObject";
                    if(!allocate(8))co_return finish(no_memory);
                    const auto ok=query?co_await service_callback::call(kernel("QueryInformationJobObject"),{call(0),word(1),call(2),word(3),scratch}):
                        co_await service_callback::call(kernel("SetInformationJobObject"),{call(0),word(1),call(2),word(3)});
                    if(query && call(4))memory.store(GuestAddress{call(4)},32,memory.load(GuestAddress{scratch},32));
                    if(ok)co_return finish(0);
                    const auto error=last_error_;co_return finish(error==24 || error==122?length_mismatch:error==87 && !call(2)?access_violation:failure(call(0)));
                }
                if(name=="NtAddAtom" || name=="NtFindAtom"){
                    // (PWSTR Name, ULONG Length 以 bytes 計, RTL_ATOM* Atom)：名稱不一定以 NUL 結尾，複製後交給全域 Atom 表。
                    const auto length=word(1);if(length%2 || length>255*2)co_return finish(invalid_parameter);
                    if(!length)co_return finish(0xc0000033U);
                    if(!call(0))co_return finish(access_violation);
                    if(!allocate(length+2))co_return finish(no_memory);
                    std::vector<std::uint8_t> text(length);memory.read(GuestAddress{call(0)},text);memory.write(GuestAddress{scratch},text);
                    const auto atom=host("kernel32.dll",name=="NtAddAtom"?"GlobalAddAtomW":"GlobalFindAtomW",{scratch});
                    if(!atom)co_return finish(name=="NtFindAtom" && (last_error_==2 || last_error_==0)?0xc0000034U:nt_status(last_error_));
                    if(call(2))memory.store(GuestAddress{call(2)},16,atom&0xffffU);co_return finish(0);
                }
                if(name=="NtDeleteAtom"){
                    // GlobalDeleteAtom 成功回 0；失敗時回原 atom 並設定 last error。
                    const auto atom=static_cast<std::uint16_t>(call(0));last_error_=0;
                    const auto remaining=host("kernel32.dll","GlobalDeleteAtom",{atom});
                    if(!remaining && !last_error_)co_return finish(0);
                    co_return finish(last_error_==87 || last_error_==6 || !last_error_?invalid_handle:nt_status(last_error_));
                }
                if(name=="NtQueryInformationAtom"){
                    // AtomBasicInformation（0）：{USHORT UsageCount; USHORT Flags; USHORT NameLength; WCHAR Name[]}；整數 atom 為固定（RTL_ATOM_PINNED）。
                    // AtomTableInformation（1）：{ULONG NumberOfAtoms; RTL_ATOM Atoms[]}。名稱、計數與 Global*Atom 共用同一份全域表。
                    const auto atom=static_cast<std::uint16_t>(call(0));const auto kind=word(1),length=word(3);const auto atom_job=job_atom_table();
                    if(kind==1){
                        std::vector<std::uint16_t> atoms;if(!atom_job)for(const auto& [id,item]:gui_.global_atoms){(void)item;atoms.push_back(id);}
                        const auto needed=static_cast<std::uint32_t>(4+atoms.size()*2);
                        if(call(4))memory.store(GuestAddress{call(4)},32,needed);
                        if(length<needed)co_return finish(length_mismatch);
                        memory.store(GuestAddress{call(2)},32,atoms.size());
                        for(std::size_t i=0;i<atoms.size();++i)memory.store(GuestAddress{call(2)+4+i*2},16,atoms[i]);co_return finish(0);
                    }
                    if(kind!=0)co_return finish(invalid_info_class);
                    if(!atom)co_return finish(invalid_parameter);
                    if(length<6)co_return finish(length_mismatch);
                    std::u16string text;std::uint32_t usage=1,flags=0;
                    if(atom<0xc000){text=u"#";for(const auto c:std::to_string(atom))text+=static_cast<char16_t>(c);flags=1;}
                    else if(atom_job){std::uint32_t error=0;text=atom_job->global_atom_name(atom,error);if(error)co_return finish(invalid_handle);}
                    else {const auto found=gui_.global_atoms.find(atom);if(found==gui_.global_atoms.end())co_return finish(invalid_handle);text=found->second.name;usage=found->second.references;}
                    const auto capacity=(length-6)/2;const auto copied=static_cast<std::uint32_t>(std::min<std::size_t>(text.size(),capacity));
                    memory.store(GuestAddress{call(2)},16,std::min<std::uint32_t>(usage,0xffffU));memory.store(GuestAddress{call(2)+2},16,flags);
                    memory.store(GuestAddress{call(2)+4},16,copied*2);
                    auto name_bytes=utf16_bytes(text.substr(0,copied));name_bytes.resize(copied*2U); // utf16_bytes 附帶 NUL；NUL 只在有空間時另外寫入
                    memory.write(GuestAddress{call(2)+6},name_bytes);
                    if(copied<capacity)memory.store(GuestAddress{call(2)+6+copied*2},16,0);
                    if(call(4))memory.store(GuestAddress{call(4)},32,6+copied*2);
                    co_return finish(copied<text.size() || copied==capacity?0xc0000023U:0U);
                }
                if(name=="NtQueryDefaultLocale"){
                    if(!call(1))co_return finish(access_violation);
                    const auto lcid=host("kernel32.dll",(call(0)&0xffU)?"GetUserDefaultLCID":"GetSystemDefaultLCID",{});
                    memory.store(GuestAddress{call(1)},32,lcid);co_return finish(0);
                }
                if(name=="NtQueryDefaultUILanguage" || name=="NtQueryInstallUILanguage"){
                    if(!call(0))co_return finish(access_violation);
                    const auto language=host("kernel32.dll",name=="NtQueryDefaultUILanguage"?"GetUserDefaultUILanguage":"GetSystemDefaultUILanguage",{});
                    memory.store(GuestAddress{call(0)},16,language&0xffffU);co_return finish(0);
                }
                if(name=="NtPowerInformation"){
                    // 與 CallNtPowerInformation 同一個簽章與 NTSTATUS 契約。
                    co_return finish(host("powrprof.dll","CallNtPowerInformation",{word(0),call(1),word(2),call(3),word(4)}));
                }
                if(name=="NtSetThreadExecutionState"){
                    if(!call(1))co_return finish(access_violation);
                    const auto previous=host("kernel32.dll","SetThreadExecutionState",{word(0)});
                    if(!previous && word(0))co_return finish(invalid_parameter);
                    memory.store(GuestAddress{call(1)},32,previous);co_return finish(0);
                }
                if(name=="NtPulseEvent"){
                    const auto found=runtime_.wait_objects.find(call(0));
                    if(found==runtime_.wait_objects.end())co_return finish(object_type(call(0)).empty()?invalid_handle:type_mismatch);
                    if(found->second.kind!=WaitObject::Kind::Event)co_return finish(type_mismatch);
                    const bool previous=found->second.signaled();
                    if(!host("kernel32.dll","PulseEvent",{call(0)}))co_return finish(failure(call(0)));
                    if(call(1))memory.store(GuestAddress{call(1)},32,previous?1U:0U);co_return finish(0);
                }
                if(name=="NtQuerySemaphore"){
                    // SemaphoreBasicInformation：{LONG CurrentCount; LONG MaximumCount}；需要 SEMAPHORE_QUERY_STATE（1）。
                    const auto found=runtime_.wait_objects.find(call(0));
                    if(found==runtime_.wait_objects.end())co_return finish(object_type(call(0)).empty()?invalid_handle:type_mismatch);
                    if(found->second.kind!=WaitObject::Kind::Semaphore)co_return finish(type_mismatch);
                    if(word(1)!=0)co_return finish(invalid_info_class);if(word(3)!=8)co_return finish(length_mismatch);
                    if(!(found->second.access&1U))co_return finish(0xc0000022U);
                    // handle 與計數狀態分開保存；計數讀共用的等待狀態。
                    const auto& object=runtime_.wait_state(found->second);
                    memory.store(GuestAddress{call(2)},32,object.available_count());memory.store(GuestAddress{call(2)+4},32,object.maximum);
                    if(call(4))memory.store(GuestAddress{call(4)},32,8);co_return finish(0);
                }
                if(name=="NtLockVirtualMemory" || name=="NtUnlockVirtualMemory"){
                    // (Process, BaseAddress*, RegionSize*, MapType)：MAP_PROCESS（1）沿用 VirtualLock 的工作集鎖定；MAP_SYSTEM（2）需要驅動權限。
                    const bool lock=name=="NtLockVirtualMemory";const auto type=word(3);
                    if(type!=1 && type!=2)co_return finish(invalid_parameter);if(type==2)co_return finish(0xc0000061U);
                    std::uint32_t pid{};std::uint64_t birth{};
                    if(const auto error=process_memory_target(memory,call(0),0x8U,pid,birth))co_return finish(error==6 && !object_type(call(0)).empty()?type_mismatch:nt_status(error));
                    if(pid!=process_id())co_return finish(0xc00000bbU); // 其他程序的工作集鎖定尚未提供
                    if(!call(1) || !call(2))co_return finish(access_violation);
                    const auto base=memory.load(GuestAddress{call(1)},bits),size=memory.load(GuestAddress{call(2)},bits);
                    if(!host("kernel32.dll",lock?"VirtualLock":"VirtualUnlock",{base,size})){
                        const auto error=last_error_;co_return finish(error==158?0xc000002aU:error==487?0xc000002dU:error==1453?0xc00000a1U:nt_status(error));}
                    // 輸出頁對齊後的實際範圍。
                    const auto first=base&~UINT64_C(0xfff),last=(base+size+0xfff)&~UINT64_C(0xfff);
                    memory.store(GuestAddress{call(1)},bits,first);memory.store(GuestAddress{call(2)},bits,last-first);co_return finish(0);
                }
                if(name=="NtFlushBuffersFileEx"){
                    // FLUSH_FLAGS_FILE_DATA_ONLY（1）／NO_SYNC（2）／FILE_DATA_SYNC_ONLY（4）都需要把資料寫入；本實作一律完整寫回。
                    if(word(1)&~7U)co_return finish(invalid_parameter);
                    if(!call(4))co_return finish(access_violation);
                    if(!co_await service_callback::call(kernel("FlushFileBuffers"),{call(0)}))co_return finish(failure(call(0)));
                    memory.store(GuestAddress{call(4)},bits,0);memory.store(GuestAddress{call(4)+unit},bits,0);co_return finish(0);
                }
                if(name=="NtCancelSynchronousIoFile"){
                    if(!call(2))co_return finish(access_violation);
                    const auto ok=co_await service_callback::call(kernel("CancelSynchronousIo"),{call(0)});
                    const auto status=ok?0U:last_error_==1168?0xc0000225U:failure(call(0));
                    memory.store(GuestAddress{call(2)},bits,status);memory.store(GuestAddress{call(2)+unit},bits,0);co_return finish(status);
                }
                if(name=="NtQuerySystemInformationEx"){
                    // (Class, InputBuffer, InputLength, SystemInformation, Length, ReturnLength)
                    const auto kind=word(0),input_length=word(2),length=word(4);const auto output=call(3);
                    if(kind==107){
                        // SystemLogicalProcessorAndGroupInformation：輸入為 LOGICAL_PROCESSOR_RELATIONSHIP，輸出與 GetLogicalProcessorInformationEx 相同。
                        if(input_length!=4 || !call(1))co_return finish(invalid_parameter);
                        if(!allocate(8))co_return finish(no_memory);memory.store(GuestAddress{scratch},32,length);
                        const auto ok=host("kernel32.dll","GetLogicalProcessorInformationEx",{memory.load(GuestAddress{call(1)},32),output,scratch});
                        if(call(5))memory.store(GuestAddress{call(5)},32,memory.load(GuestAddress{scratch},32));
                        if(ok)co_return finish(0);co_return finish(last_error_==122?length_mismatch:nt_status(last_error_));
                    }
                    if(kind==175){
                        // SystemCpuSetInformation：輸入為程序 handle（可省略）。
                        const auto process=input_length>=unit && call(1)?memory.load(GuestAddress{call(1)},bits):0;
                        if(input_length && input_length!=unit)co_return finish(invalid_parameter);
                        if(!allocate(8))co_return finish(no_memory);
                        const auto ok=host("kernel32.dll","GetSystemCpuSetInformation",{output,length,scratch,process,0});
                        if(call(5))memory.store(GuestAddress{call(5)},32,memory.load(GuestAddress{scratch},32));
                        if(ok)co_return finish(0);co_return finish(last_error_==122?0xc0000023U:nt_status(last_error_));
                    }
                    co_return finish(invalid_info_class);
                }
            }catch(const NativeError& error){co_return finish(error.status);}
            catch(const MemoryFault&){co_return finish(access_violation);}
            throw HostCallFault("未知的 NT 家族服務");
        });
}
