#include "x64arm/windows_services.hpp"
#include "bluetooth_sdp.hpp"
#include "virtual_firmware.hpp"
#include "x3daudio.hpp"
#include "windows_crt_character.hpp"
#include "windows_locale_data.hpp"
#include "x64arm/process_debugger.hpp"
#include "x64arm/unicode.hpp"
#include "x64arm/cpu_features.hpp"
#include "x64arm/windows_version.hpp"
#include "x64arm/windows_security_attributes.hpp"
#include "x64arm/windows_paths.hpp"
#include "x64arm/windows_system_paths.hpp"
#include "x64arm/security_descriptor.hpp"
#include "x64arm/security_access.hpp"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace x64arm {
std::span<const WindowsServiceDescriptor> compatibility_service_descriptors() {
#define CM_SERVICE(symbol,count,narrow) {"setupapi.dll",symbol,count,0,0,narrow},{"cfgmgr32.dll",symbol,count,0,0,narrow}
    static constexpr WindowsServiceDescriptor table[]{
        {"wldap32.dll","ldap_init",2,0,0,0,true},
        {"wldap32.dll","ldap_initA",2,0,0,0,true},
        {"wldap32.dll","ldap_initW",2,0,0,0,true},
        {"wldap32.dll","ldap_set_option",3,0,0,0,true},
        {"wldap32.dll","ldap_set_optionA",3,0,0,0,true},
        {"wldap32.dll","ldap_set_optionW",3,0,0,0,true},
        {"wldap32.dll","ldap_get_option",3,0,0,0,true},
        {"wldap32.dll","ldap_get_optionA",3,0,0,0,true},
        {"wldap32.dll","ldap_get_optionW",3,0,0,0,true},
        {"wldap32.dll","ldap_err2string",1,0,0,0,true},
        {"wldap32.dll","ldap_err2stringA",1,0,0,0,true},
        {"wldap32.dll","ldap_err2stringW",1,0,0,0,true},
        {"wldap32.dll","ldap_get_dn",2,0,0,0,true},
        {"wldap32.dll","ldap_get_dnA",2,0,0,0,true},
        {"wldap32.dll","ldap_get_dnW",2,0,0,0,true},
        {"wldap32.dll","ldap_first_attribute",3,0,0,0,true},
        {"wldap32.dll","ldap_first_attributeA",3,0,0,0,true},
        {"wldap32.dll","ldap_first_attributeW",3,0,0,0,true},
        {"wldap32.dll","ldap_next_attribute",3,0,0,0,true},
        {"wldap32.dll","ldap_next_attributeA",3,0,0,0,true},
        {"wldap32.dll","ldap_next_attributeW",3,0,0,0,true},
        {"wldap32.dll","ldap_get_values_len",3,0,0,0,true},
        {"wldap32.dll","ldap_get_values_lenA",3,0,0,0,true},
        {"wldap32.dll","ldap_get_values_lenW",3,0,0,0,true},
        {"wldap32.dll","ldap_memfree",1,0,0,0,true},
        {"wldap32.dll","ldap_memfreeA",1,0,0,0,true},
        {"wldap32.dll","ldap_memfreeW",1,0,0,0,true},
        {"wldap32.dll","ldap_search_s",7,0,0,0,true},
        {"wldap32.dll","ldap_search_sA",7,0,0,0,true},
        {"wldap32.dll","ldap_search_sW",7,0,0,0,true},
        {"wldap32.dll","ldap_simple_bind_s",3,0,0,0,true},
        {"wldap32.dll","ldap_simple_bind_sA",3,0,0,0,true},
        {"wldap32.dll","ldap_simple_bind_sW",3,0,0,0,true},
        {"wldap32.dll","ldap_first_entry",2,0,0,0,true},
        {"wldap32.dll","ldap_next_entry",2,0,0,0,true},
        {"wldap32.dll","ldap_count_entries",2,0,0,0,true},
        {"wldap32.dll","ldap_count_values_len",1,0,0,0,true},
        {"wldap32.dll","ldap_msgfree",1,0,0,0,true},
        {"wldap32.dll","ldap_unbind_s",1,0,0,0,true},
        {"wldap32.dll","ldap_unbind",1,0,0,0,true},
        {"wldap32.dll","ldap_value_free_len",1,0,0,0,true},
        {"wldap32.dll","ber_free",2,0,0,0,true},
        {"wldap32.dll","LdapGetLastError",0,0,0,0,true},
        {"wldap32.dll","#143",2,0,0,0,true},
        {"wldap32.dll","#200",1,0,0,0,true},
        {"wldap32.dll","#211",3,0,0,0,true},
        {"wldap32.dll","#22",1,0,0,0,true},
        {"wldap32.dll","#26",2,0,0,0,true},
        {"wldap32.dll","#27",2,0,0,0,true},
        {"wldap32.dll","#30",2,0,0,0,true},
        {"wldap32.dll","#301",2,0,0,0,true},
        {"wldap32.dll","#32",3,0,0,0,true},
        {"wldap32.dll","#33",3,0,0,0,true},
        {"wldap32.dll","#35",3,0,0,0,true},
        {"wldap32.dll","#41",1,0,0,0,true},
        {"wldap32.dll","#46",1,0,0,0,true},
        {"wldap32.dll","#50",7,0,0,0,true},
        {"wldap32.dll","#60",3,0,0,0,true},
        {"wldap32.dll","#79",1,0,0,0,true},

        {"x3daudio1_7.dll","X3DAudioInitialize",3,0,0,0,true},
        {"x3daudio1_7.dll","X3DAudioCalculate",5,0,0,0,true},
        {"bthprops.cpl","BluetoothSdpGetElementData",3},
        {"bthprops.cpl","BluetoothSdpGetContainerElementData",4},
        {"bthprops.cpl","BluetoothSdpGetAttributeValue",4},
        {"bluetoothapis.dll","BluetoothSdpGetElementData",3},
        {"bluetoothapis.dll","BluetoothSdpGetContainerElementData",4},
        {"bluetoothapis.dll","BluetoothSdpGetAttributeValue",4},
#include "bluetooth_descriptors.inc"

        {"setupapi.dll","SetupDiGetClassDevsA",4,0,0,8},
        {"setupapi.dll","SetupDiGetClassDevsW",4,0,0,8},
        {"setupapi.dll","SetupDiGetClassDevsExA",7,0,0,8},
        {"setupapi.dll","SetupDiGetClassDevsExW",7,0,0,8},
        {"setupapi.dll","SetupDiCreateDeviceInfoList",2},
        {"setupapi.dll","SetupDiCreateDeviceInfoListExA",4},{"setupapi.dll","SetupDiCreateDeviceInfoListExW",4},
        {"setupapi.dll","SetupDiGetDeviceInfoListClass",2},
        {"setupapi.dll","SetupDiGetDeviceInfoListDetailA",2},{"setupapi.dll","SetupDiGetDeviceInfoListDetailW",2},
        {"setupapi.dll","SetupDiBuildDriverInfoList",3,0,0,4},{"setupapi.dll","SetupDiDestroyDriverInfoList",3,0,0,4},
        {"setupapi.dll","SetupDiEnumDriverInfoA",5,0,0,12},{"setupapi.dll","SetupDiEnumDriverInfoW",5,0,0,12},
        {"setupapi.dll","SetupGetInfDriverStoreLocationA",6,0,0,16},{"setupapi.dll","SetupGetInfDriverStoreLocationW",6,0,0,16},
        {"setupapi.dll","SetupDiDestroyDeviceInfoList",1},
        {"setupapi.dll","SetupDiEnumDeviceInfo",3,0,0,2},
        {"setupapi.dll","SetupDiDeleteDeviceInfo",2},
        {"setupapi.dll","SetupDiGetDeviceRegistryPropertyA",7,0,0,36},
        {"setupapi.dll","SetupDiGetDeviceRegistryPropertyW",7,0,0,36},
        {"setupapi.dll","SetupDiGetDeviceInstanceIdA",5,0,0,8},
        {"setupapi.dll","SetupDiGetDeviceInstanceIdW",5,0,0,8},
        {"setupapi.dll","SetupDiEnumDeviceInterfaces",5,0,0,8},
        {"setupapi.dll","SetupDiOpenDevRegKey",6,0,0,60},
        {"setupapi.dll","SetupDiOpenDeviceInfoA",5,0,0,8},
        {"setupapi.dll","SetupDiOpenDeviceInfoW",5,0,0,8},
        {"setupapi.dll","SetupDiGetDevicePropertyW",8,0,0,160},
        {"setupapi.dll","SetupDiOpenDeviceInterfaceA",4,0,0,4},
        {"setupapi.dll","SetupDiOpenDeviceInterfaceW",4,0,0,4},
        {"setupapi.dll","SetupDiGetDeviceInterfaceDetailA",6,0,0,8},
        {"setupapi.dll","SetupDiGetDeviceInterfaceDetailW",6,0,0,8},
        {"setupapi.dll","SetupDiOpenDeviceInterfaceRegKey",4,0,0,12},
        {"setupapi.dll","SetupDiDeleteDeviceInterfaceData",2},
        {"setupapi.dll","SetupDiCreateDeviceInfoA",7,0,0,0x20},{"setupapi.dll","SetupDiCreateDeviceInfoW",7,0,0,0x20},
        {"setupapi.dll","SetupDiSetDeviceRegistryPropertyA",5,0,0,0x14},{"setupapi.dll","SetupDiSetDeviceRegistryPropertyW",5,0,0,0x14},
        {"setupapi.dll","SetupDiSetClassInstallParamsA",4,0,0,8},{"setupapi.dll","SetupDiSetClassInstallParamsW",4,0,0,8},
        {"setupapi.dll","SetupDiGetClassInstallParamsA",5,0,0,8},{"setupapi.dll","SetupDiGetClassInstallParamsW",5,0,0,8},
        {"setupapi.dll","SetupCopyOEMInfA",8,0,0,0x2c},{"setupapi.dll","SetupCopyOEMInfW",8,0,0,0x2c},
        {"setupapi.dll","SetupUninstallOEMInfA",3,0,0,2},{"setupapi.dll","SetupUninstallOEMInfW",3,0,0,2},
        {"setupapi.dll","SetupDiGetDriverInfoDetailA",6,0,0,0x10},{"setupapi.dll","SetupDiGetDriverInfoDetailW",6,0,0,0x10},
        {"newdev.dll","DiUninstallDevice",5,0,0,8},
        {"newdev.dll","DiInstallDriverA",4,0,0,4},{"newdev.dll","DiInstallDriverW",4,0,0,4},{"newdev.dll","DiUninstallDriverA",4,0,0,4},{"newdev.dll","DiUninstallDriverW",4,0,0,4},
        {"setupapi.dll","SetupDiCallClassInstaller",3,0,0,1},{"setupapi.dll","SetupDiRegisterDeviceInfo",6,0,0,4},{"setupapi.dll","SetupDiRemoveDevice",2},
        CM_SERVICE("CM_Get_DevNode_PropertyW",6,33),
        {"cfgmgr32.dll","CM_Register_Notification",4},{"cfgmgr32.dll","CM_Unregister_Notification",1},{"cfgmgr32.dll","CM_MapCrToWin32Err",2},
        CM_SERVICE("CM_Locate_DevNodeA",3,4),CM_SERVICE("CM_Locate_DevNodeW",3,4),
        CM_SERVICE("CM_Get_Child",3,6),CM_SERVICE("CM_Get_Sibling",3,6),CM_SERVICE("CM_Get_Parent",3,6),
        CM_SERVICE("CM_Get_DevNode_Status",4,12),
        CM_SERVICE("CM_Enable_DevNode",2,3),CM_SERVICE("CM_Disable_DevNode",2,3),
        CM_SERVICE("CM_Get_Device_IDA",4,13),CM_SERVICE("CM_Get_Device_IDW",4,13),CM_SERVICE("CM_Get_Device_ID_Size",3,6),
        CM_SERVICE("CM_Get_Device_ID_ListA",4,12),CM_SERVICE("CM_Get_Device_ID_ListW",4,12),
        CM_SERVICE("CM_Get_Device_ID_List_SizeA",3,4),CM_SERVICE("CM_Get_Device_ID_List_SizeW",3,4),
        CM_SERVICE("CM_Get_DevNode_Registry_PropertyA",6,35),CM_SERVICE("CM_Get_DevNode_Registry_PropertyW",6,35),
        CM_SERVICE("CM_Open_DevNode_Key",6,47),
        CM_SERVICE("CM_Get_Device_Interface_List_SizeA",4,8),CM_SERVICE("CM_Get_Device_Interface_List_SizeW",4,8),
        CM_SERVICE("CM_Get_Device_Interface_ListA",5,0x18),CM_SERVICE("CM_Get_Device_Interface_ListW",5,0x18),
        {"setupapi.dll","SetupDiGetDeviceInterfacePropertyW",8,0,0,0xa0},
        {"secur32.dll","GetUserNameExA",3},{"secur32.dll","GetUserNameExW",3},{"advapi32.dll","GetUserNameA",2},{"advapi32.dll","GetUserNameW",2},
        {"shlwapi.dll","IsOS",1},{"shcore.dll","IsOS",1},
        {"shlwapi.dll","SHAutoComplete",2},
        {"user32.dll","ExitWindowsEx",2},
        {"advapi32.dll","InitiateSystemShutdownA",5},{"advapi32.dll","InitiateSystemShutdownW",5},
        {"advapi32.dll","InitiateSystemShutdownExA",6},{"advapi32.dll","InitiateSystemShutdownExW",6},
        {"advapi32.dll","AbortSystemShutdownA",1},{"advapi32.dll","AbortSystemShutdownW",1},
        {"advapi32.dll","InitiateShutdownA",5},{"advapi32.dll","InitiateShutdownW",5},
        {"shell32.dll", "CommandLineToArgvW", 2}, {"kernel32.dll", "LocalFree", 1},
        {"kernel32.dll","LocalAlloc",2},{"kernel32.dll","LocalLock",1},{"kernel32.dll","LocalUnlock",1},
        {"kernel32.dll","LocalReAlloc",3},
        {"kernel32.dll","RtlCaptureStackBackTrace",4},{"ntdll.dll","RtlCaptureStackBackTrace",4},
        {"kernel32.dll","CaptureStackBackTrace",4},
        {"ntdll.dll","RtlGetCallersAddress",2},
        {"kernel32.dll","CreateNamedPipeA",8},{"kernel32.dll","CreateNamedPipeW",8},
        {"kernel32.dll","ConnectNamedPipe",2},{"kernel32.dll","DisconnectNamedPipe",1},
        {"kernel32.dll","PeekNamedPipe",6},{"kernel32.dll","TransactNamedPipe",7},
        {"kernel32.dll","SetNamedPipeHandleState",4},{"kernel32.dll","GetNamedPipeInfo",5},
        {"kernel32.dll","GetNamedPipeClientProcessId",2},{"kernel32.dll","GetNamedPipeServerProcessId",2},
        {"kernel32.dll","GetNamedPipeHandleStateA",7},{"kernel32.dll","GetNamedPipeHandleStateW",7},
        {"kernel32.dll","WaitNamedPipeA",2},{"kernel32.dll","WaitNamedPipeW",2},
        {"kernel32.dll","LockFile",5},{"kernel32.dll","UnlockFile",5},{"kernel32.dll","LockFileEx",6},{"kernel32.dll","UnlockFileEx",5},
        {"kernel32.dll","CancelIo",1},{"kernel32.dll","CancelIoEx",2},
        {"kernel32.dll","EncodePointer",1},{"kernel32.dll","DecodePointer",1},
        {"ntdll.dll","RtlEncodePointer",1},{"ntdll.dll","RtlDecodePointer",1},
        {"crt","_encode_pointer",1},{"crt","_decode_pointer",1},
        {"kernel32.dll","LocalFlags",1},{"kernel32.dll","LocalSize",1},{"kernel32.dll","LocalHandle",1},
        {"kernel32.dll", "SetConsoleCtrlHandler", 2},
        {"kernel32.dll", "GetSystemDefaultLangID", 0},{"kernel32.dll", "GetUserDefaultLangID", 0},{"kernel32.dll", "GetThreadUILanguage", 0},{"kernel32.dll", "GetUserDefaultUILanguage", 0},
        {"kernel32.dll","GetSystemDefaultUILanguage",0},{"kernel32.dll","GetSystemDefaultLCID",0},
        {"kernel32.dll","GetGeoInfoA",5},{"kernel32.dll","GetGeoInfoW",5},{"kernel32.dll","GetUserGeoID",1},{"kernel32.dll","GetUserDefaultLocaleName",2},{"kernel32.dll","GetSystemDefaultLocaleName",2},
        {"kernel32.dll","GetUserDefaultLCID",0},{"kernel32.dll","GetThreadLocale",0},
        {"kernel32.dll","GetThreadPreferredUILanguages",4},{"kernel32.dll","GetUserPreferredUILanguages",4},
        {"kernel32.dll","GetSystemPreferredUILanguages",4},
        {"kernel32.dll", "SetFileApisToOEM", 0},{"kernel32.dll", "SetFileApisToANSI", 0},{"kernel32.dll", "AreFileApisANSI", 0},{"kernel32.dll", "GetLargePageMinimum", 0},
        {"crt", "wcslen", 1}, {"crt", "wcsnlen", 2}, {"crt", "strnlen", 2}, {"crt", "memchr", 3},
        {"crt", "wcscmp", 2},{"crt", "wcsncmp", 3},{"crt", "wcsstr", 2},{"crt", "wcschr", 2},{"crt", "wcsrchr", 2},
        {"kernel32.dll","lstrlenA",1},{"kernel32.dll","lstrlenW",1},
        {"kernel32.dll","lstrcpyA",2},{"kernel32.dll","lstrcpyW",2},
        {"kernel32.dll","lstrcmpA",2},{"kernel32.dll","lstrcmpW",2},{"kernel32.dll","lstrcmpiA",2},{"kernel32.dll","lstrcmpiW",2},
        {"kernel32.dll","GetLongPathNameA",3},{"kernel32.dll","GetLongPathNameW",3},
        {"kernel32.dll","GetShortPathNameA",3},{"kernel32.dll","GetShortPathNameW",3},
        {"kernel32.dll","lstrcpynA",3},{"kernel32.dll","lstrcpynW",3},
        {"kernel32.dll","lstrcatA",2},{"kernel32.dll","lstrcatW",2},
        {"crt", "__std_exception_copy", 2}, {"crt", "__std_exception_destroy", 1}, {"crt", "_strdup", 1}, {"crt", "_strnicmp", 3}, {"crt", "_stricmp", 2}, {"crt", "_strcmpi", 2}, {"crt", "_wcsicmp", 2}, {"crt", "_wcsnicmp", 3}, {"crt", "strspn", 2}, {"crt", "strcspn", 2},
        {"crt", "isspace", 1}, {"crt", "isalnum", 1}, {"crt", "isalpha", 1},
        {"crt", "isdigit", 1}, {"crt", "isxdigit", 1}, {"crt", "isupper", 1}, {"crt", "islower", 1},
        {"crt", "isprint", 1}, {"crt", "isgraph", 1}, {"crt", "ispunct", 1}, {"crt", "iscntrl", 1},
        {"crt", "iswspace", 1}, {"crt", "iswalnum", 1}, {"crt", "iswalpha", 1}, {"crt", "iswcntrl", 1},
        {"crt", "iswdigit", 1}, {"crt", "iswxdigit", 1}, {"crt", "iswupper", 1}, {"crt", "iswlower", 1},
        {"crt", "iswprint", 1}, {"crt", "iswgraph", 1}, {"crt", "iswpunct", 1}, {"crt", "iswblank", 1},
        {"crt", "tolower", 1}, {"crt", "toupper", 1}, {"crt", "towlower", 1}, {"crt", "towupper", 1},
        {"crt", "_fileno", 1}, {"crt", "_isatty", 1}, {"crt", "_get_osfhandle", 1}, {"crt", "_setmode", 2},
        {"crt", "rand_s", 1}, {"crt", "rand", 0}, {"crt", "srand", 1},
        {"crt", "qsort", 4}, {"crt", "bsearch", 5},
        {"crt", "setlocale", 2}, {"crt", "_wsetlocale", 2}, {"crt", "localeconv", 0}, {"crt", "___lc_codepage_func", 0}, {"crt", "___lc_collate_cp_func", 0},
        {"crt", "___mb_cur_max_func", 0}, {"crt", "__mb_cur_max_func", 0}, {"crt","_ismbblead",1},{"crt","_ismbbtrail",1}, {"crt", "_lock", 1}, {"crt", "_unlock", 1},
        {"crt", "__lconv_init", 0},
        {"crt", "wcrtomb", 3}, {"crt", "wctomb", 2},
        {"crt", "___lc_locale_name_func", 0}, {"crt", "__pctype_func", 0}, {"crt", "wctype", 1},
        {"crt", "iswctype", 2}, {"crt", "_isctype", 2}, {"crt", "isblank", 1},
        {"crt", "btowc", 1}, {"crt", "wctob", 1}, {"crt", "mbrtowc", 4}, {"crt", "mbrlen", 3}, {"crt", "mbtowc", 3}, {"crt", "mblen", 2}, {"crt", "mbsinit", 1},
        {"crt", "_lock_locales", 0}, {"crt", "_unlock_locales", 0}, {"crt", "_lock_file", 1}, {"crt", "_unlock_file", 1}, {"crt", "fputc", 2}, {"crt", "putc", 2}, {"crt", "_write", 3},
        {"crt", "_fullpath", 3}, {"crt", "_wfullpath", 3}, {"crt", "_getcwd", 2}, {"crt", "_wgetcwd", 2},
        {"kernel32.dll", "GetCurrentDirectoryA", 2}, {"kernel32.dll", "GetCurrentDirectoryW", 2},
        {"kernel32.dll", "GetFullPathNameA", 4}, {"kernel32.dll", "GetFullPathNameW", 4},
        {"user32.dll","CharToOemA",2},{"user32.dll","CharToOemW",2},
        {"user32.dll","CharToOemBuffA",3},{"user32.dll","CharToOemBuffW",3},
        {"user32.dll","OemToCharA",2},{"user32.dll","OemToCharW",2},
        {"user32.dll","OemToCharBuffA",3},{"user32.dll","OemToCharBuffW",3},
        {"kernel32.dll", "MultiByteToWideChar", 6}, {"kernel32.dll", "WideCharToMultiByte", 8},
        {"kernel32.dll", "GetLocaleInfoA", 4},{"kernel32.dll", "GetLocaleInfoW", 4},{"kernel32.dll", "IsValidCodePage", 1}, {"kernel32.dll", "GetCPInfo", 2}, {"kernel32.dll", "GetOEMCP", 0},
        {"kernel32.dll","GetLocaleInfoEx",4},
        {"kernel32.dll","GetSystemFirmwareTable",4},{"kernel32.dll","EnumSystemFirmwareTables",3},{"kernel32.dll","GetFirmwareType",1},
        {"kernel32.dll","GetFirmwareEnvironmentVariableA",4},{"kernel32.dll","GetFirmwareEnvironmentVariableW",4},{"kernel32.dll","GetFirmwareEnvironmentVariableExA",5},{"kernel32.dll","GetFirmwareEnvironmentVariableExW",5},
        {"dbgeng.dll","DebugCreate",2},{"dbgeng.dll","DebugCreateEx",3},{"dbgeng.dll","DebugConnect",3},{"dbgeng.dll","DebugConnectWide",3},
        {"tbs.dll","Tbsi_GetDeviceInfo",2},{"tbs.dll","Tbsi_Context_Create",2},{"tbs.dll","Tbsip_Context_Close",1},{"tbs.dll","Tbsip_Submit_Command",7},{"tbs.dll","Tbsi_Revoke_Attestation",0},
        {"kernel32.dll","SetFirmwareEnvironmentVariableA",4},{"kernel32.dll","SetFirmwareEnvironmentVariableW",4},{"kernel32.dll","SetFirmwareEnvironmentVariableExA",5},{"kernel32.dll","SetFirmwareEnvironmentVariableExW",5},
        {"kernel32.dll", "AreFileApisANSI", 0}, {"kernel32.dll", "IsDBCSLeadByteEx", 2},
        {"kernel32.dll", "IsWow64Process", 2}, {"kernel32.dll", "IsWow64Process2", 3},
        {"kernel32.dll", "GetCurrentProcess", 0}, {"kernel32.dll", "GetCurrentThread", 0},
        {"kernel32.dll", "DuplicateHandle", 7}, {"kernel32.dll", "GetThreadPriorityBoost", 2}, {"kernel32.dll", "SetThreadPriorityBoost", 2}, {"kernel32.dll", "GetThreadPriority", 1}, {"kernel32.dll", "SetThreadPriority", 2},
        {"kernel32.dll", "SetStdHandle", 2},
        {"kernel32.dll", "ReadConsoleA", 5}, {"kernel32.dll", "ReadConsoleW", 5},
        {"kernel32.dll", "WriteConsoleA", 5}, {"kernel32.dll", "WriteConsoleW", 5}, {"kernel32.dll", "SetConsoleMode", 2},
        {"kernel32.dll", "GetConsoleCP", 0}, {"kernel32.dll", "GetConsoleOutputCP", 0}, {"kernel32.dll", "GetConsoleMode", 2}, {"kernel32.dll", "GetFileType", 1}, {"kernel32.dll", "GetHandleInformation", 2},
        {"kernel32.dll", "SetHandleInformation", 3},{"kernel32.dll", "GetConsoleScreenBufferInfo", 2},
        {"psapi.dll","EnumProcessModules",4},{"kernel32.dll","K32EnumProcessModules",4},
        {"psapi.dll","EnumProcesses",3},{"kernel32.dll","K32EnumProcesses",3},
        {"kernel32.dll","GetCurrentApplicationUserModelId",2},{"kernel32.dll","GetCurrentPackageId",2},{"kernel32.dll","GetCurrentPackageFullName",2},
        {"kernel32.dll","GetCurrentPackageFamilyName",2},{"kernel32.dll","GetCurrentPackagePath",2},
        {"psapi.dll","EnumProcessModulesEx",5},{"kernel32.dll","K32EnumProcessModulesEx",5},
        {"psapi.dll","GetModuleInformation",4},{"kernel32.dll","K32GetModuleInformation",4},
        {"kernel32.dll","QueryFullProcessImageNameA",4},{"kernel32.dll","QueryFullProcessImageNameW",4},
        {"psapi.dll","GetProcessImageFileNameA",3},{"psapi.dll","GetProcessImageFileNameW",3},
        {"kernel32.dll","K32GetProcessImageFileNameA",3},{"kernel32.dll","K32GetProcessImageFileNameW",3},
        {"psapi.dll","GetModuleBaseNameA",4},{"psapi.dll","GetModuleBaseNameW",4},
        {"kernel32.dll","K32GetModuleBaseNameA",4},{"kernel32.dll","K32GetModuleBaseNameW",4},
        {"psapi.dll","GetModuleFileNameExA",4},{"psapi.dll","GetModuleFileNameExW",4},
        {"kernel32.dll","K32GetModuleFileNameExA",4},{"kernel32.dll","K32GetModuleFileNameExW",4},
        {"kernel32.dll", "GetModuleFileNameA", 3}, {"kernel32.dll", "GetModuleFileNameW", 3},
        {"kernel32.dll", "GetModuleHandleA", 1}, {"kernel32.dll", "GetModuleHandleW", 1}, {"kernel32.dll", "GetProcAddress", 2},
        {"kernel32.dll", "LoadLibraryExW", 3}, {"kernel32.dll", "LoadLibraryExA", 3},
        {"kernel32.dll", "SetDllDirectoryA", 1}, {"kernel32.dll", "SetDllDirectoryW", 1},
        {"kernel32.dll", "SetDefaultDllDirectories", 1}, {"kernel32.dll", "AddDllDirectory", 1}, {"kernel32.dll", "RemoveDllDirectory", 1},
        {"kernel32.dll", "GetDllDirectoryA", 2}, {"kernel32.dll", "GetDllDirectoryW", 2},
        {"kernel32.dll", "GetModuleHandleExA", 3},{"kernel32.dll", "GetModuleHandleExW", 3},{"kernel32.dll", "FreeLibrary", 1}, {"kernel32.dll", "LoadLibraryW", 1}, {"kernel32.dll", "LoadLibraryA", 1}, {"kernel32.dll", "DisableThreadLibraryCalls", 1},
        {"bcryptprimitives.dll", "ProcessPrng", 2}, {"ntdll.dll", "RtlGetCurrentPeb", 0}, {"ntdll.dll", "RtlGetVersion", 1}, {"kernel32.dll", "GetVersionExA", 1}, {"kernel32.dll", "GetVersionExW", 1}, {"kernel32.dll", "GetVersion", 0}, {"kernel32.dll", "GetProductInfo", 5},
        {"kernel32.dll","VerSetConditionMask",3,1},{"ntdll.dll","VerSetConditionMask",3,1},
        {"kernel32.dll","VerifyVersionInfoA",3,4},{"kernel32.dll","VerifyVersionInfoW",3,4},{"ntdll.dll","RtlVerifyVersionInfo",3,4},
        {"ntdll.dll","RtlGetNtVersionNumbers",3},
        {"kernel32.dll", "GetSystemInfo", 1}, {"kernel32.dll", "GetNativeSystemInfo", 1},
        {"kernel32.dll", "GetSystemDirectoryA", 2}, {"kernel32.dll", "GetSystemDirectoryW", 2},
        {"kernel32.dll", "GetSystemWow64DirectoryA", 2}, {"kernel32.dll", "GetSystemWow64DirectoryW", 2},
        {"kernel32.dll", "GetWindowsDirectoryA", 2}, {"kernel32.dll", "GetWindowsDirectoryW", 2},
        {"kernel32.dll", "GetSystemWindowsDirectoryA", 2}, {"kernel32.dll", "GetSystemWindowsDirectoryW", 2},
        {"powrprof.dll", "PowerGetActiveScheme", 2}, {"powrprof.dll", "CallNtPowerInformation", 5}, {"rpcrt4.dll", "UuidToStringA", 2}, {"rpcrt4.dll", "UuidToStringW", 2}, {"rpcrt4.dll", "RpcStringFreeA", 1}, {"rpcrt4.dll", "RpcStringFreeW", 1}, {"rpcrt4.dll", "UuidCreate", 1}, {"ole32.dll", "CoCreateGuid", 1}, {"kernel32.dll", "SetHandleCount", 1}, {"kernel32.dll", "GetThreadErrorMode", 0}, {"kernel32.dll", "SetThreadErrorMode", 2}, {"kernel32.dll", "GetErrorMode", 0}, {"kernel32.dll", "SetErrorMode", 1},
        {"kernel32.dll", "WerGetFlags", 2}, {"kernel32.dll", "WerSetFlags", 1},
        {"kernel32.dll", "GetEnvironmentStringsW", 0}, {"kernel32.dll", "GetEnvironmentStringsA", 0},
        {"kernel32.dll", "FreeEnvironmentStringsW", 1}, {"kernel32.dll", "FreeEnvironmentStringsA", 1},
        {"kernel32.dll", "CreateWaitableTimerExA", 4,0,0,12}, {"kernel32.dll", "CreateWaitableTimerExW", 4,0,0,12},
        {"kernel32.dll", "CreateWaitableTimerA", 3,0,0,2}, {"kernel32.dll", "CreateWaitableTimerW", 3,0,0,2},
        {"kernel32.dll", "OpenWaitableTimerA", 3,0,0,3}, {"kernel32.dll", "OpenWaitableTimerW", 3,0,0,3},
        {"kernel32.dll", "IsDebuggerPresent", 0}, {"kernel32.dll", "IsProcessorFeaturePresent", 1},
        {"kernel32.dll", "GetEnabledXStateFeatures", 0}, {"ntdll.dll", "RtlGetEnabledExtendedFeatures", 1, 1},
        {"kernel32.dll", "QueryPerformanceCounter", 1}, {"kernel32.dll", "QueryPerformanceFrequency", 1},
        {"kernel32.dll", "GetSystemTimeAsFileTime", 1}, {"kernel32.dll", "GetTickCount", 0},
        {"kernel32.dll", "TryEnterCriticalSection", 1},
        {"shlwapi.dll", "PathIsRelativeA", 1}, {"shlwapi.dll", "PathIsRelativeW", 1},
        {"shlwapi.dll", "PathFindExtensionA", 1}, {"shlwapi.dll", "PathFindExtensionW", 1},
        {"shlwapi.dll", "PathFindFileNameA", 1}, {"shlwapi.dll", "PathFindFileNameW", 1},
        {"shlwapi.dll", "PathIsUNCA", 1}, {"shlwapi.dll", "PathIsUNCW", 1},
        {"kernel32.dll", "CreateSemaphoreA", 4}, {"kernel32.dll", "CreateSemaphoreW", 4}, {"kernel32.dll", "ReleaseSemaphore", 3},
        {"kernel32.dll", "CreateSemaphoreExA", 6,0,0,54}, {"kernel32.dll", "CreateSemaphoreExW", 6,0,0,54},
        {"kernel32.dll", "OpenSemaphoreA", 3,0,0,3}, {"kernel32.dll", "OpenSemaphoreW", 3,0,0,3},
        {"kernel32.dll", "CreateMutexA", 3}, {"kernel32.dll", "CreateMutexW", 3}, {"kernel32.dll", "ReleaseMutex", 1},
        {"kernel32.dll", "CreateMutexExA", 4}, {"kernel32.dll", "CreateMutexExW", 4}, {"kernel32.dll", "OpenMutexA", 3}, {"kernel32.dll", "OpenMutexW", 3},
        {"kernel32.dll", "CreateEventExA", 4}, {"kernel32.dll", "CreateEventExW", 4}, {"kernel32.dll", "CreateEventA", 4}, {"kernel32.dll", "CreateEventW", 4}, {"kernel32.dll", "SetEvent", 1}, {"kernel32.dll", "ResetEvent", 1},
        {"kernel32.dll", "OpenEventA", 3}, {"kernel32.dll", "OpenEventW", 3},
        {"kernel32.dll", "WaitForSingleObject", 2}, {"kernel32.dll", "WaitForMultipleObjects", 4},
        {"setupapi.dll","SetupDiClassGuidsFromNameA",4},{"setupapi.dll","SetupDiClassGuidsFromNameExA",6},
        {"setupapi.dll","SetupDiClassGuidsFromNameW",4},{"setupapi.dll","SetupDiClassGuidsFromNameExW",6},
        {"setupapi.dll","SetupDiClassNameFromGuidA",4},{"setupapi.dll","SetupDiClassNameFromGuidExA",6},
        {"setupapi.dll","SetupDiClassNameFromGuidW",4},{"setupapi.dll","SetupDiClassNameFromGuidExW",6},
        {"setupapi.dll","SetupDiGetClassDescriptionA",4},{"setupapi.dll","SetupDiGetClassDescriptionExA",6},
        {"setupapi.dll","SetupDiGetClassDescriptionW",4},{"setupapi.dll","SetupDiGetClassDescriptionExW",6},
        {"setupapi.dll","SetupDiBuildClassInfoList",4},{"setupapi.dll","SetupDiBuildClassInfoListExA",6},{"setupapi.dll","SetupDiBuildClassInfoListExW",6},
        {"setupapi.dll","SetupDiOpenClassRegKey",2},{"setupapi.dll","SetupDiOpenClassRegKeyExA",5},{"setupapi.dll","SetupDiOpenClassRegKeyExW",5},
    };
#undef CM_SERVICE
    return table;
}
// CRT 與內建 C++ 例外物件共用訊息配置、複製及銷毀，保留 ABI padding。
std::uint64_t WindowsServices::crt_duplicate_string(GuestMemory& memory,std::uint64_t source,bool wide){
    std::vector<std::uint8_t> bytes;
    if(wide)bytes=utf16_bytes(read_utf16(memory,GuestAddress{source}));
    else {const auto text=read_ansi(memory,source);bytes.assign(text.begin(),text.end());bytes.push_back(0);}
    const auto result=allocate_heap(memory,bytes.size());
    if(!result)memory.store(GuestAddress{runtime_.errno_cell},32,12);
    else memory.write(GuestAddress{result},bytes);
    return result;
}
void WindowsServices::crt_exception_data(GuestState& state,GuestMemory& memory,std::uint64_t source,std::uint64_t destination,bool destroy){
    const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    memory.check(GuestAddress{destination},unit+1,Access::Write);
    const auto text=memory.load(GuestAddress{source},bits);const bool owned=memory.load(GuestAddress{source+unit},8)!=0;
    if(destroy){
        if(owned && text)(void)free_heap(memory,text,state);
        memory.store(GuestAddress{destination},bits,0);memory.store(GuestAddress{destination+unit},8,0);
    }else {
        const auto copy=owned && text?crt_duplicate_string(memory,text):text;
        memory.store(GuestAddress{destination},bits,copy);memory.store(GuestAddress{destination+unit},8,owned && text && copy?1:0);
    }
}
std::optional<std::uint32_t> WindowsServices::invoke_compatibility(GuestState& state, GuestMemory& memory, const WindowsServiceDescriptor& service) {
    if(service.symbol=="GetFileType")if(const auto native=invoke_native_file_information(state,memory,service)){
        WindowsCall(state,memory,service).return_to(state,*native);return std::nullopt;
    }
    if(service.symbol.starts_with("CreateSemaphore") || service.symbol.starts_with("OpenSemaphore") || service.symbol.starts_with("CreateWaitableTimer") || service.symbol.starts_with("OpenWaitableTimer") || service.symbol=="ReleaseSemaphore")return invoke_sync_object(state,memory,service);
    if(service.symbol.starts_with("CM_") || service.symbol.starts_with("SetupDi") || service.symbol.starts_with("SetupGetInf") || service.symbol.starts_with("SetupCopyOEMInf") ||
       service.symbol.starts_with("SetupUninstallOEMInf") || service.family=="newdev.dll")return invoke_device_configuration(state,memory,service);
    if(service.family=="wldap32.dll")return invoke_ldap(state,memory,service);
    const auto module_api=service.symbol;
    if(service.family=="x3daudio1_7.dll"){const WindowsCall call(state,memory,service);const auto result=x3daudio::invoke(memory,call,state,module_api=="X3DAudioInitialize",x86_);call.return_to(state,result);return std::nullopt;}
    if(module_api.starts_with("BluetoothSdp")){const WindowsCall call(state,memory,service);call.return_to(state,bluetooth_sdp::invoke(memory,call,module_api,x86_));return std::nullopt;}
    if(module_api.starts_with("Bluetooth"))return invoke_bluetooth(state,memory,service);
    if(module_api=="GetFirmwareType"){
        const WindowsCall call(state,memory,service);
        if(!call(0)){last_error_=87;call.return_to(state,0);}
        else {memory.store(GuestAddress{call(0)},32,static_cast<std::uint32_t>(virtual_firmware::type));call.return_to(state,1);}
        return std::nullopt;
    }
    if(service.family=="dbgeng.dll"){
        // 沙箱沒有偵錯引擎與核心傾印分析；與 dbgeng 建立失敗時相同，清空輸出介面並回報 HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)。
        const WindowsCall call(state,memory,service);const auto out=call(module_api=="DebugCreate"?1:2);std::uint32_t status=0x80070032U;
        if(!out || (module_api=="DebugCreate" && !call(0)))status=0x80070057U;
        else {try{memory.store(GuestAddress{out},static_cast<std::uint8_t>(x86_?32:64),0);}catch(const MemoryFault&){status=0x80004003U;}}
        static bool reported=false;if(!reported){reported=true;std::fprintf(stderr,"[dbgeng] %.*s：未提供偵錯引擎\n",static_cast<int>(module_api.size()),module_api.data());}
        call.return_to(state,status);return std::nullopt;
    }
    if(service.family=="tbs.dll"){
        // 虛擬平台沒有 TPM：與未安裝 TPM 的 Windows 相同，參數檢查後回報 TBS_E_TPM_NOT_FOUND，不建立 TBS context。
        const WindowsCall call(state,memory,service);std::uint32_t status=0x8028400fU;
        if(module_api=="Tbsi_GetDeviceInfo"){if(!call(1))status=0x80284002U;else if(static_cast<std::uint32_t>(call(0))<16)status=0x80284005U;}
        else if(module_api=="Tbsi_Context_Create"){if(!call(0) || !call(1))status=0x80284002U;else {try{memory.check(GuestAddress{call(1)},x86_?4:8,Access::Write);memory.store(GuestAddress{call(1)},static_cast<std::uint8_t>(x86_?32:64),0);}catch(const MemoryFault&){status=0x80284002U;}}}
        else if(module_api!="Tbsi_Revoke_Attestation")status=0x80284004U;
        call.return_to(state,status);return std::nullopt;
    }
    if(module_api.find("FirmwareEnvironmentVariable")!=module_api.npos){
        // 與 kernel32 相同先以 RtlGUIDFromString 驗證命名空間；虛擬平台為傳統 BIOS，沒有 UEFI 變數服務，NtQuery/SetSystemEnvironmentValueEx 回報 STATUS_NOT_IMPLEMENTED。
        static_assert(virtual_firmware::type==virtual_firmware::Type::Bios,"UEFI 韌體需要變數存放區");
        const WindowsCall call(state,memory,service);const bool wide=module_api.ends_with('W');
        try{
            if(call(0))(void)(wide?read_utf16(memory,GuestAddress{call(0)}):utf8_to_utf16(read_ansi(memory,call(0))));
            const auto guid=call(1)?(wide?read_utf16(memory,GuestAddress{call(1)}):utf8_to_utf16(read_ansi(memory,call(1)))):std::u16string{};
            const auto hex=[](char16_t c){return (c>=u'0' && c<=u'9') || (c>=u'a' && c<=u'f') || (c>=u'A' && c<=u'F');};
            bool valid=guid.size()==38 && guid.front()==u'{' && guid.back()==u'}';
            for(std::size_t i=1;valid && i<37;++i)valid=(i==9 || i==14 || i==19 || i==24)?guid[i]==u'-':hex(guid[i]);
            last_error_=valid?1U:87U;
        }catch(const MemoryFault&){last_error_=998;}
        call.return_to(state,0);return std::nullopt;
    }
    if(module_api=="GetSystemFirmwareTable" || module_api=="EnumSystemFirmwareTables"){
        const WindowsCall call(state,memory,service);const bool enumerate=module_api=="EnumSystemFirmwareTables";
        const auto provider=static_cast<std::uint32_t>(call(0)),id=enumerate?0U:static_cast<std::uint32_t>(call(1));
        const auto buffer=call(enumerate?1:2);const auto capacity=static_cast<std::uint32_t>(call(enumerate?2:3));
        const auto fail=[&](unsigned error){last_error_=error;call.return_to(state,0);return std::optional<std::uint32_t>{};};
        std::vector<std::uint8_t> bytes;
        if(provider==0x52534d42U){
            if(enumerate)bytes.resize(4);
            else {
                if(id)return fail(1168);const auto info=filesystem_.information("C:\\");
                bytes=virtual_firmware::smbios(memory.processor_count(),installed_physical_memory(memory),info.metadata.id^(info.metadata.device<<32),guest_processor_information(memory));
            }
        }else if(provider==0x41435049U || provider==0x4649524dU){
            const bool acpi=provider==0x41435049U;
            const std::string directory=std::string("C:/ProgramData/MacWin/Firmware/")+(acpi?"ACPI/":"FIRM/");
            const auto read_table=[&](const std::string& path,std::vector<std::uint8_t>& data)->unsigned{
                const auto info=filesystem_.information(path);if(info.error)return info.error;
                if(info.metadata.attributes&0x10U || info.metadata.size>(acpi?1024U*1024U:128U*1024U))return 13;
                const auto file=filesystem_.open(path);if(file.error)return file.error;if(!file.bytes)return 13;data=*file.bytes;
                return acpi && !virtual_firmware::acpi_valid(data)?13U:0U;
            };
            if(enumerate){
                const auto files=filesystem_.list(directory+(acpi?"*.dat":"*.bin"));
                if(files.error && files.error!=2 && files.error!=3)return fail(files.error);
                for(const auto& entry:files.entries){
                    if(entry.metadata.attributes&0x10U)continue;
                    std::vector<std::uint8_t> data;const auto error=read_table(directory+entry.name,data);if(error)return fail(error);
                    std::uint32_t table=0;
                    if(acpi){if(data.size()<4)return fail(13);for(unsigned i=0;i<4;++i)table|=std::uint32_t{data[i]}<<(i*8);}
                    else {
                        if(entry.name.size()!=12)continue;
                        bool valid=true;for(unsigned i=0;i<8;++i){auto c=entry.name[i];if(c>='A' && c<='F')c=static_cast<char>(c+32);const unsigned digit=c>='0' && c<='9'?static_cast<unsigned>(c-'0'):c>='a' && c<='f'?static_cast<unsigned>(10+c-'a'):16U;
                            if(digit>=16){valid=false;break;}table=(table<<4)|digit;}if(!valid)continue;
                    }
                    for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<std::uint8_t>(table>>(i*8)));
                }
            }else {
                if(acpi){
                    const auto files=filesystem_.list(directory+"*.dat");
                    if(files.error)return fail(files.error==2 || files.error==3?1168:files.error);
                    bool found=false;
                    for(const auto& entry:files.entries){
                        if(entry.metadata.attributes&0x10U)continue;
                        std::vector<std::uint8_t> data;const auto error=read_table(directory+entry.name,data);if(error)return fail(error);
                        std::uint32_t signature=0;for(unsigned i=0;i<4;++i)signature|=std::uint32_t{data[i]}<<(i*8);
                        if(signature==id){bytes=std::move(data);found=true;break;}
                    }
                    if(!found)return fail(1168);
                }else{
                    std::string filename;constexpr char hex[]="0123456789abcdef";
                    for(int shift=28;shift>=0;shift-=4)filename+=hex[(id>>shift)&15];filename+=".bin";
                    const auto error=read_table(directory+filename,bytes);if(error)return fail(error==2 || error==3?1168:error);
                }
            }
        }else return fail(1);
        if(buffer && capacity<bytes.size())last_error_=122;
        else {last_error_=0;if(buffer && !bytes.empty())memory.write(GuestAddress{buffer},bytes);}
        call.return_to(state,bytes.size());return std::nullopt;
    }
    if(module_api=="EnumProcesses" || module_api=="K32EnumProcesses"){
        const WindowsCall call(state,memory,service);std::uint32_t result=0,error=0;
        if(!call(2) || (static_cast<std::uint32_t>(call(1))>=4 && !call(0)))error=87;
        else {
            std::vector<std::uint32_t> ids;error=session_process_ids(ids);
            if(!error){
                const auto count=std::min<std::size_t>(ids.size(),static_cast<std::uint32_t>(call(1))/4);
                memory.check(GuestAddress{call(2)},4,Access::Write);memory.check(GuestAddress{call(0)},count*4,Access::Write);
                for(std::size_t n=0;n<count;++n)memory.store(GuestAddress{call(0)+n*4},32,ids[n]);
                memory.store(GuestAddress{call(2)},32,count*4);result=1;
            }
        }
        if(error)last_error_=error;call.return_to(state,result);return std::nullopt;
    }
    if(module_api=="SetDefaultDllDirectories" || module_api=="AddDllDirectory" || module_api=="RemoveDllDirectory")return invoke_module(state,memory,service);
    if(module_api.starts_with("QueryFullProcessImageName") || module_api.starts_with("GetProcessImageFileName") || module_api.starts_with("K32") || module_api.starts_with("EnumProcessModules") || module_api.starts_with("GetModule") || module_api.starts_with("LoadLibrary") || module_api.starts_with("GetDllDirectory") || module_api.starts_with("SetDllDirectory") || module_api=="GetProcAddress" || module_api=="FreeLibrary" || module_api=="DisableThreadLibraryCalls")return invoke_module(state,memory,service);
    if (service.symbol == "qsort" || service.symbol == "bsearch") return start_sort(state, memory, service.symbol=="bsearch");
    const WindowsCall arg(state, memory, service);
    const auto symbol = service.symbol;
    const auto dword = [&](unsigned i) { return static_cast<std::uint32_t>(arg(i)); };
    const auto sint = [&](unsigned i) { return std::bit_cast<std::int32_t>(dword(i)); };
    std::uint64_t result = 0;
    const auto fail = [&](unsigned error) { last_error_ = error; result = 0; };
    if(symbol=="GetCurrentApplicationUserModelId"){arg.return_to(state,arg(0)?15703U:87U);return std::nullopt;}
    if(symbol=="GetCurrentPackageId" || symbol=="GetCurrentPackageFullName" || symbol=="GetCurrentPackageFamilyName" || symbol=="GetCurrentPackagePath"){
        // PE 執行環境沒有 MSIX 套件身分；此錯誤描述目前程序的實際身分狀態。
        arg.return_to(state,arg(0)?15700U:87U);return std::nullopt;
    }
    if(symbol=="SHAutoComplete"){
        // 提供 HRESULT 入口；檔案／URL 自動完成尚未提供。
        arg.return_to(state,gui_.windows.contains(arg(0))?0x80004001U:0x80070057U);return std::nullopt;
    }
    if(symbol=="ExitWindowsEx" || symbol.starts_with("InitiateSystemShutdown") || symbol.starts_with("AbortSystemShutdown") ||
       symbol=="InitiateShutdownA" || symbol=="InitiateShutdownW")return invoke_shutdown(state,memory,service);
    const auto crt_fail = [&](unsigned error) { memory.store(GuestAddress{runtime_.errno_cell}, 32, error); result = UINT32_MAX; };
    if(symbol=="CreateNamedPipeA" || symbol=="CreateNamedPipeW") {
        return invoke_file_io(state,memory,service);
    }else if(symbol.find("NamedPipe")!=symbol.npos || symbol=="CancelIo" || symbol=="CancelIoEx" || symbol=="LockFileEx" || symbol=="UnlockFileEx" || symbol=="LockFile" || symbol=="UnlockFile") {
        return invoke_file_io(state,memory,service);
    }else if(symbol=="RtlCaptureStackBackTrace" || symbol=="CaptureStackBackTrace" || symbol=="RtlGetCallersAddress") {
        const std::uint8_t width=x86_?32:64;const unsigned bytes=width/8;
        const bool callers=symbol=="RtlGetCallersAddress";
        // GetCallersAddress 取呼叫此 API 的函式之 caller／parent；與 CaptureStackBackTrace 共用實際展開。
        const auto capacity=callers?2U:std::min(dword(1),65535U),skip=callers?1U:dword(0);std::uint32_t hash=0;
        const auto output=[&](std::uint64_t index){return callers?arg(static_cast<unsigned>(index)):arg(2)+index*bytes;};
        if(callers){memory.check(GuestAddress{arg(0)},bytes,Access::Write);memory.check(GuestAddress{arg(1)},bytes,Access::Write);}
        else if(capacity)memory.check(GuestAddress{arg(2)},std::uint64_t{capacity}*bytes,Access::Write);
        if(!callers && arg(3))memory.check(GuestAddress{arg(3)},4,Access::Write);
        auto walk=state;walk.rip=GuestAddress{memory.load(GuestAddress{walk.reg(Gpr::Rsp)},width)};walk.reg(Gpr::Rsp)+=bytes;
        const auto teb=x86_?state.fs_base:state.gs_base;
        const auto low=memory.load(GuestAddress{teb+(x86_?8:16)},width),high=memory.load(GuestAddress{teb+(x86_?4:8)},width);
        // x86 使用有界 EBP chain；x64 使用映像的 unwind metadata。不掃描猜測返回位址。
        for(std::uint64_t index=0;result<capacity && index<std::uint64_t{skip}+capacity;++index) {
            if(walk.reg(Gpr::Rsp)<low || walk.reg(Gpr::Rsp)>=high || handles(walk.rip) || !module_image(walk.rip.value,true))break;
            try {memory.check(walk.rip,1,Access::Execute);}catch(const MemoryFault&) {break;}
            if(index>=skip) {memory.store(GuestAddress{output(result)},width,walk.rip.value);hash+=static_cast<std::uint32_t>(walk.rip.value);++result;}
            try {
                const auto old_sp=walk.reg(Gpr::Rsp);
                if(x86_) {
                    const auto bp=walk.reg(Gpr::Rbp);
                    if(bp<old_sp || bp%4 || bp>high || high-bp<8)break;
                    walk.rip=GuestAddress{memory.load(GuestAddress{bp+4},32)};walk.reg(Gpr::Rsp)=bp+8;walk.reg(Gpr::Rbp)=memory.load(GuestAddress{bp},32);
                }else {
                    const auto* image=module_image(walk.rip.value,true);
                    walk=virtual_unwind(memory,*image,walk.rip.value,walk,0).caller;
                }
                if(walk.reg(Gpr::Rsp)<=old_sp)break;
            }catch(const MemoryFault&) {break;}catch(const HostCallFault&) {break;}
        }
        if(callers)while(result<capacity){memory.store(GuestAddress{output(result)},width,0);++result;}
        else if(arg(3))memory.store(GuestAddress{arg(3)},32,hash);
    }else if(symbol=="EncodePointer" || symbol=="DecodePointer" || symbol=="RtlEncodePointer" || symbol=="RtlDecodePointer" || symbol=="_encode_pointer" || symbol=="_decode_pointer") {
        if(!runtime_.pointer_cookie) {
            std::array<std::uint8_t,8> bytes{};
            if(!random_bytes(bytes))throw HostCallFault("無法取得程序指標編碼金鑰");
            for(unsigned i=0;i<8;++i)runtime_.pointer_cookie|=static_cast<std::uint64_t>(bytes[i])<<(i*8);
            runtime_.pointer_cookie|=UINT64_C(0x8000000000000000);
        }
        const auto rotation=static_cast<int>(runtime_.pointer_cookie&63);
        const bool encode=symbol=="EncodePointer" || symbol=="RtlEncodePointer" || symbol=="_encode_pointer";
        if(x86_) {
            const auto cookie=static_cast<std::uint32_t>(runtime_.pointer_cookie);
            const auto pointer=static_cast<std::uint32_t>(arg(0));
            result=encode?std::rotr(pointer^cookie,rotation&31):std::rotl(pointer,rotation&31)^cookie;
        } else result=encode?std::rotr(arg(0)^runtime_.pointer_cookie,rotation):std::rotl(arg(0),rotation)^runtime_.pointer_cookie;
    }else if(symbol.starts_with("PathFindExtension") || symbol.starts_with("PathFindFileName") || symbol.starts_with("PathIsRelative") || symbol.starts_with("PathIsUNC")){
        const bool wide=symbol.ends_with('W');
        // ANSI 的索引仍是原始位元組位移；不可經 UTF-8 解碼改變返回指標。
        std::u16string path;
        if(arg(0)) {
            if(wide)path=read_utf16(memory,GuestAddress{arg(0)},259);
            else {const auto bytes=read_ansi(memory,arg(0),259);path.assign(bytes.begin(),bytes.end());}
        }
        if(symbol.starts_with("PathIsUNC"))result=path.starts_with(u"\\\\");
        else if(symbol.starts_with("PathIsRelative"))result=path.empty() || (path.front()!=u'\\' && !(path.size()>1 && path[1]==u':'));
        else if(arg(0)) {
            std::size_t offset=0;
            if(symbol.starts_with("PathFindExtension")) {
                offset=windows_extension_offset(path);
            } else {
                for(std::size_t i=path.size();i>1;--i) {
                    const auto previous=path[i-2],current=path[i-1];
                    if((previous==u'\\' || previous==u'/' || previous==u':') && current!=u'\\' && current!=u'/') {offset=i-1;break;}
                }
            }
            result=arg(0)+offset*(wide?2U:1U);
        }
    }else if(symbol=="wcscmp" || symbol=="wcsncmp"){
        const auto count=symbol=="wcsncmp"?arg(2):output_limit/2;
        for(std::uint64_t i=0;i<count;++i){if(i>=output_limit/2)throw HostCallFault("寬字串比較超過上限");
            const auto left=memory.load(GuestAddress{arg(0)+i*2},16),right=memory.load(GuestAddress{arg(1)+i*2},16);
            if(left!=right){result=left<right?UINT32_MAX:1;break;}if(!left)break;
        }
    }else if(symbol=="wcsstr"){
        const auto pattern=read_utf16(memory,GuestAddress{arg(1)});if(pattern.empty())result=arg(0);
        else {const auto haystack=read_utf16(memory,GuestAddress{arg(0)});const auto at=haystack.find(pattern);if(at!=haystack.npos)result=arg(0)+at*2;}
    }else if(symbol=="wcschr" || symbol=="wcsrchr"){
        const auto text=read_utf16(memory,GuestAddress{arg(0)});const auto character=static_cast<char16_t>(arg(1));
        const auto at=!character?text.size():symbol=="wcschr"?text.find(character):text.rfind(character);if(at!=text.npos)result=arg(0)+at*2;
    }else if(symbol.starts_with("GetUserName")){
        const bool wide=symbol.ends_with('W'),extended=symbol.starts_with("GetUserNameEx");const auto buffer=arg(extended?1:0),size=arg(extended?2:1);
        if(!size || (extended && !arg(0)))fail(87);
        else if(extended && arg(0)!=2)fail(1332); // 本機帳號只有 SAM compatible 名稱，沒有網域 DN／UPN。
        else {
            initialize_tokens();const auto& sid=runtime_.token_objects.at(effective_token_id()).user.sid;
            const auto account=security::resolve_account(sid,runtime_.token_objects.at(1).user.sid);
            if(!account){fail(1332);arg.return_to(state,result);return std::nullopt;}
            const auto value=utf8_to_utf16(extended && !account->domain.empty()?std::string(account->domain)+"\\"+std::string(account->name):std::string(account->name));
            const auto text=utf16_to_utf8(value);const auto count=(wide?value.size():text.size())+1;
            const auto capacity=memory.load(GuestAddress{size},32);memory.check(GuestAddress{size},4,Access::Write);
            if(!buffer || capacity<count){memory.store(GuestAddress{size},32,count);fail(extended?234:122);}
            else {if(wide)memory.write(GuestAddress{buffer},utf16_bytes(value));else {std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);memory.write(GuestAddress{buffer},bytes);}memory.store(GuestAddress{size},32,extended?count-1:count);result=1;}
        }
    }
    else if(symbol.starts_with("GetLocaleInfo"))return invoke_locale(state,memory,service);
    else if(symbol=="IsValidCodePage") result=dword(0)>3 && windows_crt_codepage(dword(0)).has_value();
    else if(symbol=="GetOEMCP") result=65001;
    else if(symbol=="GetCPInfo") {
        const auto page=dword(0)==0 || dword(0)==1 || dword(0)==3?65001U:dword(0);const auto info=windows_crt_codepage(page);
        if(!info || !arg(1))fail(87);
        else{
            std::array<std::uint8_t,20> bytes{};bytes[0]=static_cast<std::uint8_t>(info->maximum_character_bytes);bytes[4]='?';
            unsigned at=6;bool valid=true;
            for(unsigned i=0;i<256;++i)if(info->lead[i]){const auto first=i;while(i+1<256 && info->lead[i+1])++i;if(at+2>18){valid=false;break;}bytes[at++]=static_cast<std::uint8_t>(first);bytes[at++]=static_cast<std::uint8_t>(i);}
            if(!valid)fail(87);else{memory.write(GuestAddress{arg(1)},bytes);result=1;}
        }
    }
    else if(symbol=="GetConsoleScreenBufferInfo")return invoke_console_control(state,memory,service);
    else if (symbol == "CommandLineToArgvW") {
        const std::uint8_t bits=x86_?32:64;const unsigned width=x86_?4:8;
        if(!arg(0) || !arg(1)){fail(87);arg.return_to(state,result);return std::nullopt;}
        const auto text = read_utf16(memory, GuestAddress{arg(0)});
        auto values = parse_windows_arguments(text);
        if (text.empty()) {
            const auto argv = memory.load(GuestAddress{runtime_.wargv_cell}, bits);
            const auto first = memory.load(GuestAddress{argv}, bits);
            values = {first ? read_utf16(memory, GuestAddress{first}) : std::u16string{}};
        } else if (text.front() == u' ' || text.front() == u'\t') values.insert(values.begin(), u"");
        memory.check(GuestAddress{arg(1)}, 4, Access::Write);
        std::size_t size = (values.size() + 1) * width;
        for (const auto& value : values) size += (value.size() + 1) * 2;
        result = allocate_heap(memory, size);
        if (!result) fail(8);
        else {
            auto cursor = result + (values.size() + 1) * width;
            for (std::size_t i = 0; i < values.size(); ++i) {
                memory.store(GuestAddress{result + i * width}, bits, cursor);
                const auto bytes = utf16_bytes(values[i]); memory.write(GuestAddress{cursor}, bytes); cursor += bytes.size();
            }
            memory.store(GuestAddress{result+values.size()*width},bits,0);
            runtime_.local_allocations.insert(result); memory.store(GuestAddress{arg(1)}, 32, values.size());
        }
    } else if (symbol.starts_with("Local") && (symbol!="LocalFree" || gui_.global_memory.contains(arg(0)))) {
        const auto target=symbol=="LocalAlloc"?"GlobalAlloc":symbol=="LocalReAlloc"?"LocalReAlloc":symbol=="LocalFree"?"GlobalFree":symbol=="LocalLock"?"GlobalLock":symbol=="LocalUnlock"?"GlobalUnlock":symbol=="LocalFlags"?"GlobalFlags":symbol=="LocalSize"?"GlobalSize":"GlobalHandle";
        return invoke_gui_extra(state,memory,{"kernel32.dll",target,service.arguments});
    } else if (symbol == "LocalFree") {
        if (!arg(0)) result = 0;
        else if (!runtime_.local_allocations.contains(arg(0)) || !free_heap(memory, arg(0), state)) {last_error_ = 6; result = arg(0);}
        else runtime_.local_allocations.erase(arg(0));
    } else if(symbol=="GetUserGeoID") {result=UINT32_MAX;if(dword(0)!=16 && dword(0)!=14)last_error_=87;}
    else if(symbol=="GetGeoInfoA" || symbol=="GetGeoInfoW") {if(!dword(0) || dword(0)==UINT32_MAX || !dword(1) || dword(1)>16 || sint(3)<0 || (arg(3) && !arg(2)))fail(87);else fail(50);}
    else if(symbol=="GetUserDefaultLocaleName" || symbol=="GetSystemDefaultLocaleName") {if(!arg(0) || sint(1)<=0)fail(87);else if(dword(1)<6)fail(122);else {memory.write(GuestAddress{arg(0)},utf16_bytes(u"en-US"));result=6;}}
    else if(symbol=="GetSystemDefaultLangID" || symbol=="GetUserDefaultLangID" || symbol=="GetThreadUILanguage" || symbol=="GetUserDefaultUILanguage" || symbol=="GetSystemDefaultUILanguage" || symbol=="GetSystemDefaultLCID" || symbol=="GetUserDefaultLCID" || symbol=="GetThreadLocale")result=0x409;
    else if(symbol=="GetThreadPreferredUILanguages" || symbol=="GetUserPreferredUILanguages" || symbol=="GetSystemPreferredUILanguages") {
        const bool thread=symbol=="GetThreadPreferredUILanguages",system=symbol=="GetSystemPreferredUILanguages";
        const auto flags=dword(0),allowed=thread?0x7cU:system?0x40cU:0xcU;
        if(!arg(1) || !arg(3) || (flags&~allowed) || (flags&0xc)==0xc ||
           (thread && (((flags&0x10) && !(flags&0x20)) || ((flags&0x40) && (flags&0x30)))))fail(87);
        else {
            const auto capacity=memory.load(GuestAddress{arg(3)},32);
            memory.check(GuestAddress{arg(1)},4,Access::Write);memory.check(GuestAddress{arg(3)},4,Access::Write);
            std::vector<std::u16string> languages;
            // 尚未設定個別 thread 偏好；預設融合 user/system 的 en-US。
            if(!thread || !(flags&0x40)) {
                languages.push_back(flags&4?u"0409":u"en-US");
                if(thread && (flags&0x10))languages.push_back(flags&4?u"0009":u"en");
            }
            std::u16string list;for(const auto& language:languages){list+=language;list.push_back(0);}if(list.empty())list.push_back(0);
            const auto bytes=utf16_bytes(list);const auto required=bytes.size()/2;
            memory.store(GuestAddress{arg(1)},32,languages.size());memory.store(GuestAddress{arg(3)},32,required);
            if(!arg(2) && !capacity)result=1;
            else if(!arg(2) || capacity<required)fail(122);
            else {memory.write(GuestAddress{arg(2)},bytes);result=1;}
        }
    }
    else if (symbol == "SetFileApisToOEM" || symbol == "SetFileApisToANSI")runtime_.file_apis_ansi=symbol=="SetFileApisToANSI";
    else if(symbol=="AreFileApisANSI")result=runtime_.file_apis_ansi;
    else if(symbol=="GetLargePageMinimum")result=0; // 虛擬配置僅提供一般頁面，沒有 large-page pool。
    else if (symbol == "SetConsoleCtrlHandler") {
        if (!arg(0)) {runtime_.ignore_ctrl_c = dword(1) != 0; result = 1;}
        else if (dword(1)) {
            memory.check(GuestAddress{arg(0)}, 1, Access::Execute);
            if (runtime_.console_handlers.size() >= 128) fail(8);
            else {runtime_.console_handlers.insert(runtime_.console_handlers.begin(), arg(0)); result = 1;}
        } else {
            const auto found = std::find(runtime_.console_handlers.begin(), runtime_.console_handlers.end(), arg(0));
            if (found == runtime_.console_handlers.end()) fail(87);
            else {runtime_.console_handlers.erase(found); result = 1;}
        }
    } else if (symbol == "ProcessPrng") {
        result=fill_random(memory,arg(0),arg(1))?1:0;
    } else if (symbol == "RtlGetCurrentPeb") result=peb_base_.value;
    else if (symbol == "GetProductInfo") {
        // 與虛擬 Windows 10 workstation 一致，不查詢宿主授權或版本。
        if(!arg(4))fail(87);
        else {memory.store(GuestAddress{arg(4)},32,0);if(dword(0)<6)fail(87);else {memory.store(GuestAddress{arg(4)},32,0x30);result=1;}}
    }
    else if(symbol=="IsOS"){
        // 與版本／產品及 IsWow64Process 共用同一個 Windows 10 Professional workstation 設定。
        // 此環境未啟用網域、Terminal Services、快速使用者切換或特殊裝置套件。
        result=dword(0)==1 || dword(0)==7 || dword(0)==8 || dword(0)==20 || (dword(0)==30 && x86_);
    }
    else if (symbol == "GetVersion") result=(windows_version_build<<16)|(windows_version_minor<<8)|windows_version_major;
    else if (symbol == "RtlGetVersion" || symbol == "GetVersionExA" || symbol == "GetVersionExW") {
        const auto size=memory.load(GuestAddress{arg(0)},32);
        const bool ansi=symbol=="GetVersionExA", native=symbol=="RtlGetVersion";const auto base_size=ansi?148U:276U;
        if (size != base_size && size != base_size+8U) {if (native) result=0xc000000d;else fail(87);}
        else {
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));memory.write(GuestAddress{arg(0)},bytes);
            memory.store(GuestAddress{arg(0)},32,size);memory.store(GuestAddress{arg(0)+4},32,windows_version_major);
            memory.store(GuestAddress{arg(0)+8},32,windows_version_minor);
            memory.store(GuestAddress{arg(0)+12},32,windows_version_build);memory.store(GuestAddress{arg(0)+16},32,2);
            if (size==base_size+8U) memory.store(GuestAddress{arg(0)+base_size+6U},8,1);
            result=native?0:1;
        }
    } else if (symbol == "RtlGetNtVersionNumbers") {
        // checked/free build 標記在 build DWORD 高四位；此虛擬環境為 free build。
        for(unsigned i=0;i<3;++i)if(arg(i))memory.store(GuestAddress{arg(i)},32,i==0?windows_version_major:i==1?windows_version_minor:0xf0000000U|windows_version_build);
    } else if (symbol == "VerSetConditionMask") {
        // 每欄三位元；Windows 的多 bit 輸入只選最高有效欄，重複設定採 OR。
        result=arg(0);const auto types=dword(1)&0xffU;
        if(types) result|=(arg(2)&7U)<<(3U*(static_cast<unsigned>(std::bit_width(types))-1U));
    } else if (symbol == "VerifyVersionInfoA" || symbol == "VerifyVersionInfoW" || symbol == "RtlVerifyVersionInfo") {
        const bool native=symbol=="RtlVerifyVersionInfo", ansi=symbol=="VerifyVersionInfoA";
        const auto extra=ansi?148U:276U, types=dword(1);const auto mask=arg(2);
        std::uint32_t error=0;
        if(!arg(0) || !types || (types&~0xffU) || !mask || memory.load(GuestAddress{arg(0)},32)!=extra+8U)error=87;
        const auto condition=[&](unsigned index){return static_cast<unsigned>((mask>>(index*3U))&7U);};
        const auto compare=[](std::uint64_t actual,std::uint64_t wanted,unsigned op){
            switch(op){case 1:return actual==wanted;case 2:return actual>wanted;case 3:return actual>=wanted;
            case 4:return actual<wanted;case 5:return actual<=wanted;default:return false;}
        };
        // 與 GetVersionEx/RtlGetVersion 相同的虛擬 OS 設定；不從 macOS 版本推測。
        for(const unsigned index:{7U,6U,3U,2U}){
            if(error || !(types&(1U<<index)))continue;
            const auto op=condition(index);
            const auto offset=index==7?extra+6U:index==6?extra+4U:index==3?16U:12U;
            const auto wanted=memory.load(GuestAddress{arg(0)+offset},index==7?8:index==6?16:32);
            if(index==6){if(op!=6 && op!=7)error=87;else if(wanted)error=1150;}
            else if(op<1 || op>5)error=87;
            else if(!compare(index==7?1U:index==3?2U:windows_version_build,wanted,op))error=1150;
        }
        // major/minor/SP 是同一版本 tuple；不同的高階值決定結果，不再比較低階。
        unsigned direction=0;bool matches=true;
        for(const unsigned index:{1U,0U,5U,4U}){
            if(error || !(types&(1U<<index)))continue;
            const auto requested=condition(index);unsigned op=requested;
            if(requested<1 || requested>5){error=87;break;}
            if(!direction || direction==1)direction=requested;
            else if((direction==2 || direction==3) && requested>=4)op=direction;
            else if((direction==4 || direction==5) && (requested==2 || requested==3))op=direction;
            const auto offset=index==1?4U:index==0?8U:index==5?extra:extra+2U;
            const auto wanted=memory.load(GuestAddress{arg(0)+offset},index<=1?32:16);
            const std::uint64_t actual=index==1?windows_version_major:index==0?windows_version_minor:0U;
            matches=compare(actual,wanted,op);
            if(actual!=wanted)break;
        }
        if(!error && !matches)error=1150;
        if(native)result=error==87?0xc000000dU:error?0xc0000059U:0U;
        else if(error)fail(error);else result=1;
    } else if (symbol == "GetSystemInfo" || symbol == "GetNativeSystemInfo") {
        if(x86_) {
            memory.write(GuestAddress{arg(0)},std::array<std::uint8_t,36>{});
            memory.store(GuestAddress{arg(0)},16,symbol=="GetNativeSystemInfo"?9:0);
            memory.store(GuestAddress{arg(0)+4},32,4096);memory.store(GuestAddress{arg(0)+8},32,65536);
            memory.store(GuestAddress{arg(0)+12},32,user_address_limit()-1);memory.store(GuestAddress{arg(0)+16},32,(UINT64_C(1)<<runtime_.processor_count)-1);
            memory.store(GuestAddress{arg(0)+20},32,runtime_.processor_count);memory.store(GuestAddress{arg(0)+24},32,586);
            memory.store(GuestAddress{arg(0)+28},32,65536);memory.store(GuestAddress{arg(0)+32},16,6);
        } else {
        memory.write(GuestAddress{arg(0)},std::array<std::uint8_t,48>{});
        for (const auto [offset,value] : std::array<std::pair<unsigned,std::uint64_t>,8>{{{0,9},{4,4096},{32,runtime_.processor_count},{36,8664},{40,65536},{44,6},{46,0},{2,0}}})
            memory.store(GuestAddress{arg(0)+offset},offset==0 || offset==2 || offset>=44 ? 16 : 32,value);
        memory.store(GuestAddress{arg(0)+8},64,65536);memory.store(GuestAddress{arg(0)+16},64,user_address_limit()-1);
        memory.store(GuestAddress{arg(0)+24},64,runtime_.processor_count==64?UINT64_MAX:(UINT64_C(1)<<runtime_.processor_count)-1);
        }
    } else if (symbol.starts_with("GetSystemDirectory") || symbol.starts_with("GetSystemWow64Directory") || symbol.starts_with("GetWindowsDirectory") || symbol.starts_with("GetSystemWindowsDirectory")) {
        // 與 GetNativeSystemInfo／IsWow64Process 共用 64 位元客體 OS 設定及既有目錄／緩衝區規則。
        const auto text=utf16_to_utf8(symbol.starts_with("GetSystemWow64Directory") ? windows_wow64_directory : symbol.starts_with("GetSystemDirectory") ? windows_system_directory : windows_directory);
        result=text.size()+1;
        if (dword(1)>=result) {
            if (symbol.back()=='W') memory.write(GuestAddress{arg(0)},utf16_bytes(utf8_to_utf16(text)));
            else {std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);memory.write(GuestAddress{arg(0)},bytes);}
            --result;
        }
    } else if (symbol == "WerGetFlags") {
        if (arg(0)!=UINT64_MAX) result=0x80070006;else memory.store(GuestAddress{arg(1)},32,runtime_.wer_flags);
    } else if (symbol == "WerSetFlags") {
        if (dword(0)&~0xffU) result=0x80070057;else runtime_.wer_flags=dword(0);
    } else if(symbol=="PowerGetActiveScheme") {
        return invoke_power(state,memory,service);
    } else if(symbol=="CallNtPowerInformation")return invoke_power(state,memory,service);
    else if(symbol=="UuidToStringA" || symbol=="UuidToStringW") {
        const bool wide=symbol.ends_with('W');const std::uint8_t width=x86_?32:64;
        memory.check(GuestAddress{arg(1)},width/8,Access::Write);
        std::string text;static constexpr char hex[]="0123456789abcdef";
        for(const unsigned offset:std::array<unsigned,16>{3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15}){
            if(text.size()==8 || text.size()==13 || text.size()==18 || text.size()==23)text+='-';
            const auto byte=arg(0)?memory.load(GuestAddress{arg(0)+offset},8):0;text+=hex[byte>>4];text+=hex[byte&15];
        }
        const auto pointer=allocate_heap(memory,wide?74:37);
        if(!pointer)result=14;
        else {
            if(wide)memory.write(GuestAddress{pointer},utf16_bytes(utf8_to_utf16(text)));
            else {text.push_back(0);memory.write(GuestAddress{pointer},std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));}
            memory.store(GuestAddress{arg(1)},width,pointer);
        }
    } else if(symbol=="RpcStringFreeA" || symbol=="RpcStringFreeW") {
        const std::uint8_t width=x86_?32:64;memory.check(GuestAddress{arg(0)},width/8,Access::Write);
        if(!free_heap(memory,memory.load(GuestAddress{arg(0)},width),state))result=87;
        else memory.store(GuestAddress{arg(0)},width,0);
    } else if(symbol=="UuidCreate" || symbol=="CoCreateGuid") {
        memory.check(GuestAddress{arg(0)},16,Access::Write);
        if(!fill_random(memory,arg(0),16))result=symbol=="UuidCreate"?14U:0x8007000eU;
        else {
            memory.store(GuestAddress{arg(0)+7},8,(memory.load(GuestAddress{arg(0)+7},8)&15)|64);
            memory.store(GuestAddress{arg(0)+8},8,(memory.load(GuestAddress{arg(0)+8},8)&63)|128);
        }
    } else if(symbol=="SetHandleCount")result=dword(0); // NT 系統不使用 DOS handle table 限制。
    else if(symbol=="GetThreadErrorMode" || symbol=="SetThreadErrorMode"){
        auto& mode=threads_.items.at(threads_.current).error_mode;
        if(symbol=="GetThreadErrorMode")result=mode.value_or(runtime_.error_mode);
        else if(dword(0)&~0x8003U)fail(87);
        else {if(arg(1))memory.store(GuestAddress{arg(1)},32,mode.value_or(runtime_.error_mode));mode=dword(0);result=1;}
    }
    else if (symbol == "GetErrorMode" || symbol == "SetErrorMode") {result=runtime_.error_mode;if (symbol=="SetErrorMode") runtime_.error_mode=dword(0);}
    else if (symbol == "_getcwd" || symbol == "_wgetcwd" || symbol == "_fullpath" || symbol == "_wfullpath" || symbol.starts_with("GetFullPathName") || symbol.starts_with("GetCurrentDirectory")) {
        const bool crt = symbol.front() == '_', current = symbol.ends_with("getcwd") || symbol.starts_with("GetCurrentDirectory");
        const bool wide = symbol == "_wgetcwd" || symbol == "_wfullpath" || symbol.back() == 'W';
        const auto pointer = current ? 0 : crt ? arg(1) : arg(0);
        std::u16string path;bool device=false;
        try {
            auto input=pointer ? (wide ? read_utf16(memory,GuestAddress{pointer}) : utf8_to_utf16(read_ansi(memory,pointer))) : std::u16string{};
            if(current || (crt && input.empty())) path=runtime_.current_directory;
            else {
                device=windows_dos_device(input)!=0;
                path=full_windows_environment_path(std::move(input),runtime_.current_directory,runtime_.environment);
            }
        }
        catch (const std::invalid_argument&) { if (crt) { memory.store(GuestAddress{runtime_.errno_cell}, 32, 22); result = 0; } else fail(123); arg.return_to(state, result); return std::nullopt; }
        std::vector<std::uint8_t> bytes;
        if (wide) bytes = utf16_bytes(path);
        else { const auto text = utf16_to_utf8(path); bytes.assign(text.begin(), text.end()); bytes.push_back(0); }
        const auto count = bytes.size() / (wide ? 2U : 1U);
        auto destination = crt ? arg(0) : arg(current ? 1 : 2); const auto capacity = crt ? arg(current ? 1 : 2) : dword(current ? 0 : 1);
        if (crt && !destination) {
            destination = allocate_heap(memory, bytes.size());
            if (!destination) { memory.store(GuestAddress{runtime_.errno_cell}, 32, 12); result = 0; }
            else { memory.write(GuestAddress{destination}, bytes); result = destination; }
        } else if (capacity < count || !destination) {
            if (crt) { memory.store(GuestAddress{runtime_.errno_cell}, 32, 34); result = 0; }
            else result = count;
        } else {
            if (!crt && !current && arg(3)) memory.check(GuestAddress{arg(3)}, x86_?4:8, Access::Write);
            memory.write(GuestAddress{destination}, bytes); result = crt ? destination : count - 1;
            if (!crt && !current && arg(3)) {
                const auto part=windows_path_file_part(path,device);
                const auto offset=part==path.npos?0:wide?part*2:utf16_to_utf8(path.substr(0,part)).size();
                memory.store(GuestAddress{arg(3)},x86_?32:64,part==path.npos?0:destination+offset);
            }
        }
    } else if (symbol == "DuplicateHandle") {
#include "windows_wait_completion_duplicate.inc"
        if(bluetooth_handle(arg(1)) && current_process_handle(arg(0)))return duplicate_bluetooth(state,memory,service);
        if(power_requests_.contains(arg(1)) && current_process_handle(arg(0)) && (!arg(2) || current_process_handle(arg(2))))return invoke_power(state,memory,service);
        if(job_handle(arg(1)) && current_process_handle(arg(0)) && current_process_handle(arg(2)))return invoke_job(state,memory,service);
        if(transaction_handle(arg(1)) && current_process_handle(arg(0)) && arg(2) && current_process_handle(arg(2))){
            if(dword(6)&~3U)fail(87);else if(!arg(3))fail(87);
            else {memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);const auto copy=duplicate_transaction(arg(1));runtime_.handle_flags[copy]=arg(5)?1:0;
                memory.store(GuestAddress{arg(3)},x86_?32:64,copy);if(dword(6)&1U){(void)close_transaction(arg(1));runtime_.handle_flags.erase(arg(1));}result=1;}
            arg.return_to(state,result);return std::nullopt;
        }
        if(!current_process_handle(arg(0)) || (arg(2) && !current_process_handle(arg(2))))return duplicate_remote_handle(state,memory,service);
        if(const auto source=arg(1);source>=0x100 && source<=0x102 && standard_open_[source-0x100]){
            // 標準串流以 0x100～0x102 代表宿主即時描述元（CRT fd 0～2 共用同一狀態）。同程序複本沿用同一串流值並
            // 增加參照數，寫入、型別與重新導向保持一致；跨程序仍由 duplicate_remote_handle 以描述元傳遞。
            const auto stream=static_cast<unsigned>(source-0x100);const bool close_source=(dword(6)&1U)!=0;
            if(!current_process_handle(arg(0),0x40) || (arg(2) && !current_process_handle(arg(2),0x40)))fail(5);
            else if(dword(6)&~3U)fail(87);
            else if(close_source && (runtime_.handle_flags[source]&2U))fail(5);
            else if(!arg(2)){
                if(!close_source)fail(87);
                else {if(standard_duplicates_[stream])--standard_duplicates_[stream];else standard_open_[stream]=false;result=1;}
            }else if(!arg(3))fail(87);
            else{
                memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);memory.store(GuestAddress{arg(3)},x86_?32:64,source);
                if(!close_source)++standard_duplicates_[stream];
                if(arg(5))runtime_.handle_flags[source]|=1U;
                result=1;
            }
            arg.return_to(state,result);return std::nullopt;
        }
        if(const auto event=runtime_.wait_objects.find(arg(1));event!=runtime_.wait_objects.end() && (event->second.kind==WaitObject::Kind::Event || event->second.kind==WaitObject::Kind::MemoryResource || event->second.kind==WaitObject::Kind::Mutex)){
            std::uint32_t access=event->second.access,access_error=0;
            if(!(dword(6)&2U)){try{initialize_tokens();const auto mutex=event->second.kind==WaitObject::Kind::Mutex;const auto all=mutex?0x1f0001U:0x1f0003U;const security::AccessMapping mapping{0x20001U,mutex?0x20001U:0x20002U,0x120000U,all};const auto descriptor=mutex?(runtime_.mutex_objects.at(event->second.mutex).shared?shared_object_security(runtime_.mutex_objects.at(event->second.mutex).shared->descriptor()):runtime_.mutex_objects.at(event->second.mutex).security_descriptor):(event->second.shared_event?shared_object_security(event->second.shared_event->descriptor()):event->second.security_descriptor);access=security::duplicate_object_access(runtime_.token_objects.at(effective_token_id()),security::parse_relative_descriptor(descriptor),access,dword(4),mapping);}catch(const SecurityDescriptorError& failure){access_error=failure.code;}}
            if(event->second.kind==WaitObject::Kind::MemoryResource && (access&~0x100001U))access_error=5;
            const auto source=arg(1);const bool close_source=(dword(6)&1U) && current_process_handle(arg(0),0x40) && !(runtime_.handle_flags[source]&2U);
            const auto close=[&]{if(close_source)(void)close_wait_object(source);};struct Cleanup{const decltype(close)& action;~Cleanup(){action();}} cleanup{close};
            if(!current_process_handle(arg(0),0x40) || (arg(2) && !current_process_handle(arg(2),0x40)))fail(5);
            else if(dword(6)&~3U)fail(87);
            else if((dword(6)&1U) && !close_source)fail(5);
            else if(!arg(2)){if(close_source)result=1;else fail(87);}
            else if(!arg(3))fail(87);
            else if(access_error)fail(access_error);
            else {memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);InheritedHandle item;const auto error=export_transfer_handle(arg(1),item);
                if(error)fail(error);else {const auto handle=runtime_.allocate_wait_handle();auto copy=event->second;copy.access=access;runtime_.wait_objects.emplace(handle,std::move(copy));runtime_.handle_flags[handle]=arg(5)?1:0;memory.store(GuestAddress{arg(3)},x86_?32:64,handle);result=1;}}
            arg.return_to(state,result);return std::nullopt;
        }
        if(mapping_handle(arg(1)))return invoke_mapping(state,memory,service);
        if(const auto source_object=runtime_.wait_objects.find(arg(1));source_object!=runtime_.wait_objects.end() && source_object->second.identity){
            const auto source_handle=arg(1);
            const bool close_source=(dword(6)&1U)!=0 && !(runtime_.handle_flags[source_handle]&2U);
            const auto close=[&]{if(close_source)(void)close_wait_object(source_handle);};
            struct CloseSource {const decltype(close)& action;~CloseSource(){action();}} cleanup{close};
            if(!current_process_handle(arg(0),0x40) || (arg(2) && !current_process_handle(arg(2),0x40)))fail(5);
            else if((dword(6)&1U) && !close_source)fail(5);
            else if((dword(6)&~3U) || (!arg(2) && !close_source))fail(87);
            else if(!arg(2))result=1;
            else if((dword(6)&~3U) || !arg(3))fail(87);
            else if(!(dword(6)&2U) && (dword(4)&~source_object->second.access))fail(5);
            else if(runtime_.wait_objects.size()>=process_handle_limit)fail(8);
            else {
                memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);const auto handle=runtime_.allocate_wait_handle();auto copy=source_object->second;if(!(dword(6)&2U))copy.access=dword(4);
                try {runtime_.wait_objects.emplace(handle,std::move(copy));runtime_.handle_flags[handle]=arg(5)?1:0;memory.store(GuestAddress{arg(3)},x86_?32:64,handle);}
                catch(...) {runtime_.wait_objects.erase(handle);runtime_.handle_flags.erase(handle);throw;}
                result=1;
            }
            arg.return_to(state,result);return std::nullopt;
        }
        if(arg(1)==(x86_?UINT32_MAX:UINT64_MAX) || process_signaled(arg(1)).has_value())return invoke_process(state,memory,service);
        if(const auto file=files_.find(arg(1));file!=files_.end()) {
            const auto source_handle=arg(1),object=file->second.object;const bool close_source=(dword(6)&1U)!=0;
            const auto close=[&] {
                if(!close_source || arg(0)!=(x86_?UINT32_MAX:UINT64_MAX) || (runtime_.handle_flags[source_handle]&2U))return;files_.erase(source_handle);runtime_.handle_flags.erase(source_handle);
                if(std::none_of(files_.begin(),files_.end(),[&](const auto& entry){return entry.second.object==object;}))runtime_.win_file_positions.erase(object);
            };
            struct CloseSource {const decltype(close)& action;~CloseSource(){action();}} cleanup{close};
            if(arg(0)!=(x86_?UINT32_MAX:UINT64_MAX) || (arg(2)!=(x86_?UINT32_MAX:UINT64_MAX) && !(close_source && !arg(2))))fail(6);
            else if(dword(6)&~3U)fail(87);
            else if(close_source && (runtime_.handle_flags[source_handle]&2U))fail(5);
            else if(!arg(2))result=1;
            else if(!(dword(6)&2U) && (security::mapped_access(dword(4),{0x120089U,0x120116U,0x1200a0U,0x1f01ffU})&~security::mapped_access(file->second.access,{0x120089U,0x120116U,0x1200a0U,0x1f01ffU})))fail(5);
            else if(files_.size()>=1024)fail(4);
            else {
                if(arg(3))memory.check(GuestAddress{arg(3)},x86_?4:8,Access::Write);
                const auto handle=runtime_.allocate_wait_handle();
                try {
                    auto duplicate=file->second;if(!(dword(6)&2U))duplicate.access=security::mapped_access(dword(4),{0x120089U,0x120116U,0x1200a0U,0x1f01ffU});
                    runtime_.handle_flags.emplace(handle,dword(5)?1U:0U);files_.emplace(handle,std::move(duplicate));
                    if(arg(3))memory.store(GuestAddress{arg(3)},x86_?32:64,handle);result=1;
                } catch(const std::bad_alloc&) {runtime_.handle_flags.erase(handle);files_.erase(handle);fail(8);}
            }
            arg.return_to(state,result);return std::nullopt;
        }
        const auto source = thread_id(arg(1));
        const auto token=runtime_.token_handles.find(arg(1));
        if(!current_process_handle(arg(0),0x40) || !current_process_handle(arg(2),0x40) || (!source && token==runtime_.token_handles.end()))fail(6);
        else if ((dword(6) & ~3U) || !arg(3)) fail(87);
        else if(!(dword(6)&2U) && (dword(4)&~(token!=runtime_.token_handles.end()?token->second:thread_access(arg(1)))))fail(5);
        else if((dword(6)&1U) && (runtime_.handle_flags[arg(1)]&2U))fail(5);
        else if (runtime_.process_thread_handles.size() >= 1024) fail(8);
        else {
            memory.check(GuestAddress{arg(3)}, x86_?4:8, Access::Write);
            const auto handle = runtime_.allocate_wait_handle();
            if(token!=runtime_.token_handles.end()){runtime_.token_handles.emplace(handle,(dword(6)&2U)?token->second:dword(4));runtime_.token_handle_objects[handle]=runtime_.token_handle_objects.at(arg(1));runtime_.handle_flags[handle]=dword(5)?1:0;}
            else {runtime_.process_thread_handles.emplace(handle, std::pair{source, dword(5) != 0});runtime_.thread_handle_access[handle]=(dword(6)&2U)?thread_access(arg(1)):dword(4);runtime_.handle_flags[handle]=dword(5)?1:0;}
            memory.store(GuestAddress{arg(3)}, x86_?32:64, handle);
            if (dword(6) & 1){runtime_.process_thread_handles.erase(arg(1));runtime_.thread_handle_access.erase(arg(1));runtime_.token_handles.erase(arg(1));runtime_.token_handle_objects.erase(arg(1));runtime_.handle_flags.erase(arg(1));}result = 1;
        }
    } else if(symbol=="GetThreadPriorityBoost" || symbol=="SetThreadPriorityBoost"){
        const auto id=thread_id(arg(0));const bool query=symbol=="GetThreadPriorityBoost";
        if(!id)fail(6);
        else if(!(thread_access(arg(0))&(query?0x840U:0x420U)))fail(5);
        else if(query && !arg(1))fail(87);
        else {auto& thread=threads_.items.at(id);
            if(query)memory.store(GuestAddress{arg(1)},32,thread.priority_boost_disabled?1:0);
            else {thread.priority_boost_disabled=dword(1)!=0;if(thread.priority_boost_disabled)thread.dynamic_priority_boost=0;}
            result=1;
        }
    } else if (symbol == "GetThreadPriority" || symbol == "SetThreadPriority") {
        const auto id = thread_id(arg(0));
        if (!id) { fail(6); if (symbol == "GetThreadPriority") result = INT32_MAX; }
        else if(!(thread_access(arg(0))&(symbol=="GetThreadPriority"?0x840U:0x420U))){fail(5);if(symbol=="GetThreadPriority")result=INT32_MAX;}
        else if (symbol == "GetThreadPriority") result = static_cast<std::uint32_t>(threads_.items.at(id).priority);
        else if ((sint(1) < -2 || sint(1) > 2) && sint(1) != -15 && sint(1) != 15) fail(87);
        else { threads_.items.at(id).priority = sint(1); result = 1; }
    } else if (symbol == "_lock_file" || symbol == "_unlock_file" || symbol=="_lock" || symbol=="_unlock" || symbol=="_lock_locales" || symbol=="_unlock_locales") {
        const bool locale=symbol=="_lock_locales" || symbol=="_unlock_locales";
        if(locale && guest_locale_lock_state_.value){
            // 客體快速路徑啟用時，狀態格是唯一事實來源；此處處理競爭、遞迴上限與有等待者的釋放。
            const GuestAddress owner_cell=guest_locale_lock_state_,recursion_cell{owner_cell.value+4},waiter_cell{owner_cell.value+8};
            const auto self=thread_identifier(threads_.current);
            const auto owner=static_cast<std::uint32_t>(memory.load(owner_cell,32));
            const auto recursion=static_cast<std::uint32_t>(memory.load(recursion_cell,32));
            if(!self)throw HostCallFault("執行緒缺少公開識別碼");
            if(symbol=="_unlock_locales"){
                if(owner!=self || !recursion)throw HostCallFault("執行緒未持有 CRT lock");
                memory.store(recursion_cell,32,recursion-1);if(recursion==1)memory.store(owner_cell,32,0);
            }else if(!owner || owner==self){
                if(recursion==UINT32_MAX)throw HostCallFault("CRT lock 遞迴過深");
                memory.store(owner_cell,32,self);memory.store(recursion_cell,32,owner?recursion+1:1);
            }else{
                memory.store(waiter_cell,32,memory.load(waiter_cell,32)+1);
                auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};
                thread.wait.kind=ThreadWaitKind::CrtLock;thread.wait.handles={UINT64_MAX};
                schedule(state,memory);return std::nullopt;
            }
        } else {
        const auto stream = locale?0:crt_stream(arg(0)), base = runtime_base_.value + 0x800;
        const bool internal=locale || symbol=="_lock" || symbol=="_unlock",unlock=symbol.starts_with("_unlock");
        if(internal && !locale && dword(0)>=1024)throw HostCallFault("CRT lock 編號超過上限");
        if (!internal && !runtime_.streams.contains(stream) && !(stream >= base && stream <= base + 0x100 && (stream - base) % 128 == 0)) throw HostCallFault("FILE 鎖定收到無效串流");
        const auto key=locale?UINT64_MAX:internal?(UINT64_C(1)<<63)|dword(0):stream;auto& lock=runtime_.file_locks[key];
        if(unlock) {
            if(lock.owner!=threads_.current || !lock.recursion)throw HostCallFault("執行緒未持有 CRT lock");
            if(!--lock.recursion)lock.owner=0;
        } else if(!lock.owner || lock.owner==threads_.current) {
            if(lock.recursion==UINT32_MAX)throw HostCallFault("CRT lock 遞迴過深");lock.owner=threads_.current;++lock.recursion;
        } else {
            auto& thread=threads_.items.at(threads_.current);thread.status=ThreadStatus::Waiting;thread.wait={};thread.wait.kind=ThreadWaitKind::CrtLock;thread.wait.handles={key};
            schedule(state,memory);return std::nullopt;
        }
        }
    } else if (symbol == "fputc" || symbol == "putc" || symbol == "_write") {
        const bool write = symbol == "_write";
        const auto fd = write ? dword(0) : crt_stream(arg(1)) == runtime_base_.value + 0x880 ? 1U : crt_stream(arg(1)) == runtime_base_.value + 0x900 ? 2U : UINT32_MAX;
        if (fd < 1 || fd > 2 || !standard_open_[fd]) crt_fail(9);
        else {
            const auto count = write ? dword(2) : 1U;
            if (count > output_limit) throw HostCallFault("CRT 串流單次輸出超過上限");
            std::vector<std::uint8_t> bytes(count);
            if (write) memory.read(GuestAddress{arg(1)}, bytes); else bytes[0] = static_cast<std::uint8_t>(arg(0));
            const auto written=write_standard_output(fd,bytes);if(written.error)crt_fail(written.error==109?32:9);else result=write?written.bytes:arg(0)&255;
        }
    } else if (symbol.starts_with("CreateMutex") || symbol.starts_with("OpenMutex")) {
        const bool create=symbol.starts_with("Create"),extended=symbol.starts_with("CreateMutexEx");
        bool valid=true,inherit=!create && arg(1);const auto named=arg(create && extended?1:2);
        auto name=named?(symbol.ends_with('W')?read_utf16(memory,GuestAddress{named}):utf8_to_utf16(read_ansi(memory,named))):std::u16string{};
        if(name.size()>=260){fail(206);valid=false;}
        if(name.starts_with(u"Local\\"))name.erase(0,6);
        const auto suffix=name.starts_with(u"Global\\")?std::u16string_view(name).substr(7):std::u16string_view(name);
        if(suffix.find(u'\\')!=suffix.npos){fail(123);valid=false;}
        if(!name.empty() && (std::any_of(runtime_.wait_states.begin(),runtime_.wait_states.end(),[&](const auto& item){return item.second.name==name;}) || std::any_of(runtime_.wait_objects.begin(),runtime_.wait_objects.end(),[&](const auto& item){return item.second.kind==WaitObject::Kind::Event && item.second.name==name;}))){fail(6);valid=false;}
        if(valid && runtime_.wait_objects.size()>=process_handle_limit){fail(8);valid=false;}
        if(extended && (dword(2)&~1U)){fail(87);valid=false;}
        if(!create && name.empty()){fail(87);valid=false;}
        if(valid) try {
            const auto existing=name.empty()?runtime_.mutex_objects.end():std::find_if(runtime_.mutex_objects.begin(),runtime_.mutex_objects.end(),[&](const auto& item){return item.second.name==name;});
            const auto requested=create?(extended?dword(3):0x1f0001U):dword(0);
            auto access=security::creation_access(requested,{0x20001U,0x20001U,0x120000U,0x1f0001U});
            const bool initial=create && (extended?(dword(2)&1U)!=0:dword(1)!=0);
            const auto handle=runtime_.allocate_wait_handle();bool local_new=existing==runtime_.mutex_objects.end();auto identity=local_new?handle:existing->first;
            bool created=local_new;MutexObject mutex;
            const auto factory=[&]{return synchronization_descriptor(memory,arg(0)).bytes;};
            if(create && arg(0)){const unsigned size=x86_?12:24;memory.check(GuestAddress{arg(0)},size,Access::Read);if(memory.load(GuestAddress{arg(0)},32)!=size)throw SecurityDescriptorError{87};inherit=memory.load(GuestAddress{arg(0)+(x86_?8:16)},32)!=0;}
            if(local_new){
                mutex.name=name;
                if(!name.empty()){
                    const auto opened=open_shared_mutex(mapping_session(),name,create,initial?threads_.current:0,1,create?ObjectSecurityFactory{factory}:ObjectSecurityFactory{});
                    if(opened.error)throw SecurityDescriptorError{opened.error};mutex.shared=opened.mutex;created=!opened.existed;
                    if(!created)access=synchronization_access(shared_object_security(mutex.shared->descriptor()),requested,0x1f0001U);
                    for(auto& [other,saved]:runtime_.mutex_objects)if(saved.shared && saved.shared->descriptor()==mutex.shared->descriptor()){local_new=false;identity=other;saved.name=name;break;}
                }else {mutex.owner=initial?threads_.current:0;mutex.recursion=initial?1U:0U;mutex.security_descriptor=factory();}
            }else{
                const auto& saved=existing->second;
                access=synchronization_access(saved.shared?shared_object_security(saved.shared->descriptor()):saved.security_descriptor,requested,0x1f0001U);
            }
            WaitObject object;object.kind=WaitObject::Kind::Mutex;object.mutex=identity;object.access=access;
            if(local_new)runtime_.mutex_objects.emplace(identity,std::move(mutex));
            try {runtime_.wait_objects.emplace(handle,object);if(inherit)runtime_.handle_flags.emplace(handle,1);}
            catch(...) {runtime_.wait_objects.erase(handle);if(local_new)runtime_.mutex_objects.erase(identity);throw;}
            result=handle;if(create)last_error_=created?0:183;
        }catch(const SecurityDescriptorError& error){fail(error.code);}
    } else if (symbol == "ReleaseMutex") {
        const auto handle=runtime_.wait_objects.find(arg(0));
        if(handle==runtime_.wait_objects.end() || handle->second.kind!=WaitObject::Kind::Mutex)fail(6);
        else if(!(handle->second.access&1U))fail(5);
        else {
            auto& mutex=runtime_.mutex_objects.at(handle->second.mutex);
            if(mutex.shared){SharedEventLock lock(mutex.shared->lock_descriptor());const auto error=mutex.shared->release_locked(threads_.current);if(error)fail(error);else result=1;}
            else if(mutex.owner!=threads_.current || !mutex.recursion)fail(288);
            else {if(!--mutex.recursion)mutex.owner=0;result=1;}
        }
    } else if(symbol.starts_with("CreateEvent") || symbol.starts_with("OpenEvent")) {
        const bool create=symbol.starts_with("Create"),extended=symbol.starts_with("CreateEventEx");bool valid=true,inherit=!create && arg(1);
        const auto name_at=arg(create?(extended?1:3):2);auto name=name_at?(symbol.ends_with('W')?read_utf16(memory,GuestAddress{name_at}):utf8_to_utf16(read_ansi(memory,name_at))):std::u16string{};
        if(name.starts_with(u"Local\\"))name.erase(0,6);
        if(!name.empty() && (std::any_of(runtime_.wait_states.begin(),runtime_.wait_states.end(),[&](const auto& item){return item.second.name==name;}) || std::any_of(runtime_.mutex_objects.begin(),runtime_.mutex_objects.end(),[&](const auto& item){return item.second.name==name;}))){fail(6);valid=false;}
        if(extended && (dword(2)&~3U)){fail(87);valid=false;}
        if(!create && name.empty()){fail(87);valid=false;}
        if(runtime_.wait_objects.size()>=process_handle_limit){fail(8);valid=false;}
        if(valid) try {
            const auto requested=create?(extended?dword(3):0x1f0003U):dword(0);
            auto access=security::creation_access(requested,{0x20001U,0x20002U,0x120000U,0x1f0003U});
            if(create && arg(0)){const unsigned size=x86_?12:24;memory.check(GuestAddress{arg(0)},size,Access::Read);if(memory.load(GuestAddress{arg(0)},32)!=size)throw SecurityDescriptorError{87};inherit=memory.load(GuestAddress{arg(0)+(x86_?8:16)},32)!=0;}
            WaitObject object;object.kind=WaitObject::Kind::Event;object.name=name;object.maximum=1;object.manual_reset=create && (extended?(dword(2)&1)!=0:arg(1)!=0);object.count=create && (extended?(dword(2)&2)!=0:arg(2)!=0)?1U:0U;object.access=access;
            if(!name.empty()) {
                const auto factory=[&]{return synchronization_descriptor(memory,arg(0)).bytes;};
                auto opened=open_shared_event(mapping_session(),std::move(name),create,object.manual_reset,object.count!=0,create?ObjectSecurityFactory{factory}:ObjectSecurityFactory{});
                if(opened.error){fail(opened.error);valid=false;}
                else {object.shared_event=std::move(opened.event);object.manual_reset=object.shared_event->manual_reset();if(opened.existed)object.access=synchronization_access(shared_object_security(object.shared_event->descriptor()),requested,0x1f0003U);if(create)last_error_=opened.existed?183:0;}
            }else object.security_descriptor=synchronization_descriptor(memory,arg(0)).bytes;
            if(valid){result=runtime_.allocate_wait_handle();runtime_.wait_objects.emplace(result,std::move(object));if(inherit)runtime_.handle_flags[result]=1;}
        }catch(const SecurityDescriptorError& error){fail(error.code);}
    } else if (symbol == "SetEvent" || symbol == "ResetEvent") {
        const auto found=runtime_.wait_objects.find(arg(0));
        if(found==runtime_.wait_objects.end() || (found->second.kind!=WaitObject::Kind::Event && found->second.kind!=WaitObject::Kind::MemoryResource))fail(6);
        else if(!(found->second.access&2U))fail(5);
        else {found->second.set_signal(symbol=="SetEvent");result=1;}
    } else if(symbol=="___lc_locale_name_func" || symbol=="__pctype_func") {
        return invoke_locale(state,memory,service);
    } else if(symbol=="iswctype" || symbol=="_isctype" || symbol=="wctype") {
        return invoke_crt_character(state,memory,service);
    } else if(symbol=="btowc" || symbol=="wctob" || symbol=="wcrtomb" || symbol=="wctomb" ||
              symbol=="mbrtowc" || symbol=="mbrlen" || symbol=="mbtowc" || symbol=="mblen" || symbol=="mbsinit") {
        return invoke_locale(state,memory,service);
    }
    else if(symbol=="_ismbblead" || symbol=="_ismbbtrail")return invoke_locale(state,memory,service);
    else if(symbol=="___mb_cur_max_func" || symbol=="__mb_cur_max_func" || symbol=="setlocale" || symbol=="_wsetlocale" || symbol=="localeconv" || symbol=="___lc_codepage_func" || symbol=="___lc_collate_cp_func" || symbol=="__lconv_init") {
        return invoke_locale(state,memory,service);
    } else if (symbol == "rand_s") {
        if (!arg(0)) { memory.store(GuestAddress{runtime_.errno_cell}, 32, 22); result = 22; }
        else {
            memory.check(GuestAddress{arg(0)}, 4, Access::Write);
            if (!fill_random(memory,arg(0),4)) {memory.store(GuestAddress{arg(0)},32,0);memory.store(GuestAddress{runtime_.errno_cell},32,12);result=12;}
        }
    } else if (symbol == "srand" || symbol == "rand") {
        static const bool diagnose = std::getenv("X64ARM_CRT_RANDOM_DIAGNOSTIC") != nullptr;
        if (symbol == "srand") runtime_.random_state = dword(0);
        else {
            runtime_.random_state = runtime_.random_state * 214013U + 2531011U;
            result = (runtime_.random_state >> 16) & 32767;
        }
        if (diagnose) std::fprintf(stderr, "[crt-random] function=%.*s caller=%llx state=%u value=%llu\n",
            static_cast<int>(symbol.size()), symbol.data(),
            static_cast<unsigned long long>(memory.load(GuestAddress{state.reg(Gpr::Rsp)}, x86_ ? 32 : 64)),
            runtime_.random_state, static_cast<unsigned long long>(result));
    }
    else if (symbol.starts_with("lstrcmp")) {
        const bool wide=symbol.ends_with('W'),ignore_case=symbol.starts_with("lstrcmpi");
        auto left=arg(0)?(wide?read_utf16(memory,GuestAddress{arg(0)}):utf8_to_utf16_lossy(read_ansi(memory,arg(0)))):std::u16string{};
        auto right=arg(1)?(wide?read_utf16(memory,GuestAddress{arg(1)}):utf8_to_utf16_lossy(read_ansi(memory,arg(1)))):std::u16string{};
        if(ignore_case){for(auto& c:left)c=unicode_case_unit(c,true);for(auto& c:right)c=unicode_case_unit(c,true);}
        result=left<right?UINT32_MAX:left>right?1:0;
    } else if ((symbol.starts_with("GetShortPathName") || symbol.starts_with("GetLongPathName"))) {
        const bool wide=symbol.ends_with('W');const auto path=wide?read_utf16(memory,GuestAddress{arg(0)}):utf8_to_utf16_lossy(read_ansi(memory,arg(0)));
        const auto info=filesystem_.information(utf16_to_utf8(path));
        if(info.error)fail(info.error);
        else {
            // 映射的宿主檔案系統沒有 DOS 8.3 別名，保留原始名稱。
            const auto ansi=utf16_to_utf8(path);result=(wide?path.size():ansi.size())+1;
            if(dword(2)>=result){if(wide)memory.write(GuestAddress{arg(1)},utf16_bytes(path));else{std::vector<std::uint8_t> data(ansi.begin(),ansi.end());data.push_back(0);memory.write(GuestAddress{arg(1)},data);}--result;}
        }
    }
    else if (symbol.starts_with("lstrcpyn")) {
        const auto count=std::bit_cast<std::int32_t>(dword(2));
        result=arg(0);
        if(count>0){
            if(count>1048576)throw HostCallFault("Win32 字串長度超過上限");
            const auto unit=symbol.ends_with('W')?2U:1U;
            std::vector<std::uint8_t> bytes;
            for(std::int32_t i=0;i<count-1;++i){
                const auto value=memory.load(GuestAddress{arg(1)+static_cast<std::uint64_t>(i)*unit},static_cast<std::uint8_t>(unit*8));
                if(!value)break;
                for(unsigned n=0;n<unit;++n)bytes.push_back(static_cast<std::uint8_t>(value>>(n*8)));
            }
            bytes.resize(bytes.size()+unit);memory.write(GuestAddress{arg(0)},bytes);
        }
    }
    else if (symbol.starts_with("lstrlen") || symbol.starts_with("lstrcpy") || symbol.starts_with("lstrcat")) {
        const bool wide=symbol.ends_with('W');
        if(symbol.starts_with("lstrlen"))result=!arg(0)?0:wide?read_utf16(memory,GuestAddress{arg(0)},1048575).size():read_ansi(memory,arg(0),1048575).size();
        else if(arg(0) && arg(1)) {
            if(wide) {
                auto text=read_utf16(memory,GuestAddress{arg(1)},1048575);
                if(symbol.starts_with("lstrcat"))text=read_utf16(memory,GuestAddress{arg(0)},1048575)+text;
                if(text.size()>1048575)throw HostCallFault("Win32 字串超過上限");
                memory.write(GuestAddress{arg(0)},utf16_bytes(text));
            }else {
                auto text=read_ansi(memory,arg(1),1048575);
                if(symbol.starts_with("lstrcat"))text=read_ansi(memory,arg(0),1048575)+text;
                if(text.size()>1048575)throw HostCallFault("Win32 字串超過上限");
                std::vector<std::uint8_t> bytes(text.begin(),text.end());bytes.push_back(0);memory.write(GuestAddress{arg(0)},bytes);
            }
            result=arg(0);
        }
    } else if (symbol == "wcslen" || symbol == "wcsnlen" || symbol == "strnlen") {
        const unsigned width = symbol == "strnlen" ? 1 : 2;
        const auto limit = symbol == "wcslen" ? UINT64_C(1048576) : arg(1);
        if (limit > 1048576 || limit > (UINT64_MAX - arg(0)) / width) throw HostCallFault("CRT 字串範圍超過上限");
        for (; result < limit; ++result) if (!memory.load(GuestAddress{arg(0) + result * width}, static_cast<std::uint8_t>(width * 8))) break;
        if (symbol == "wcslen" && result == limit) throw HostCallFault("wcslen 找不到終止字元");
    } else if (symbol == "memchr") {
        if (arg(2) > memory.limit() || arg(2) > UINT64_MAX - arg(0)) throw HostCallFault("memchr 範圍超過上限");
        for (std::uint64_t i = 0; i < arg(2); ++i) if (memory.load(GuestAddress{arg(0) + i}, 8) == (arg(1) & 255)) { result = arg(0) + i; break; }
    } else if (symbol == "_strdup" || symbol == "__std_exception_copy" || symbol == "__std_exception_destroy") {
        if(symbol=="_strdup")result=crt_duplicate_string(memory,arg(0));
        else {crt_exception_data(state,memory,arg(0),symbol=="__std_exception_copy"?arg(1):arg(0),symbol=="__std_exception_destroy");result=0;}
    } else if (symbol == "_strnicmp" || symbol=="_stricmp" || symbol=="_strcmpi" || symbol=="_wcsicmp" || symbol=="_wcsnicmp") {
        const bool bounded=symbol=="_strnicmp" || symbol=="_wcsnicmp",wide=symbol.starts_with("_wcs");
        const auto limit=bounded?arg(2):UINT64_C(1048576);const unsigned unit=wide?2:1;
        if(limit && (!arg(0) || !arg(1))) {
            memory.store(GuestAddress{runtime_.errno_cell},32,22);
            if(current_invalid_parameter_handler()){start_callbacks(state,memory,{{current_invalid_parameter_handler(),{0,0,0,0,0}}},Continuation::Return,INT32_MAX);return std::nullopt;}
            return 0xc0000417U;
        }
        if(limit>1048576 || limit>(UINT64_MAX-std::max(arg(0),arg(1)))/unit)throw HostCallFault("字串比較範圍超過上限");
        bool ended=bounded;
        for(std::uint64_t i=0;i<limit;++i){
            auto a=memory.load(GuestAddress{arg(0)+i*unit},static_cast<std::uint8_t>(unit*8));
            auto b=memory.load(GuestAddress{arg(1)+i*unit},static_cast<std::uint8_t>(unit*8));
            // 與 setlocale 接受的 C locale 一致；高位元組／UTF-16 非 ASCII 字元保持原值。
            if(a>='A' && a<='Z')a+=32;if(b>='A' && b<='Z')b+=32;
            if(a!=b){result=static_cast<std::uint32_t>(static_cast<int>(a)-static_cast<int>(b));ended=true;break;}
            if(!a){ended=true;break;}
        }
        if(!ended)throw HostCallFault("字串比較找不到終止字元");
    } else if (symbol == "strspn" || symbol == "strcspn") {
        const auto text = read_ansi(memory, arg(0)), accept = read_ansi(memory, arg(1));
        result = symbol=="strspn"?text.find_first_not_of(accept):text.find_first_of(accept); if (result == std::string::npos) result = text.size();
    } else if(crt_character_operation(service)) {
        return invoke_crt_character(state,memory,service);
    } else if (symbol == "_fileno") {
        const auto p = crt_stream(arg(0)), begin = runtime_base_.value + 0x800;
        if (p >= begin && p <= begin + 0x100 && (p - begin) % 0x80 == 0 && standard_open_[(p - begin) / 0x80]) result = (p - begin) / 0x80;
        else if (runtime_.streams.contains(p)) result=runtime_.streams.at(p).descriptor;
        else crt_fail(9);
    } else if (symbol == "_isatty" || symbol == "_get_osfhandle" || symbol == "_setmode") {
        const auto fd = dword(0);
        if (fd >= 3 || !standard_open_[fd]) { crt_fail(9); if (symbol == "_isatty") result = 0; else if (symbol == "_get_osfhandle") result = UINT64_MAX; }
        else if (symbol == "_get_osfhandle") result = 0x100 + fd;
        else if (symbol == "_isatty") result = 0; // 標準串流為收集管線，沒有終端。
        else if (dword(1) != 0x4000 && dword(1) != 0x8000 && dword(1) != 0x10000 && dword(1) != 0x20000 && dword(1) != 0x40000) crt_fail(22);
        else { result = runtime_.descriptor_modes[fd]; runtime_.descriptor_modes[fd] = dword(1); }
    } else if (symbol.starts_with("CharToOem") || symbol.starts_with("OemToChar")) {
        const bool wide=symbol.ends_with('W'),to_oem=symbol.starts_with("CharToOem");
        const bool counted=symbol.find("Buff")!=std::string_view::npos;
        if(wide && arg(0)==arg(1)){fail(487);}
        else if(!arg(0) || !arg(1)){fail(87);}
        else {
            // 與 GetACP/GetOEMCP 及 CP_ACP/CP_OEMCP 相同的虛擬 UTF-8 碼頁。
            // 先完成輸入快照，ANSI 同址或部分重疊時也不會破壞未讀資料。
            const auto count=counted?dword(2):0U;
            if(count>1048576)throw HostCallFault("OEM 轉換超過上限");
            std::vector<std::uint8_t> output;
            std::size_t capacity=count;
            if(wide && to_oem){
                auto text=counted?read_utf16_counted(memory,GuestAddress{arg(0)},count):read_utf16(memory,GuestAddress{arg(0)});
                if(!counted)text.push_back(0);
                capacity=text.size();
                try {const auto bytes=utf16_to_utf8(text);output.assign(bytes.begin(),bytes.end());}
                catch(const std::invalid_argument&){last_error_=1113;arg.return_to(state,1);return std::nullopt;}
            }else{
                std::string bytes;
                if(counted){bytes.resize(count);memory.read(GuestAddress{arg(0)},{reinterpret_cast<std::uint8_t*>(bytes.data()),bytes.size()});}
                else {bytes=read_ansi(memory,arg(0));bytes.push_back(0);}
                if(wide){const auto text=utf8_to_utf16_lossy(bytes);output=utf16_bytes(text);output.resize(text.size()*2);}
                else {
                    capacity=bytes.size();
                    const auto converted=utf16_to_utf8(utf8_to_utf16_lossy(bytes));
                    output.assign(converted.begin(),converted.end());
                }
            }
            // 此舊 API 的 OEM 目的長度與輸入字元數相同；UTF-8 展開不得越界。
            // BOOL 依契約表示有效參數，底層轉碼失敗仍透過 LastError 保留。
            if((to_oem || !wide) && output.size()>capacity)last_error_=122;
            else memory.write(GuestAddress{arg(1)},output);
            result=1;
        }
    } else if (symbol == "WideCharToMultiByte" || symbol == "MultiByteToWideChar") {
        const bool wide = symbol == "WideCharToMultiByte";
        const auto count = sint(3), capacity = sint(5); const auto codepage = dword(0),flags=dword(1);
        const auto page=codepage==0 || codepage==1 || codepage==3?65001U:codepage;
        const auto page_info=windows_crt_codepage(page);
        const bool legacy_composition=codepage==0 || codepage==3 || codepage==1252;
        const bool multibyte=page!=65001 && page!=1252;
        const auto allowed_flags=wide?(codepage==1252?0x480U:multibyte && page!=54936?0x400U:0x80U):(legacy_composition?0xfU:multibyte && page!=54936?0xbU:8U);
        if (!page_info) fail(87);
        else if((flags&~allowed_flags) || (!wide && (flags&3)==3))fail(1004);
        else if (count == 0 || count < -1 || capacity < 0 || !arg(2) || (!arg(4) && capacity) || arg(2)==arg(4)) fail(87);
        // CP_UTF8 的明確參數限制不套用到 CP_ACP／CP_OEMCP 別名。
        else if (wide && codepage==65001 && (arg(6) || arg(7))) fail(87);
        else {
            std::vector<std::uint8_t> output; std::size_t required = 0;
            try {
                if (wide) {
                    std::u16string text;
                    if (count == -1) { text = read_utf16(memory, GuestAddress{arg(2)}); text.push_back(0); }
                    else {
                        if (count > 1048576) throw HostCallFault("UTF-16 轉換超過上限");
                        text=read_utf16_counted(memory,GuestAddress{arg(2)},static_cast<std::size_t>(count));
                    }
                    bool used_default=false;
                    if(multibyte){
                        std::string replacement(1,arg(6)?static_cast<char>(memory.load(GuestAddress{arg(6)},8)):'?');
                        if(arg(6) && page_info->lead[static_cast<unsigned char>(replacement[0])])replacement.push_back(static_cast<char>(memory.load(GuestAddress{arg(6)+1},8)));
                        auto converted=windows_nls_encode(text,page,(flags&0x400U)!=0,replacement,(flags&0x80U)!=0);if(!converted)throw std::invalid_argument("無法轉換指定碼頁");
                        output=std::move(converted->bytes);used_default=converted->used_default;
                    }else{const auto bytes=codepage==1252?utf16_to_cp1252(text,arg(6)?static_cast<char>(memory.load(GuestAddress{arg(6)},8)):'?',used_default):utf16_to_utf8(text);output.assign(bytes.begin(),bytes.end());}
                    if(codepage!=65001 && arg(7))memory.store(GuestAddress{arg(7)},32,used_default?1:0);required=output.size();
                } else {
                    std::string text;
                    if (count == -1) { text = read_ansi(memory, arg(2)); text.push_back(0); }
                    else {
                        if (count > 1048576) throw HostCallFault("UTF-8 轉換超過上限");
                        text.resize(static_cast<std::size_t>(count));
                        memory.read(GuestAddress{arg(2)},std::span(reinterpret_cast<std::uint8_t*>(text.data()),text.size()));
                    }
                    std::u16string converted;
                    if(multibyte){auto value=windows_nls_decode(text,page,(flags&8)!=0);if(!value)throw std::invalid_argument("無效的多位元組字元");converted=std::move(*value);}
                    else converted=codepage==1252?cp1252_to_utf16(text):(flags&8)?utf8_to_utf16(text):utf8_to_utf16_lossy(text);
                    // 相容延伸：虛擬 UTF-8 ACP/THREAD_ACP 仍接受舊 CRT 的組合旗標。
                    // 明確 CP_UTF8 保持僅 0/MB_ERR_INVALID_CHARS；不忽略 NFC/NFD 語意。
                    // CP1252 的映射字元已是 NFC；ACP 的既有 flags=0/8 保留 UTF-8 碼點。
                    if((codepage==1252 && (flags&2)) || ((multibyte || codepage==0 || codepage==3) && (flags&3))){auto normalized=normalize_utf16(converted,(flags&2)==0);if(!normalized){fail(50);arg.return_to(state,0);return std::nullopt;}converted=std::move(*normalized);}
                    required = converted.size(); output = utf16_bytes(converted); output.resize(required * 2);
                }
            } catch (const std::invalid_argument&) { fail(1113); arg.return_to(state, 0); return std::nullopt; }
            if (!capacity) result = required;
            else if (static_cast<std::size_t>(capacity) < required) fail(122);
            else { memory.write(GuestAddress{arg(4)}, output); result = required; }
        }
    } else if (symbol == "AreFileApisANSI") result = 1;
    else if(symbol=="IsDBCSLeadByteEx"){
        const auto page=dword(0)==0 || dword(0)==1 || dword(0)==3?65001U:dword(0);const auto info=windows_crt_codepage(page);
        if(!info)fail(87);else result=info->lead[static_cast<std::uint8_t>(arg(1))];
    }
    else if (symbol == "IsWow64Process" || symbol == "IsWow64Process2") {
        std::array<std::uint64_t,5> information{};
        std::uint32_t pid{},target_bits{};
        const auto error=process_basic_information(memory,arg(0),information,pid,target_bits);
        if(error)fail(error);
        else if(!arg(1))fail(87);
        else {
            // 沿程序查詢共用權限與生命週期；架構屬於目標程序，不能取呼叫者的 ABI。
            // 虛擬 Windows 系統為 AMD64，pNativeMachine 是可選輸出。
            if(symbol=="IsWow64Process")memory.store(GuestAddress{arg(1)},32,target_bits==32?1:0);
            else {
                memory.check(GuestAddress{arg(1)},2,Access::Write);
                if(arg(2))memory.check(GuestAddress{arg(2)},2,Access::Write);
                memory.store(GuestAddress{arg(1)},16,target_bits==32?0x14c:0);
                if(arg(2))memory.store(GuestAddress{arg(2)},16,0x8664);
            }
            result=1;
        }
    }
    else if (symbol == "GetCurrentProcess") result = UINT64_MAX;
    else if (symbol == "GetCurrentThread") result = UINT64_MAX - 1;
    else if(symbol=="GetConsoleMode" || symbol=="SetConsoleMode" || symbol=="SetStdHandle" || symbol.starts_with("ReadConsole") || symbol.starts_with("WriteConsole"))return invoke_console(state,memory,service);
    else if (symbol == "GetConsoleCP" || symbol == "GetConsoleOutputCP") result=65001;
    else if (symbol == "GetFileType" || symbol == "GetHandleInformation" || symbol=="SetHandleInformation") {
        const auto handle = arg(0);
        const bool standard = handle >= 0x100 && handle < 0x103 && standard_open_[handle - 0x100];
        const bool socket=runtime_.network.sockets.contains(handle);
        const bool valid=wait_completion_handles_.contains(handle) || bluetooth_handle(handle) || power_requests_.contains(handle) || transaction_handle(handle) || job_handle(handle) || mapping_handle(handle) || process_signaled(handle).has_value() || standard || files_.contains(handle) || socket || runtime_.wait_objects.contains(handle) || runtime_.process_thread_handles.contains(handle) || runtime_.completion_ports.contains(handle) || runtime_.process_snapshots.contains(handle) || runtime_.token_handles.contains(handle);
        if (!valid) fail(6);
        else if (symbol == "GetFileType") result = standard?standard_file_type(static_cast<unsigned>(handle-0x100)):bluetooth_handle(handle) || console_handle(handle) || (files_.contains(handle) && (files_.at(handle).serial || files_.at(handle).hid || (files_.at(handle).native && files_.at(handle).native->null_device())))?2:socket || (files_.contains(handle) && files_.at(handle).pipe) ? 3 : files_.contains(handle)?1:0;
        else if(symbol=="SetHandleInformation"){
            if((dword(1)|dword(2))&~3U)fail(87);
            else {auto& flags=runtime_.handle_flags[handle];flags=(flags&~dword(1))|(dword(2)&dword(1));result=1;}
        }else { memory.store(GuestAddress{arg(1)}, 32, runtime_.handle_flags[handle]); result = 1; }
    } else if (symbol == "IsDebuggerPresent") result = debugger_attached() || process_debugger::self(memory,peb_base_,runtime_.network.external_io).present;
    else if(symbol=="IsProcessorFeaturePresent")result=virtual_processor_feature(static_cast<std::uint32_t>(arg(0)),x86_);
    else if(symbol=="GetEnabledXStateFeatures")result=virtual_xstate_mask;
    else if(symbol=="RtlGetEnabledExtendedFeatures")result=virtual_xstate_mask&arg(0);
    else if (symbol == "QueryPerformanceCounter" || symbol == "QueryPerformanceFrequency") {
        memory.store(GuestAddress{arg(0)}, 64, symbol == "QueryPerformanceFrequency" ? 10000000 : runtime_.clock_ticks);
        result = 1;
    } else if (symbol == "GetSystemTimeAsFileTime") {
        memory.store(GuestAddress{arg(0)}, 64, runtime_.system_filetime());
    }
    else if (symbol == "GetTickCount") result = static_cast<std::uint32_t>(runtime_.clock_ticks / 10000);
    else throw HostCallFault("尚未實作相容服務：" + std::string(symbol));
    arg.return_to(state, result,symbol=="VerSetConditionMask" || symbol=="GetEnabledXStateFeatures" || symbol=="RtlGetEnabledExtendedFeatures"); return std::nullopt;
}
}
