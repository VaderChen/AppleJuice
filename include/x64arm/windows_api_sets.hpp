#pragma once
#include <string_view>

namespace x64arm {
// API-set 合約依「家族」對應到本專案承接該家族的模組。Windows 的 apisetschema 多半把合約指向 kernelbase.dll，
// 但本專案把 kernelbase 的函式依家族分散在 kernel32／advapi32／version／ole32 等服務，因此必要時拆解
// api-ms-win-core-*：比對時先去掉 "-l<主>-<次>-<修>.dll" 版本尾碼，再查家族表。
// 家族名以 '-' 結尾者為前綴（涵蓋同族子合約）。未列出的 api-ms-win-core-* 仍由 kernel32 承接；
// 其他未列出的合約不冒用任何 DLL。Loader 模組辨識、函式解析、版本資源與 IsApiSetImplemented 共用本表。
struct WindowsApiSetFamily {std::string_view family,host;};
inline constexpr WindowsApiSetFamily windows_api_set_families[]{
    // kernelbase 拆解：登錄、版本資源、COM、路徑、URL／字串雜項各歸原家族。
    {"api-ms-win-core-registry","advapi32.dll"},{"api-ms-win-core-localregistry","advapi32.dll"},
    {"api-ms-win-core-version","version.dll"},
    {"api-ms-win-core-com","ole32.dll"},{"api-ms-win-core-com-private","ole32.dll"},{"api-ms-win-core-com-midlproxystub","ole32.dll"},
    {"api-ms-win-core-path","pathcch.dll"},
    {"api-ms-win-core-shlwapi-","shlwapi.dll"},{"api-ms-win-core-url","shlwapi.dll"},
    {"api-ms-win-core-winrt-","combase.dll"},{"api-ms-win-core-winrt","combase.dll"},
    // 安全、事件、服務：advapi32（Windows 由 sechost／kernelbase 承接）。
    {"api-ms-win-security-","advapi32.dll"},{"api-ms-win-eventing-","advapi32.dll"},{"api-ms-win-service-","advapi32.dll"},
    {"api-ms-win-power-base","powrprof.dll"},{"api-ms-win-power-setting","powrprof.dll"},
    {"api-ms-win-devices-config","cfgmgr32.dll"},
    {"api-ms-win-shcore-","shcore.dll"},
    {"api-ms-win-downlevel-kernel32","kernel32.dll"},{"api-ms-win-downlevel-shell32","shell32.dll"},
    {"api-ms-win-downlevel-advapi32","advapi32.dll"},{"api-ms-win-downlevel-user32","user32.dll"},
    {"api-ms-win-downlevel-ole32","ole32.dll"},{"api-ms-win-downlevel-version","version.dll"},
    {"api-ms-win-downlevel-shlwapi","shlwapi.dll"},
    {"api-ms-win-appmodel-runtime","kernel32.dll"},
    // ext-ms-win：依子系統家族。
    {"ext-ms-win-ntuser-","user32.dll"},{"ext-ms-win-rtcore-ntuser-","user32.dll"},
    {"ext-ms-win-gdi-","gdi32.dll"},{"ext-ms-win-rtcore-gdi-","gdi32.dll"},
    {"ext-ms-win-kernel32-","kernel32.dll"},{"ext-ms-win-advapi32-","advapi32.dll"},
    {"ext-ms-win-shell32-","shell32.dll"},{"ext-ms-win-ole32-","ole32.dll"},
};
// 合約名稱（小寫、含 .dll）去掉版本尾碼後的家族名；不是 "<名>-l<數>-<數>-<數>.dll" 形式時回傳原名。
constexpr std::string_view windows_api_set_family(std::string_view name){
    if(!name.ends_with(".dll"))return name;
    auto stem=name.substr(0,name.size()-4);
    for(int part=0;part<3;++part){
        const auto dash=stem.rfind('-');if(dash==stem.npos || dash+1==stem.size())return name;
        const auto digits=stem.substr(dash+1);
        const bool last=part==2;const auto body=last?digits.substr(1):digits;
        if(last && digits.front()!='l')return name;
        if(body.empty())return name;
        for(const auto c:body)if(c<'0' || c>'9')return name;
        stem=stem.substr(0,dash);
    }
    return stem;
}
constexpr std::string_view windows_api_set_host(std::string_view name){
    if(name=="kernelbase.dll")return "kernel32.dll";
    if(!name.starts_with("api-ms-win-") && !name.starts_with("ext-ms-win-"))return name;
    const auto family=windows_api_set_family(name);
    for(const auto& entry:windows_api_set_families){
        const bool prefix=entry.family.ends_with('-');
        const auto base=prefix?entry.family.substr(0,entry.family.size()-1):entry.family;
        if(family==base || (prefix && family.starts_with(entry.family)))return entry.host;
    }
    if(name.starts_with("api-ms-win-core-"))return "kernel32.dll";
    return name;
}
// 家族拆解的編譯期契約：版本尾碼、kernelbase 拆解、前綴子合約與未列出合約。
static_assert(windows_api_set_family("api-ms-win-core-registry-l1-1-0.dll")=="api-ms-win-core-registry");
static_assert(windows_api_set_family("ucrtbase.dll")=="ucrtbase.dll");
static_assert(windows_api_set_host("api-ms-win-core-registry-l1-1-0.dll")=="advapi32.dll");
static_assert(windows_api_set_host("api-ms-win-core-version-l1-1-1.dll")=="version.dll");
static_assert(windows_api_set_host("api-ms-win-core-com-l1-1-0.dll")=="ole32.dll");
static_assert(windows_api_set_host("api-ms-win-core-winrt-string-l1-1-0.dll")=="combase.dll");
static_assert(windows_api_set_host("api-ms-win-security-lsalookup-ansi-l2-1-0.dll")=="advapi32.dll");
static_assert(windows_api_set_host("api-ms-win-core-synch-l1-2-0.dll")=="kernel32.dll");
static_assert(windows_api_set_host("ext-ms-win-ntuser-powermanagement-l1-1-0.dll")=="user32.dll");
static_assert(windows_api_set_host("api-ms-win-crt-runtime-l1-1-0.dll")=="api-ms-win-crt-runtime-l1-1-0.dll");
static_assert(windows_api_set_host("ext-ms-win-dxcore-l1-1-0.dll")=="ext-ms-win-dxcore-l1-1-0.dll");
}
