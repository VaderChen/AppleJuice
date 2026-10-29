// 此檔於 x64arm namespace 內引入。只有登錄 raw I/O 尚未完成時才查詢，
// 不放入正常服務熱路徑，也不讀取客體參數或修改服務／客體狀態。
namespace {
bool registry_dependent_service(const WindowsServiceDescriptor& service) {
    const auto family=service.family,name=service.symbol;
    const auto one_of=[](std::string_view value,std::initializer_list<std::string_view> names) {
        for(const auto candidate:names)if(value==candidate)return true;
        return false;
    };
    const auto contains=[&](std::span<const WindowsServiceDescriptor> descriptors) {
        for(const auto& candidate:descriptors) {
            if(candidate.family!=family)continue;
            if(candidate.symbol==name)return true;
            // 靜態匯入及動態解析可能保留 Zw 名稱；與相同 Nt provider 共用資格。
            if(family=="ntdll.dll" && name.starts_with("Zw") && candidate.symbol.starts_with("Nt") &&
               name.substr(2)==candidate.symbol.substr(2))return true;
        }
        return false;
    };

    // 串流供給、完成事件與既有 MMCSS 優先權操作不可因登錄 I/O 暫停。
    // 一般 COM 啟用另於下方分類；它在選擇內建音訊工廠前仍會查詢登錄。
    if(family=="internal-wasapi" || family=="mmdevapi.dll" || family=="audioses.dll" ||
       family=="internal-dsound" || family=="dsound.dll")return false;
    if(family=="avrt.dll")return one_of(name,{"AvSetMmThreadCharacteristicsA","AvSetMmThreadCharacteristicsW"});
    if(family=="winmm.dll")return name.starts_with("PlaySound") || name.starts_with("sndPlaySound");

    // 包含 Win32／NT 登錄、SHGetValue／SHDeleteKey 及依 guest service handle
    // 決定是否交易的服務控制 API，不以匯出名稱前綴取代實際 descriptor 家族。
    if(contains(registry_service_descriptors()) || contains(service_control_descriptors()))return true;

    const bool kernel=family=="kernel32.dll" || family=="kernelbase.dll";
    if(kernel) {
        // pending 交易保留 C++ 呼叫堆疊；其間不可銷毀、暫停或改寫該堆疊所屬
        // 執行緒，也不可讓整份 RegistryState 回滾覆蓋其他呼叫的 handle 變動。
        if(one_of(name,{"ExitProcess","TerminateProcess","ExitThread","TerminateThread",
            "FreeLibraryAndExitThread","SuspendThread","Wow64SuspendThread","ResumeThread",
            "GetThreadContext","SetThreadContext","Wow64GetThreadContext","Wow64SetThreadContext",
            "CreateThread","CreateRemoteThread","CreateRemoteThreadEx","TerminateJobObject",
            "SetInformationJobObject","AssignProcessToJobObject","ContinueDebugEvent",
            "CloseHandle","DuplicateHandle","SetHandleInformation","GetHandleInformation",
            "CompareObjectHandles","GetProcessHandleCount","FatalAppExitA","FatalAppExitW",
            "CreateFileA","CreateFileW","CreateFile2","SearchPathA","SearchPathW",
            "CreateProcessA","CreateProcessW","CreateProcessAsUserA","CreateProcessAsUserW","WinExec"}))return true;
        // 切換／刪除 fiber 也會移交或清理 callback 框架；純查詢仍可運作。
        if(contains(fiber_service_descriptors()) && name!="IsThreadAFiber")return true;
    }
    if(family=="ntdll.dll") {
        // Native VM 共用的錯誤分類會透過 object_type 查詢登錄 handle；NtCreateKey
        // 提交前已建立暫存 handle，連失敗結果也必須等交易完成才判定型別。
        // 此 catalog 的 NT 入口僅涵蓋程序記憶體，不包含一般事件與音訊服務。
        if(contains(memory_service_descriptors()))return true;
        const auto native=name.starts_with("Nt") || name.starts_with("Zw")?name.substr(2):std::string_view{};
        if(one_of(native,{"Close","DuplicateObject","QueryObject","SetInformationObject","CompareObjects",
            "QueryMutant","QueryAttributesFile","QueryFullAttributesFile",
            "QueryInformationProcess","QueryInformationThread","QuerySystemInformation",
            "SetInformationProcess","SetInformationThread","TerminateProcess","TerminateThread",
            "SuspendProcess","ResumeProcess","SuspendThread","ResumeThread","GetContextThread","SetContextThread",
            "CreateThread","CreateThreadEx","CreateUserProcess","TerminateJobObject","SetInformationJobObject",
            "AssignProcessToJobObject","CreateFile","OpenFile","QuerySecurityObject","SetSecurityObject",
            "Continue","RaiseException"}))return true;
        if(one_of(name,{"RtlExitUserProcess","RtlExitUserThread","RtlExitUserProcessWithStatus",
            "RtlRestoreContext","RtlUnwind","RtlUnwindEx","RtlRaiseException","RtlFailFast"}))return true;
    }
    if(family=="crt" && one_of(name,{"exit","_exit","_Exit","quick_exit","abort","terminate","_terminate",
        "_cexit","_c_exit","_endthread","_endthreadex","_beginthread","_beginthreadex",
        "__crtTerminateProcess","_invoke_watson","_invalid_parameter_noinfo_noreturn"}))return true;
    // 建立子程序的共用繼承檢查也會查詢 handle 型別；CRT spawn／exec／system
    // 直接進入此流程，必須在建立暫存參數之前等待。既有子程序的 _cwait 不受影響。
    if(family=="crt" && name!="_cwait" && contains(process_service_descriptors()))return true;
    if(family=="advapi32.dll" && name.starts_with("CreateProcess") && contains(process_service_descriptors()))return true;
    if((family=="advapi32.dll" && (name.starts_with("InitiateSystemShutdown") || name.starts_with("InitiateShutdown"))) ||
       (family=="user32.dll" && name=="ExitWindowsEx"))return true;

    if(family=="advapi32.dll" && (one_of(name,{"ImpersonateAnonymousToken","GetSecurityInfo","SetSecurityInfo",
        "GetKernelObjectSecurity","SetKernelObjectSecurity"}) || name.starts_with("GetNamedSecurityInfo") ||
        name.starts_with("SetNamedSecurityInfo")))return true;

    // SetupAPI／CM 的共用入口會先更新 devnode；USB CreateFile 的間接更新已列於上方。
    if((family=="setupapi.dll" || family=="cfgmgr32.dll") &&
       (name.starts_with("CM_") || name.starts_with("SetupDi") || name.starts_with("SetupGetInf") ||
        name.starts_with("SetupCopyOEMInf") || name.starts_with("SetupUninstallOEMInf")))return true;
    if(family=="newdev.dll")return true;
    if(family=="user32.dll" && name.starts_with("RegisterDeviceNotification"))return true;

    if((family=="ole32.dll" || family=="combase.dll") && one_of(name,{"CoCreateInstance","CoCreateInstanceEx","CoGetClassObject",
        "ProgIDFromCLSID","CLSIDFromProgID","CLSIDFromString","RoGetActivationFactory","RoActivateInstance"}))return true;
    if(family=="internal-com-rot" && name=="Register")return true;
    // 外部型別參照會惰性查詢 TypeLib 登錄，包含 COM metadata 與 Invoke。
    // 釋放、引用計數及 QueryInterface 不讀 registry，毋須因 raw I/O 暫停。
    if(contains(type_library_service_descriptors()) && !one_of(name,{"QueryInterface","AddRef","Release"}) &&
       !name.starts_with("Release"))return true;
    if(family=="internal-dwrite" && name=="Factory.GetEudcFontCollection")return true;
    if((family=="dinput.dll" || family=="dinput8.dll") &&
       one_of(name,{"DllRegisterServer","DllUnregisterServer"}))return true;
    if(family=="internal-dinput" && one_of(name,{"BuildActionMap","SetActionMap","ConfigureDevices","EnumDevicesBySemantics"}))return true;

    if(family=="shlwapi.dll" && name.starts_with("AssocQueryString"))return true;
    if(family=="shell32.dll" && (name.starts_with("ShellExecute") || name.starts_with("FindExecutable")))return true;
    if(family=="userenv.dll" && (name.starts_with("GetProfilesDirectory") || name.starts_with("GetDefaultUserProfileDirectory") ||
        name.starts_with("GetAllUsersProfileDirectory") || name.starts_with("GetUserProfileDirectory") ||
        name.starts_with("ExpandEnvironmentStringsForUser") || one_of(name,{"CreateEnvironmentBlock",
        "CreateAppContainerProfile","DeleteAppContainerProfile","GetAppContainerFolderPath"})))return true;

    // WinINet 的發送／接收完成路徑也會更新 URL cache；不能只分類名稱含 UrlCache 者。
    // WinHTTP 本身不使用此持久快取，只保留會載入 proxy 設定的入口。
    if(family=="wininet.dll" && contains(http_service_descriptors()))return true;
    if(family=="winhttp.dll" && one_of(name,{"WinHttpOpen","WinHttpSetOption","WinHttpGetDefaultProxyConfiguration",
        "WinHttpSetDefaultProxyConfiguration","WinHttpGetIEProxyConfigForCurrentUser"}))return true;
    if(family=="ws2_32.dll" && (name.starts_with("WSASetService") || name.starts_with("WSALookupServiceBegin")))return true;
    if(family=="mpr.dll" && (name.starts_with("WNetOpenEnum") || name.starts_with("WNetAddConnection2") ||
        name.starts_with("WNetCancelConnection2")))return true;
    if((family=="wevtapi.dll" && name=="EvtQuery") || (family=="advapi32.dll" &&
       (name.starts_with("OpenEventLog") || name.starts_with("RegisterEventSource"))))return true;
    if(family=="internal-url-security" && one_of(name,{"MapUrlToZone","GetSecurityId","ProcessUrlAction"}))return true;

    // callback_return、背景 poll、初始化／析構不具有本函式的 API descriptor，
    // 必須由呼叫端另行延後其登錄存取；不能藉由封鎖一般事件／音訊服務替代。
    return false;
}
}
